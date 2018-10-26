// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <algorithm>
#include <iomanip>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <mesos/attributes.hpp>
#include <mesos/type_utils.hpp>

#include <mesos/authorizer/authorizer.hpp>

#include <mesos/maintenance/maintenance.hpp>

#include <mesos/master/master.hpp>

#include <mesos/v1/master/master.hpp>

#include <process/async.hpp>
#include <process/collect.hpp>
#include <process/defer.hpp>
#include <process/future.hpp>
#include <process/help.hpp>
#include <process/logging.hpp>

#include <process/metrics/metrics.hpp>

#include <stout/base64.hpp>
#include <stout/errorbase.hpp>
#include <stout/foreach.hpp>
#include <stout/hashmap.hpp>
#include <stout/hashset.hpp>
#include <stout/json.hpp>
#include <stout/jsonify.hpp>
#include <stout/lambda.hpp>
#include <stout/net.hpp>
#include <stout/none.hpp>
#include <stout/nothing.hpp>
#include <stout/numify.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/protobuf.hpp>
#include <stout/representation.hpp>
#include <stout/result.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>
#include <stout/unreachable.hpp>
#include <stout/utils.hpp>
#include <stout/uuid.hpp>

#include "common/http.hpp"
#include "common/protobuf_utils.hpp"
#include "common/recordio.hpp"
#include "common/resources_utils.hpp"

#include "internal/devolve.hpp"

#include "logging/logging.hpp"

#include "master/machine.hpp"
#include "master/maintenance.hpp"
#include "master/master.hpp"
#include "master/registry_operations.hpp"
#include "master/validation.hpp"

#include "mesos/mesos.hpp"
#include "mesos/resources.hpp"

#include "version/version.hpp"

using google::protobuf::RepeatedPtrField;

using process::AUTHENTICATION;
using process::AUTHORIZATION;
using process::Clock;
using process::DESCRIPTION;
using process::Failure;
using process::Future;
using process::HELP;
using process::Logging;
using process::Promise;
using process::TLDR;

using process::http::Accepted;
using process::http::BadRequest;
using process::http::Conflict;
using process::http::Forbidden;
using process::http::InternalServerError;
using process::http::MethodNotAllowed;
using process::http::NotFound;
using process::http::NotImplemented;
using process::http::NotAcceptable;
using process::http::OK;
using process::http::Pipe;
using process::http::ServiceUnavailable;
using process::http::TemporaryRedirect;
using process::http::UnsupportedMediaType;
using process::http::URL;

using process::http::authentication::Principal;

using std::copy_if;
using std::list;
using std::map;
using std::set;
using std::shared_ptr;
using std::string;
using std::tie;
using std::tuple;
using std::vector;

using mesos::authorization::createSubject;
using mesos::authorization::GET_MAINTENANCE_SCHEDULE;
using mesos::authorization::GET_MAINTENANCE_STATUS;
using mesos::authorization::MARK_AGENT_GONE;
using mesos::authorization::SET_LOG_LEVEL;
using mesos::authorization::START_MAINTENANCE;
using mesos::authorization::STOP_MAINTENANCE;
using mesos::authorization::UPDATE_MAINTENANCE_SCHEDULE;
using mesos::authorization::VIEW_EXECUTOR;
using mesos::authorization::VIEW_FLAGS;
using mesos::authorization::VIEW_FRAMEWORK;
using mesos::authorization::VIEW_ROLE;
using mesos::authorization::VIEW_TASK;

using mesos::internal::recordio::Reader;

using ::recordio::Decoder;

namespace mesos {
namespace internal {
namespace master {

// Pull in model overrides from common.
using mesos::internal::model;

// Pull in definitions from process.
using process::http::Response;
using process::http::Request;
using process::Owned;


string Master::Http::API_HELP()
{
  return HELP(
    TLDR(
        "Endpoint for API calls against the master."),
    DESCRIPTION(
        "Returns 200 OK when the request was processed successfully.",
        "",
        "Returns 307 TEMPORARY_REDIRECT redirect to the leading master when",
        "current master is not the leader.",
        "",
        "Returns 503 SERVICE_UNAVAILABLE if the leading master cannot be",
        "found."),
    AUTHENTICATION(true),
    AUTHORIZATION(
        "The information returned by this endpoint for certain calls",
        "might be filtered based on the user accessing it.",
        "For example a user might only see the subset of frameworks,",
        "tasks, and executors they are allowed to view.",
        "See the authorization documentation for details."));
}


Future<Response> Master::Http::api(
    const Request& request,
    const Option<Principal>& principal) const
{
  // TODO(greggomann): Remove this check once the `Principal` type is used in
  // `ReservationInfo`, `DiskInfo`, and within the master's `principals` map.
  // See MESOS-7202.
  if (principal.isSome() && principal->value.isNone()) {
    return Forbidden(
        "The request's authenticated principal contains claims, but no value "
        "string. The master currently requires that principals have a value");
  }

  // TODO(vinod): Add metrics for rejected requests.

  // TODO(vinod): Add support for rate limiting.

  // When current master is not the leader, redirect to the leading master.
  // Note that this could happen when an operator, or some other
  // service, including a scheduler realizes this is the leading
  // master before the master itself realizes it, e.g., due to a
  // ZooKeeper watch delay.
  if (!master->elected()) {
    return redirect(request);
  }

  CHECK_SOME(master->recovered);

  if (!master->recovered->isReady()) {
    return ServiceUnavailable("Master has not finished recovery");
  }

  if (request.method != "POST") {
    return MethodNotAllowed({"POST"}, request.method);
  }

  // TODO(anand): Content type values are case-insensitive.
  Option<string> contentType_ = request.headers.get("Content-Type");
  if (contentType_.isNone()) {
    return BadRequest("Expecting 'Content-Type' to be present");
  }

  ContentType contentType;
  if (contentType_.get() == APPLICATION_JSON) {
    contentType = ContentType::JSON;
  } else if (contentType_.get() == APPLICATION_PROTOBUF) {
    contentType = ContentType::PROTOBUF;
  } else if (contentType_.get() == APPLICATION_RECORDIO) {
    contentType = ContentType::RECORDIO;
  } else {
    return UnsupportedMediaType(
        string("Expecting 'Content-Type' of ") +
        APPLICATION_JSON + " or " + APPLICATION_PROTOBUF +
        + " or " + APPLICATION_RECORDIO);
  }

  Option<ContentType> messageContentType;
  Option<string> messageContentType_ =
    request.headers.get(MESSAGE_CONTENT_TYPE);

  if (streamingMediaType(contentType)) {
    if (messageContentType_.isNone()) {
      return BadRequest(
          "Expecting '" + stringify(MESSAGE_CONTENT_TYPE) + "' to be" +
          " set for streaming requests");
    }

    if (messageContentType_.get() == APPLICATION_JSON) {
      messageContentType = Option<ContentType>(ContentType::JSON);
    } else if (messageContentType_.get() == APPLICATION_PROTOBUF) {
      messageContentType = Option<ContentType>(ContentType::PROTOBUF);
    } else {
      return UnsupportedMediaType(
          string("Expecting '") + MESSAGE_CONTENT_TYPE + "' of " +
          APPLICATION_JSON + " or " + APPLICATION_PROTOBUF);
    }
  } else {
    // Validate that a client has not set the "Message-Content-Type"
    // header for a non-streaming request.
    if (messageContentType_.isSome()) {
      return UnsupportedMediaType(
          string("Expecting '") + MESSAGE_CONTENT_TYPE + "' to be not"
          " set for non-streaming requests");
    }
  }

  // This lambda deserializes a string into a valid `Call`
  // based on the content type.
  auto deserializer = [](const string& body, ContentType contentType)
      -> Try<mesos::master::Call> {
    Try<v1::master::Call> v1Call =
      deserialize<v1::master::Call>(contentType, body);

    if (v1Call.isError()) {
      return Error(v1Call.error());
    }

    mesos::master::Call call = devolve(v1Call.get());

    Option<Error> error = validation::master::call::validate(call);
    if (error.isSome()) {
      return Error("Failed to validate master::Call: " + error->message);
    }

    return call;
  };

  ContentType acceptType;
  if (request.acceptsMediaType(APPLICATION_JSON)) {
    acceptType = ContentType::JSON;
  } else if (request.acceptsMediaType(APPLICATION_PROTOBUF)) {
    acceptType = ContentType::PROTOBUF;
  } else if (request.acceptsMediaType(APPLICATION_RECORDIO)) {
    acceptType = ContentType::RECORDIO;
  } else {
    return NotAcceptable(
        string("Expecting 'Accept' to allow ") +
        APPLICATION_JSON + " or " + APPLICATION_PROTOBUF + " or " +
        APPLICATION_RECORDIO);
  }

  Option<ContentType> messageAcceptType;
  if (streamingMediaType(acceptType)) {
    // Note that `acceptsMediaType()` returns true if the given headers
    // field does not exist, i.e. by default we return JSON here.
    if (request.acceptsMediaType(MESSAGE_ACCEPT, APPLICATION_JSON)) {
      messageAcceptType = ContentType::JSON;
    } else if (request.acceptsMediaType(MESSAGE_ACCEPT, APPLICATION_PROTOBUF)) {
      messageAcceptType = ContentType::PROTOBUF;
    } else {
      return NotAcceptable(
          string("Expecting '") + MESSAGE_ACCEPT + "' to allow " +
          APPLICATION_JSON + " or " + APPLICATION_PROTOBUF);
    }
  } else {
    // Validate that a client has not set the "Message-Accept"
    // header for a non-streaming response.
    if (request.headers.contains(MESSAGE_ACCEPT)) {
      return NotAcceptable(
          string("Expecting '") + MESSAGE_ACCEPT +
          "' to be not set for non-streaming responses");
    }
  }

  CHECK_EQ(Request::PIPE, request.type);
  CHECK_SOME(request.reader);

  RequestMediaTypes mediaTypes {
      contentType, acceptType, messageContentType, messageAcceptType};

  if (streamingMediaType(contentType)) {
    CHECK_SOME(mediaTypes.messageContent);

    shared_ptr<Reader<mesos::master::Call>> reader(
        new Reader<mesos::master::Call>(
            Decoder<mesos::master::Call>(lambda::bind(
                deserializer, lambda::_1, mediaTypes.messageContent.get())),
            request.reader.get()));

    return reader->read()
      .then(defer(
          master->self(),
          [=](const Result<mesos::master::Call>& call) -> Future<Response> {
            if (call.isNone()) {
              return BadRequest("Received EOF while reading request body");
            }

            if (call.isError()) {
              return BadRequest(call.error());
            }

            return _api(call.get(), reader, mediaTypes, principal);
          }));
  } else {
    Pipe::Reader reader = request.reader.get();  // Remove const.

    return reader.readAll()
      .then(defer(
          master->self(),
          [=](const string& body) -> Future<Response> {
            Try<mesos::master::Call> call = deserializer(body, contentType);
            if (call.isError()) {
              return BadRequest(call.error());
            }
            return _api(call.get(), None(), mediaTypes, principal);
          }));
  }
}


Future<Response> Master::Http::_api(
    const mesos::master::Call& call,
    Option<shared_ptr<Reader<mesos::master::Call>>> reader,
    const RequestMediaTypes& mediaTypes,
    const Option<Principal>& principal) const
{
  // Validate that a client has not _accidentally_ sent us a
  // streaming request for a call type that does not support it.
  if (streamingMediaType(mediaTypes.content) &&
      call.type() != mesos::master::Call::SUBSCRIBE) {
    return UnsupportedMediaType(
        "Streaming 'Content-Type' " + stringify(mediaTypes.content) + " is "
        "not supported for " + stringify(call.type()) + " call");
  }

  if (streamingMediaType(mediaTypes.accept) &&
      call.type() != mesos::master::Call::SUBSCRIBE) {
    return NotAcceptable("Streaming response is not supported for " +
        stringify(call.type()) + " call");
  }

  LOG(INFO) << "Processing call " << call.type();

  switch (call.type()) {
    case mesos::master::Call::UNKNOWN:
      return NotImplemented();

    case mesos::master::Call::GET_HEALTH:
      return getHealth(call, principal, mediaTypes.accept);

    case mesos::master::Call::GET_FLAGS:
      return getFlags(call, principal, mediaTypes.accept);

    case mesos::master::Call::GET_VERSION:
      return getVersion(call, principal, mediaTypes.accept);

    case mesos::master::Call::GET_METRICS:
      return getMetrics(call, principal, mediaTypes.accept);

    case mesos::master::Call::GET_LOGGING_LEVEL:
      return getLoggingLevel(call, principal, mediaTypes.accept);

    case mesos::master::Call::SET_LOGGING_LEVEL:
      return setLoggingLevel(call, principal, mediaTypes.accept);

    case mesos::master::Call::LIST_FILES:
      return listFiles(call, principal, mediaTypes.accept);

    case mesos::master::Call::READ_FILE:
      return readFile(call, principal, mediaTypes.accept);

    case mesos::master::Call::GET_STATE:
      return getState(call, principal, mediaTypes.accept);

    case mesos::master::Call::GET_AGENTS:
      return getAgents(call, principal, mediaTypes.accept);

    case mesos::master::Call::GET_FRAMEWORKS:
      return getFrameworks(call, principal, mediaTypes.accept);

    case mesos::master::Call::GET_EXECUTORS:
      return getExecutors(call, principal, mediaTypes.accept);

    case mesos::master::Call::GET_OPERATIONS:
      return getOperations(call, principal, mediaTypes.accept);

    case mesos::master::Call::GET_TASKS:
      return getTasks(call, principal, mediaTypes.accept);

    case mesos::master::Call::GET_ROLES:
      return getRoles(call, principal, mediaTypes.accept);

    case mesos::master::Call::GET_WEIGHTS:
      return weightsHandler.get(call, principal, mediaTypes.accept);

    case mesos::master::Call::UPDATE_WEIGHTS:
      return weightsHandler.update(call, principal, mediaTypes.accept);

    case mesos::master::Call::GET_MASTER:
      return getMaster(call, principal, mediaTypes.accept);

    case mesos::master::Call::SUBSCRIBE:
      return subscribe(call, reader, principal, mediaTypes);

    case mesos::master::Call::RESERVE_RESOURCES:
      return reserveResources(call, principal, mediaTypes.accept);

    case mesos::master::Call::UNRESERVE_RESOURCES:
      return unreserveResources(call, principal, mediaTypes.accept);

    case mesos::master::Call::CREATE_VOLUMES:
      return createVolumes(call, principal, mediaTypes.accept);

    case mesos::master::Call::DESTROY_VOLUMES:
      return destroyVolumes(call, principal, mediaTypes.accept);

    case mesos::master::Call::GROW_VOLUME:
      return growVolume(call, principal, mediaTypes.accept);

    case mesos::master::Call::SHRINK_VOLUME:
      return shrinkVolume(call, principal, mediaTypes.accept);

    case mesos::master::Call::GET_MAINTENANCE_STATUS:
      return getMaintenanceStatus(call, principal, mediaTypes.accept);

    case mesos::master::Call::GET_MAINTENANCE_SCHEDULE:
      return getMaintenanceSchedule(call, principal, mediaTypes.accept);

    case mesos::master::Call::UPDATE_MAINTENANCE_SCHEDULE:
      return updateMaintenanceSchedule(call, principal, mediaTypes.accept);

    case mesos::master::Call::START_MAINTENANCE:
      return startMaintenance(call, principal, mediaTypes.accept);

    case mesos::master::Call::STOP_MAINTENANCE:
      return stopMaintenance(call, principal, mediaTypes.accept);

    case mesos::master::Call::GET_QUOTA:
      return quotaHandler.status(call, principal, mediaTypes.accept);

    case mesos::master::Call::UPDATE_QUOTA:
      return NotImplemented();

    // TODO(bmahler): Add this to a deprecated call section
    // at the bottom once deprecated by `UPDATE_QUOTA`.
    case mesos::master::Call::SET_QUOTA:
      return quotaHandler.set(call, principal);

    // TODO(bmahler): Add this to a deprecated call section
    // at the bottom once deprecated by `UPDATE_QUOTA`.
    case mesos::master::Call::REMOVE_QUOTA:
      return quotaHandler.remove(call, principal);

    case mesos::master::Call::TEARDOWN:
      return teardown(call, principal, mediaTypes.accept);

    case mesos::master::Call::MARK_AGENT_GONE:
      return markAgentGone(call, principal, mediaTypes.accept);
  }

  UNREACHABLE();
}


Future<Response> Master::Http::subscribe(
    const mesos::master::Call& call,
    Option<shared_ptr<Reader<mesos::master::Call>>> reader,
    const Option<Principal>& principal,
    RequestMediaTypes mediaTypes) const
{
  CHECK_EQ(mesos::master::Call::SUBSCRIBE, call.type());

  return ObjectApprovers::create(
      master->authorizer,
      principal,
      {VIEW_FRAMEWORK, VIEW_TASK, VIEW_EXECUTOR, VIEW_ROLE})
    .then(defer(
        master->self(),
        [=](const Owned<ObjectApprovers>& approvers) -> Future<Response> {
          Pipe pipe;
          OK ok;

          // For backwards compatibility, the client can omit the
          // 'Message-Accept' header in favor of the 'Accept' header
          // when using this endpoint, even though the response type
          // will always be RecordIO serialization of JSON or protobuf.
          ContentType contentType = mediaTypes.accept;
          if (streamingMediaType(mediaTypes.accept)) {
            contentType = mediaTypes.messageAccept.get();
          }

          ok.headers["Content-Type"] = stringify(contentType);
          ok.type = Response::PIPE;
          ok.reader = pipe.reader();

          HttpConnection http{pipe.writer(), contentType, id::UUID::random()};

          mesos::master::Event event;
          event.set_type(mesos::master::Event::SUBSCRIBED);
          *event.mutable_subscribed()->mutable_get_state() =
            _getState(approvers);

          event.mutable_subscribed()->set_heartbeat_interval_seconds(
              DEFAULT_HEARTBEAT_INTERVAL.secs());

          http.send<mesos::master::Event, v1::master::Event>(event);

          mesos::master::Event heartbeatEvent;
          heartbeatEvent.set_type(mesos::master::Event::HEARTBEAT);
          http.send<mesos::master::Event, v1::master::Event>(heartbeatEvent);

          // Master::subscribe will start the heartbeater process, which should
          // only happen after `SUBSCRIBED` event is sent.
          master->subscribe(http, reader, principal);

          return ok;
        }));
}


// TODO(ijimenez): Add some information or pointers to help
// users understand the HTTP Event/Call API.
string Master::Http::SCHEDULER_HELP()
{
  return HELP(
    TLDR(
        "Endpoint for schedulers to make calls against the master."),
    DESCRIPTION(
        "Returns 202 Accepted iff the request is accepted.",
        "",
        "Returns 307 TEMPORARY_REDIRECT redirect to the leading master when",
        "current master is not the leader.",
        "",
        "Returns 503 SERVICE_UNAVAILABLE if the leading master cannot be",
        "found."),
    AUTHENTICATION(true),
    AUTHORIZATION(
        "The returned frameworks information might be filtered based on the",
        "users authorization.",
        "See the authorization documentation for details."));
}


Future<Response> Master::Http::scheduler(
    const Request& request,
    const Option<Principal>& principal) const
{
  // TODO(greggomann): Remove this check once the `Principal` type is used in
  // `ReservationInfo`, `DiskInfo`, and within the master's `principals` map.
  // See MESOS-7202.
  if (principal.isSome() && principal->value.isNone()) {
    return Forbidden(
        "The request's authenticated principal contains claims, but no value "
        "string. The master currently requires that principals have a value");
  }

  // TODO(vinod): Add metrics for rejected requests.

  // TODO(vinod): Add support for rate limiting.

  // When current master is not the leader, redirect to the leading master.
  // Note that this could happen if the scheduler realizes this is the
  // leading master before the master itself realizes it, e.g., due to
  // a ZooKeeper watch delay.
  if (!master->elected()) {
    return redirect(request);
  }

  CHECK_SOME(master->recovered);

  if (!master->recovered->isReady()) {
    return ServiceUnavailable("Master has not finished recovery");
  }

  if (request.method != "POST") {
    return MethodNotAllowed({"POST"}, request.method);
  }

  v1::scheduler::Call v1Call;

  // TODO(anand): Content type values are case-insensitive.
  Option<string> contentType = request.headers.get("Content-Type");

  if (contentType.isNone()) {
    return BadRequest("Expecting 'Content-Type' to be present");
  }

  if (contentType.get() == APPLICATION_PROTOBUF) {
    if (!v1Call.ParseFromString(request.body)) {
      return BadRequest("Failed to parse body into Call protobuf");
    }
  } else if (contentType.get() == APPLICATION_JSON) {
    Try<JSON::Value> value = JSON::parse(request.body);

    if (value.isError()) {
      return BadRequest("Failed to parse body into JSON: " + value.error());
    }

    Try<v1::scheduler::Call> parse =
      ::protobuf::parse<v1::scheduler::Call>(value.get());

    if (parse.isError()) {
      return BadRequest("Failed to convert JSON into Call protobuf: " +
                        parse.error());
    }

    v1Call = parse.get();
  } else {
    return UnsupportedMediaType(
        string("Expecting 'Content-Type' of ") +
        APPLICATION_JSON + " or " + APPLICATION_PROTOBUF);
  }

  scheduler::Call call = devolve(v1Call);

  Option<Error> error = validation::scheduler::call::validate(call, principal);

  if (error.isSome()) {
    master->metrics->incrementInvalidSchedulerCalls(call);
    return BadRequest("Failed to validate scheduler::Call: " + error->message);
  }

  ContentType acceptType;

  // Ideally this handler would be consistent with the Operator API handler
  // and determine the accept type regardless of the type of request.
  // However, to maintain backwards compatibility, it determines the accept
  // type only if the response will not be empty.
  if (call.type() == scheduler::Call::SUBSCRIBE ||
      call.type() == scheduler::Call::RECONCILE_OPERATIONS) {
    if (request.acceptsMediaType(APPLICATION_JSON)) {
      acceptType = ContentType::JSON;
    } else if (request.acceptsMediaType(APPLICATION_PROTOBUF)) {
      acceptType = ContentType::PROTOBUF;
    } else {
      return NotAcceptable(
          string("Expecting 'Accept' to allow ") +
          "'" + APPLICATION_PROTOBUF + "' or '" + APPLICATION_JSON + "'");
    }
  }

  if (call.type() == scheduler::Call::SUBSCRIBE) {
    // Make sure that a stream ID was not included in the request headers.
    if (request.headers.contains("Mesos-Stream-Id")) {
      return BadRequest(
          "Subscribe calls should not include the 'Mesos-Stream-Id' header");
    }

    const FrameworkInfo& frameworkInfo = call.subscribe().framework_info();

    // We allow an authenticated framework to not specify a principal in
    // `FrameworkInfo`, but in that case we log a WARNING here. We also set
    // `FrameworkInfo.principal` to the value of the authenticated principal
    // and use it for authorization later.
    //
    // NOTE: Common validation code, called previously, verifies that the
    // authenticated principal is the same as `FrameworkInfo.principal`,
    // if present.
    if (principal.isSome() && !frameworkInfo.has_principal()) {
      CHECK_SOME(principal->value);

      LOG(WARNING)
        << "Setting 'principal' in FrameworkInfo to '" << principal->value.get()
        << "' because the framework authenticated with that principal but "
        << "did not set it in FrameworkInfo";

      call.mutable_subscribe()->mutable_framework_info()->set_principal(
          principal->value.get());
    }

    Pipe pipe;
    OK ok;
    ok.headers["Content-Type"] = stringify(acceptType);

    ok.type = Response::PIPE;
    ok.reader = pipe.reader();

    // Generate a stream ID and return it in the response.
    id::UUID streamId = id::UUID::random();
    ok.headers["Mesos-Stream-Id"] = streamId.toString();

    HttpConnection http {pipe.writer(), acceptType, streamId};
    master->subscribe(http, call.subscribe());

    return ok;
  }

  // We consolidate the framework lookup logic here because it is
  // common for all the call handlers.
  Framework* framework = master->getFramework(call.framework_id());

  if (framework == nullptr) {
    return BadRequest("Framework cannot be found");
  }

  framework->metrics.incrementCall(call.type());

  // TODO(greggomann): Move this implicit scheduler authorization
  // into the authorizer. See MESOS-7399.
  if (principal.isSome() && principal != framework->info.principal()) {
    return BadRequest(
        "Authenticated principal '" + stringify(principal.get()) + "' does not "
        "match principal '" + framework->info.principal() + "' set in "
        "`FrameworkInfo`");
  }

  if (!framework->connected()) {
    return Forbidden("Framework is not subscribed");
  }

  if (framework->http.isNone()) {
    return Forbidden("Framework is not connected via HTTP");
  }

  // This isn't a `SUBSCRIBE` call, so the request should include a stream ID.
  if (!request.headers.contains("Mesos-Stream-Id")) {
    return BadRequest(
        "All non-subscribe calls should include the 'Mesos-Stream-Id' header");
  }

  const string& streamId = request.headers.at("Mesos-Stream-Id");
  if (streamId != framework->http->streamId.toString()) {
    return BadRequest(
        "The stream ID '" + streamId + "' included in this request "
        "didn't match the stream ID currently associated with framework ID "
        + framework->id().value());
  }

  switch (call.type()) {
    case scheduler::Call::SUBSCRIBE:
      // SUBSCRIBE call should have been handled above.
      LOG(FATAL) << "Unexpected 'SUBSCRIBE' call";

    case scheduler::Call::TEARDOWN:
      master->removeFramework(framework);
      return Accepted();

    case scheduler::Call::ACCEPT:
      master->accept(framework, std::move(*call.mutable_accept()));
      return Accepted();

    case scheduler::Call::DECLINE:
      master->decline(framework, std::move(*call.mutable_decline()));
      return Accepted();

    case scheduler::Call::ACCEPT_INVERSE_OFFERS:
      master->acceptInverseOffers(framework, call.accept_inverse_offers());
      return Accepted();

    case scheduler::Call::DECLINE_INVERSE_OFFERS:
      master->declineInverseOffers(framework, call.decline_inverse_offers());
      return Accepted();

    case scheduler::Call::REVIVE:
      master->revive(framework, call.revive());
      return Accepted();

    case scheduler::Call::SUPPRESS:
      master->suppress(framework, call.suppress());
      return Accepted();

    case scheduler::Call::KILL:
      master->kill(framework, call.kill());
      return Accepted();

    case scheduler::Call::SHUTDOWN:
      master->shutdown(framework, call.shutdown());
      return Accepted();

    case scheduler::Call::ACKNOWLEDGE:
      master->acknowledge(framework, std::move(*call.mutable_acknowledge()));
      return Accepted();

    case scheduler::Call::ACKNOWLEDGE_OPERATION_STATUS:
      master->acknowledgeOperationStatus(
          framework, std::move(*call.mutable_acknowledge_operation_status()));
      return Accepted();

    case scheduler::Call::RECONCILE:
      master->reconcile(framework, std::move(*call.mutable_reconcile()));
      return Accepted();

    case scheduler::Call::RECONCILE_OPERATIONS:
      return reconcileOperations(
          framework, call.reconcile_operations(), acceptType);

    case scheduler::Call::MESSAGE:
      master->message(framework, std::move(*call.mutable_message()));
      return Accepted();

    case scheduler::Call::REQUEST:
      master->request(framework, call.request());
      return Accepted();

    case scheduler::Call::UNKNOWN:
      LOG(WARNING) << "Received 'UNKNOWN' call";
      return NotImplemented();
  }

  return NotImplemented();
}


static Resources removeDiskInfos(const Resources& resources)
{
  Resources result;

  foreach (Resource resource, resources) {
    resource.clear_disk();
    result += resource;
  }

  return result;
}


string Master::Http::CREATE_VOLUMES_HELP()
{
  return HELP(
    TLDR(
        "Create persistent volumes on reserved resources."),
    DESCRIPTION(
        "Returns 202 ACCEPTED which indicates that the create",
        "operation has been validated successfully by the master.",
        "",
        "Returns 307 TEMPORARY_REDIRECT redirect to the leading master when",
        "current master is not the leader.",
        "",
        "Returns 503 SERVICE_UNAVAILABLE if the leading master cannot be",
        "found.",
        "",
        "The request is then forwarded asynchronously to the Mesos",
        "agent where the reserved resources are located.",
        "That asynchronous message may not be delivered or",
        "creating the volumes at the agent might fail.",
        "",
        "Please provide \"slaveId\" and \"volumes\" values describing",
        "the volumes to be created."),
    AUTHENTICATION(true),
    AUTHORIZATION(
        "Using this endpoint to create persistent volumes requires that",
        "the current principal is authorized to create volumes for the",
        "specific role.",
        "See the authorization documentation for details."));
}


Future<Response> Master::Http::createVolumes(
    const Request& request,
    const Option<Principal>& principal) const
{
  // TODO(greggomann): Remove this check once the `Principal` type is used in
  // `ReservationInfo`, `DiskInfo`, and within the master's `principals` map.
  // See MESOS-7202.
  if (principal.isSome() && principal->value.isNone()) {
    return Forbidden(
        "The request's authenticated principal contains claims, but no value "
        "string. The master currently requires that principals have a value");
  }

  // When current master is not the leader, redirect to the leading master.
  if (!master->elected()) {
    return redirect(request);
  }

  if (request.method != "POST") {
    return MethodNotAllowed({"POST"}, request.method);
  }

  // Parse the query string in the request body.
  Try<hashmap<string, string>> decode =
    process::http::query::decode(request.body);

  if (decode.isError()) {
    return BadRequest("Unable to decode query string: " + decode.error());
  }

  const hashmap<string, string>& values = decode.get();

  Option<string> value;

  value = values.get("slaveId");
  if (value.isNone()) {
    return BadRequest("Missing 'slaveId' query parameter in the request body");
  }

  SlaveID slaveId;
  slaveId.set_value(value.get());

  value = values.get("volumes");
  if (value.isNone()) {
    return BadRequest("Missing 'volumes' query parameter in the request body");
  }

  Try<JSON::Array> parse =
    JSON::parse<JSON::Array>(value.get());

  if (parse.isError()) {
    return BadRequest(
        "Error in parsing 'volumes' query parameter in the request body: " +
        parse.error());
  }

  RepeatedPtrField<Resource> volumes;
  foreach (const JSON::Value& value, parse->values) {
    Try<Resource> volume = ::protobuf::parse<Resource>(value);
    if (volume.isError()) {
      return BadRequest(
          "Error in parsing 'volumes' query parameter in the request body: " +
          volume.error());
    }

    volumes.Add()->CopyFrom(volume.get());
  }

  return _createVolumes(slaveId, volumes, principal);
}


Future<Response> Master::Http::_createVolumes(
    const SlaveID& slaveId,
    const RepeatedPtrField<Resource>& volumes,
    const Option<Principal>& principal) const
{
  Slave* slave = master->slaves.registered.get(slaveId);
  if (slave == nullptr) {
    return BadRequest("No agent found with specified ID");
  }

  // Create an operation.
  Offer::Operation operation;
  operation.set_type(Offer::Operation::CREATE);
  operation.mutable_create()->mutable_volumes()->CopyFrom(volumes);

  Option<Error> error = validateAndUpgradeResources(&operation);
  if (error.isSome()) {
    return BadRequest(error->message);
  }

  error = validation::operation::validate(
      operation.create(),
      slave->checkpointedResources,
      principal,
      slave->capabilities);

  if (error.isSome()) {
    return BadRequest(
        "Invalid CREATE operation on agent " + stringify(*slave) + ": " +
        error->message);
  }

  return master->authorizeCreateVolume(operation.create(), principal)
    .then(defer(master->self(), [=](bool authorized) -> Future<Response> {
      if (!authorized) {
        return Forbidden();
      }

      // The resources required for this operation are equivalent to the
      // volumes specified by the user minus any DiskInfo (DiskInfo will
      // be created when this operation is applied).
      return _operation(
          slaveId, removeDiskInfos(operation.create().volumes()), operation);
    }));
}


Future<Response> Master::Http::createVolumes(
    const mesos::master::Call& call,
    const Option<Principal>& principal,
    ContentType /*contentType*/) const
{
  // TODO(greggomann): Remove this check once the `Principal` type is used in
  // `ReservationInfo`, `DiskInfo`, and within the master's `principals` map.
  // See MESOS-7202.
  if (principal.isSome() && principal->value.isNone()) {
    return Forbidden(
        "The request's authenticated principal contains claims, but no value "
        "string. The master currently requires that principals have a value");
  }

  CHECK_EQ(mesos::master::Call::CREATE_VOLUMES, call.type());
  CHECK(call.has_create_volumes());

  const SlaveID& slaveId = call.create_volumes().slave_id();
  const RepeatedPtrField<Resource>& volumes = call.create_volumes().volumes();

  return _createVolumes(slaveId, volumes, principal);
}


string Master::Http::DESTROY_VOLUMES_HELP()
{
  return HELP(
    TLDR(
        "Destroy persistent volumes."),
    DESCRIPTION(
        "Returns 202 ACCEPTED which indicates that the destroy",
        "operation has been validated successfully by the master.",
        "",
        "Returns 307 TEMPORARY_REDIRECT redirect to the leading master when",
        "current master is not the leader.",
        "",
        "Returns 503 SERVICE_UNAVAILABLE if the leading master cannot be",
        "found.",
        "",
        "The request is then forwarded asynchronously to the Mesos",
        "agent where the reserved resources are located.",
        "That asynchronous message may not be delivered or",
        "destroying the volumes at the agent might fail.",
        "",
        "Please provide \"slaveId\" and \"volumes\" values describing",
        "the volumes to be destroyed."),
    AUTHENTICATION(true),
    AUTHORIZATION(
        "Using this endpoint to destroy persistent volumes requires that",
        "the current principal is authorized to destroy volumes created",
        "by the principal who created the volume.",
        "See the authorization documentation for details."));
}


Future<Response> Master::Http::destroyVolumes(
    const Request& request,
    const Option<Principal>& principal) const
{
  // TODO(greggomann): Remove this check once the `Principal` type is used in
  // `ReservationInfo`, `DiskInfo`, and within the master's `principals` map.
  // See MESOS-7202.
  if (principal.isSome() && principal->value.isNone()) {
    return Forbidden(
        "The request's authenticated principal contains claims, but no value "
        "string. The master currently requires that principals have a value");
  }

  // When current master is not the leader, redirect to the leading master.
  if (!master->elected()) {
    return redirect(request);
  }

  if (request.method != "POST") {
    return MethodNotAllowed({"POST"}, request.method);
  }

  // Parse the query string in the request body.
  Try<hashmap<string, string>> decode =
    process::http::query::decode(request.body);

  if (decode.isError()) {
    return BadRequest("Unable to decode query string: " + decode.error());
  }

  const hashmap<string, string>& values = decode.get();

  Option<string> value;

  value = values.get("slaveId");
  if (value.isNone()) {
    return BadRequest("Missing 'slaveId' query parameter in the request body");
  }

  SlaveID slaveId;
  slaveId.set_value(value.get());

  value = values.get("volumes");
  if (value.isNone()) {
    return BadRequest("Missing 'volumes' query parameter in the request body");
  }

  Try<JSON::Array> parse =
    JSON::parse<JSON::Array>(value.get());

  if (parse.isError()) {
    return BadRequest(
        "Error in parsing 'volumes' query parameter in the request body: " +
        parse.error());
  }

  RepeatedPtrField<Resource> volumes;
  foreach (const JSON::Value& value, parse->values) {
    Try<Resource> volume = ::protobuf::parse<Resource>(value);
    if (volume.isError()) {
      return BadRequest(
          "Error in parsing 'volumes' query parameter in the request body: " +
          volume.error());
    }

    volumes.Add()->CopyFrom(volume.get());
  }

  return _destroyVolumes(slaveId, volumes, principal);
}


Future<Response> Master::Http::_destroyVolumes(
    const SlaveID& slaveId,
    const RepeatedPtrField<Resource>& volumes,
    const Option<Principal>& principal) const
{
  Slave* slave = master->slaves.registered.get(slaveId);
  if (slave == nullptr) {
    return BadRequest("No agent found with specified ID");
  }

  // Create an operation.
  Offer::Operation operation;
  operation.set_type(Offer::Operation::DESTROY);
  operation.mutable_destroy()->mutable_volumes()->CopyFrom(volumes);

  Option<Error> error = validateAndUpgradeResources(&operation);
  if (error.isSome()) {
    return BadRequest(error->message);
  }

  error = validation::operation::validate(
      operation.destroy(),
      slave->checkpointedResources,
      slave->usedResources,
      slave->pendingTasks);

  if (error.isSome()) {
    return BadRequest("Invalid DESTROY operation: " + error->message);
  }

  return master->authorizeDestroyVolume(operation.destroy(), principal)
    .then(defer(master->self(), [=](bool authorized) -> Future<Response> {
      if (!authorized) {
        return Forbidden();
      }

      return _operation(slaveId, operation.destroy().volumes(), operation);
    }));
}


Future<Response> Master::Http::destroyVolumes(
    const mesos::master::Call& call,
    const Option<Principal>& principal,
    ContentType /*contentType*/) const
{
  // TODO(greggomann): Remove this check once the `Principal` type is used in
  // `ReservationInfo`, `DiskInfo`, and within the master's `principals` map.
  // See MESOS-7202.
  if (principal.isSome() && principal->value.isNone()) {
    return Forbidden(
        "The request's authenticated principal contains claims, but no value "
        "string. The master currently requires that principals have a value");
  }

  CHECK_EQ(mesos::master::Call::DESTROY_VOLUMES, call.type());
  CHECK(call.has_destroy_volumes());

  const SlaveID& slaveId = call.destroy_volumes().slave_id();
  const RepeatedPtrField<Resource>& volumes = call.destroy_volumes().volumes();

  return _destroyVolumes(slaveId, volumes, principal);
}


Future<Response> Master::Http::growVolume(
    const mesos::master::Call& call,
    const Option<Principal>& principal,
    ContentType /*contentType*/) const
{
  // TODO(greggomann): Remove this check once the `Principal` type is used in
  // `ReservationInfo`, `DiskInfo`, and within the master's `principals` map.
  // See MESOS-7202.
  if (principal.isSome() && principal->value.isNone()) {
    return Forbidden(
        "The request's authenticated principal contains claims, but no value "
        "string. The master currently requires that principals have a value");
  }

  CHECK_EQ(mesos::master::Call::GROW_VOLUME, call.type());
  CHECK(call.has_grow_volume());

  // Only agent default resources are supported right now.
  CHECK(call.grow_volume().has_slave_id());

  const SlaveID& slaveId = call.grow_volume().slave_id();

  Slave* slave = master->slaves.registered.get(slaveId);
  if (slave == nullptr) {
    return BadRequest("No agent found with specified ID");
  }

  // Create an operation.
  Offer::Operation operation;
  operation.set_type(Offer::Operation::GROW_VOLUME);

  operation.mutable_grow_volume()->mutable_volume()->CopyFrom(
      call.grow_volume().volume());

  operation.mutable_grow_volume()->mutable_addition()->CopyFrom(
      call.grow_volume().addition());

  Option<Error> error = validateAndUpgradeResources(&operation);
  if (error.isSome()) {
    return BadRequest(error->message);
  }

  error = validation::operation::validate(
      operation.grow_volume(), slave->capabilities);

  if (error.isSome()) {
    return BadRequest(
        "Invalid GROW_VOLUME operation on agent " +
        stringify(*slave) + ": " + error->message);
  }

  return master->authorizeResizeVolume(
      operation.grow_volume().volume(), principal)
    .then(defer(master->self(), [=](bool authorized) -> Future<Response> {
      if (!authorized) {
        return Forbidden();
      }

      // The `volume` and `addition` fields contain the resources required for
      // this operation.
      return _operation(
          slaveId,
          Resources(operation.grow_volume().volume()) +
            Resources(operation.grow_volume().addition()),
          operation);
    }));
}


Future<Response> Master::Http::shrinkVolume(
    const mesos::master::Call& call,
    const Option<Principal>& principal,
    ContentType /*contentType*/) const
{
  // TODO(greggomann): Remove this check once the `Principal` type is used in
  // `ReservationInfo`, `DiskInfo`, and within the master's `principals` map.
  // See MESOS-7202.
  if (principal.isSome() && principal->value.isNone()) {
    return Forbidden(
        "The request's authenticated principal contains claims, but no value "
        "string. The master currently requires that principals have a value");
  }

  CHECK_EQ(mesos::master::Call::SHRINK_VOLUME, call.type());
  CHECK(call.has_shrink_volume());

  // Only persistent volumes are supported right now.
  CHECK(call.shrink_volume().has_slave_id());

  const SlaveID& slaveId = call.shrink_volume().slave_id();

  Slave* slave = master->slaves.registered.get(slaveId);
  if (slave == nullptr) {
    return BadRequest("No agent found with specified ID");
  }

  // Create an operation.
  Offer::Operation operation;
  operation.set_type(Offer::Operation::SHRINK_VOLUME);

  operation.mutable_shrink_volume()->mutable_volume()->CopyFrom(
      call.shrink_volume().volume());

  operation.mutable_shrink_volume()->mutable_subtract()->CopyFrom(
      call.shrink_volume().subtract());

  Option<Error> error = validateAndUpgradeResources(&operation);
  if (error.isSome()) {
    return BadRequest(error->message);
  }

  error = validation::operation::validate(
      operation.shrink_volume(), slave->capabilities);

  if (error.isSome()) {
    return BadRequest(
        "Invalid SHRINK_VOLUME operation on agent " +
        stringify(*slave) + ": " + error->message);
  }

  return master->authorizeResizeVolume(
      operation.shrink_volume().volume(), principal)
    .then(defer(master->self(), [=](bool authorized) -> Future<Response> {
      if (!authorized) {
        return Forbidden();
      }

      // The `volume` field contains the resources required for this operation.
      return _operation(
          slaveId, operation.shrink_volume().volume(), operation);
    }));
}


string Master::Http::FRAMEWORKS_HELP()
{
  return HELP(
    TLDR("Exposes the frameworks info."),
    DESCRIPTION(
        "Returns 200 OK when the frameworks info was queried successfully.",
        "",
        "Returns 307 TEMPORARY_REDIRECT redirect to the leading master when",
        "current master is not the leader.",
        "",
        "Returns 503 SERVICE_UNAVAILABLE if the leading master cannot be",
        "found.",
        "",
        "Query parameters:",
        ">        framework_id=VALUE   The ID of the framework returned "
        "(if no framework ID is specified, all frameworks will be returned)."),
    AUTHENTICATION(true),
    AUTHORIZATION(
        "This endpoint might be filtered based on the user accessing it.",
        "See the authorization documentation for details."));
}


Future<Response> Master::Http::frameworks(
    const Request& request,
    const Option<Principal>& principal) const
{
  // TODO(greggomann): Remove this check once the `Principal` type is used in
  // `ReservationInfo`, `DiskInfo`, and within the master's `principals` map.
  // See MESOS-7202.
  if (principal.isSome() && principal->value.isNone()) {
    return Forbidden(
        "The request's authenticated principal contains claims, but no value "
        "string. The master currently requires that principals have a value");
  }

  // When current master is not the leader, redirect to the leading master.
  if (!master->elected()) {
    return redirect(request);
  }

  return ObjectApprovers::create(
      master->authorizer,
      principal,
      {VIEW_FRAMEWORK, VIEW_TASK, VIEW_EXECUTOR})
    .then(defer(
        master->self(),
        [this, request](const Owned<ObjectApprovers>& approvers) {
          return deferBatchedRequest(
            &Master::ReadOnlyHandler::frameworks, request, approvers);
        }));
}


mesos::master::Response::GetFrameworks::Framework model(
    const Framework& framework)
{
  mesos::master::Response::GetFrameworks::Framework _framework;

  _framework.mutable_framework_info()->CopyFrom(framework.info);

  _framework.set_active(framework.active());
  _framework.set_connected(framework.connected());
  _framework.set_recovered(framework.recovered());

  int64_t time = framework.registeredTime.duration().ns();
  if (time != 0) {
    _framework.mutable_registered_time()->set_nanoseconds(time);
  }

  time = framework.unregisteredTime.duration().ns();
  if (time != 0) {
    _framework.mutable_unregistered_time()->set_nanoseconds(time);
  }

  time = framework.reregisteredTime.duration().ns();
  if (time != 0) {
    _framework.mutable_reregistered_time()->set_nanoseconds(time);
  }

  foreach (const Offer* offer, framework.offers) {
    _framework.mutable_offers()->Add()->CopyFrom(*offer);
  }

  foreach (const InverseOffer* offer, framework.inverseOffers) {
    _framework.mutable_inverse_offers()->Add()->CopyFrom(*offer);
  }

  foreach (Resource resource, framework.totalUsedResources) {
    convertResourceFormat(&resource, ENDPOINT);

    _framework.mutable_allocated_resources()->Add()->CopyFrom(resource);
  }

  foreach (Resource resource, framework.totalOfferedResources) {
    convertResourceFormat(&resource, ENDPOINT);

    _framework.mutable_offered_resources()->Add()->CopyFrom(resource);
  }

  return _framework;
}


Future<Response> Master::Http::getFrameworks(
    const mesos::master::Call& call,
    const Option<Principal>& principal,
    ContentType contentType) const
{
  CHECK_EQ(mesos::master::Call::GET_FRAMEWORKS, call.type());

  return ObjectApprovers::create(
      master->authorizer,
      principal,
      {VIEW_FRAMEWORK})
    .then(defer(
        master->self(),
        [=](const Owned<ObjectApprovers>& approvers) -> Future<Response> {
          mesos::master::Response response;
          response.set_type(mesos::master::Response::GET_FRAMEWORKS);
          *response.mutable_get_frameworks() = _getFrameworks(approvers);

          return OK(
              serialize(contentType, evolve(response)), stringify(contentType));
        }));
}


mesos::master::Response::GetFrameworks Master::Http::_getFrameworks(
    const Owned<ObjectApprovers>& approvers) const
{
  mesos::master::Response::GetFrameworks getFrameworks;
  foreachvalue (const Framework* framework,
                master->frameworks.registered) {
    // Skip unauthorized frameworks.
    if (!approvers->approved<VIEW_FRAMEWORK>(framework->info)) {
      continue;
    }

    *getFrameworks.add_frameworks() = model(*framework);
  }

  foreachvalue (const Owned<Framework>& framework,
                master->frameworks.completed) {
    // Skip unauthorized frameworks.
    if (!approvers->approved<VIEW_FRAMEWORK>(framework->info)) {
      continue;
    }

    *getFrameworks.add_completed_frameworks() = model(*framework);
  }

  return getFrameworks;
}


Future<Response> Master::Http::getExecutors(
    const mesos::master::Call& call,
    const Option<Principal>& principal,
    ContentType contentType) const
{
  CHECK_EQ(mesos::master::Call::GET_EXECUTORS, call.type());

  return ObjectApprovers::create(
      master->authorizer,
      principal,
      {VIEW_FRAMEWORK, VIEW_EXECUTOR})
    .then(defer(
        master->self(),
        [=](const Owned<ObjectApprovers>& approvers) -> Response {
          mesos::master::Response response;
          response.set_type(mesos::master::Response::GET_EXECUTORS);

          *response.mutable_get_executors() = _getExecutors(approvers);

          return OK(
              serialize(contentType, evolve(response)), stringify(contentType));
        }));
}


mesos::master::Response::GetExecutors Master::Http::_getExecutors(
    const Owned<ObjectApprovers>& approvers) const
{
  // Construct framework list with both active and completed frameworks.
  vector<const Framework*> frameworks;
  foreachvalue (Framework* framework, master->frameworks.registered) {
    // Skip unauthorized frameworks.
    if (!approvers->approved<VIEW_FRAMEWORK>(framework->info)) {
      continue;
    }

    frameworks.push_back(framework);
  }

  foreachvalue (const Owned<Framework>& framework,
                master->frameworks.completed) {
    // Skip unauthorized frameworks.
    if (!approvers->approved<VIEW_FRAMEWORK>(framework->info)) {
      continue;
    }

    frameworks.push_back(framework.get());
  }

  mesos::master::Response::GetExecutors getExecutors;

  foreach (const Framework* framework, frameworks) {
    foreachpair (const SlaveID& slaveId,
                 const auto& executorsMap,
                 framework->executors) {
      foreachvalue (const ExecutorInfo& executorInfo, executorsMap) {
        // Skip unauthorized executors.
        if (!approvers->approved<VIEW_EXECUTOR>(
                executorInfo, framework->info)) {
          continue;
        }

        mesos::master::Response::GetExecutors::Executor* executor =
          getExecutors.add_executors();

        executor->mutable_executor_info()->CopyFrom(executorInfo);
        executor->mutable_slave_id()->CopyFrom(slaveId);
      }
    }
  }

  return getExecutors;
}


Future<Response> Master::Http::getState(
    const mesos::master::Call& call,
    const Option<Principal>& principal,
    ContentType contentType) const
{
  CHECK_EQ(mesos::master::Call::GET_STATE, call.type());

  return ObjectApprovers::create(
      master->authorizer,
      principal,
      {VIEW_FRAMEWORK, VIEW_TASK, VIEW_EXECUTOR, VIEW_ROLE})
    .then(defer(
        master->self(),
        [=](const Owned<ObjectApprovers>& approvers) -> Response {
          mesos::master::Response response;
          response.set_type(mesos::master::Response::GET_STATE);

          *response.mutable_get_state() = _getState(approvers);

          return OK(
              serialize(contentType, evolve(response)), stringify(contentType));
        }));
}


mesos::master::Response::GetState Master::Http::_getState(
    const Owned<ObjectApprovers>& approvers) const
{
  // NOTE: This function must be blocking instead of returning a
  // `Future`. This is because `subscribe()` needs to atomically
  // add subscriber to `subscribers` map and send the captured state
  // in `SUBSCRIBED` without being interleaved by any other events.

  mesos::master::Response::GetState getState;

  *getState.mutable_get_tasks() = _getTasks(approvers);
  *getState.mutable_get_executors() = _getExecutors(approvers);
  *getState.mutable_get_frameworks() = _getFrameworks(approvers);
  *getState.mutable_get_agents() = _getAgents(approvers);

  return getState;
}


class Master::Http::FlagsError : public Error
{
public:
  enum Type
  {
    UNAUTHORIZED
  };

  // TODO(arojas): Provide a proper string representation of the enum.
  explicit FlagsError(Type _type)
    : Error(stringify(_type)), type(_type) {}

  FlagsError(Type _type, const string& _message)
    : Error(stringify(_type)), type(_type), message(_message) {}

  const Type type;
  const string message;
};


string Master::Http::FLAGS_HELP()
{
  return HELP(
    TLDR("Exposes the master's flag configuration."),
    None(),
    AUTHENTICATION(true),
    AUTHORIZATION(
        "Querying this endpoint requires that the current principal",
        "is authorized to view all flags.",
        "See the authorization documentation for details."));
}


Future<Response> Master::Http::flags(
    const Request& request,
    const Option<Principal>& principal) const
{
  // TODO(greggomann): Remove this check once the `Principal` type is used in
  // `ReservationInfo`, `DiskInfo`, and within the master's `principals` map.
  // See MESOS-7202.
  if (principal.isSome() && principal->value.isNone()) {
    return Forbidden(
        "The request's authenticated principal contains claims, but no value "
        "string. The master currently requires that principals have a value");
  }

  // TODO(nfnt): Remove check for enabled
  // authorization as part of MESOS-5346.
  if (request.method != "GET" && master->authorizer.isSome()) {
    return MethodNotAllowed({"GET"}, request.method);
  }

  Option<string> jsonp = request.url.query.get("jsonp");

  return _flags(principal)
    .then([jsonp](const Try<JSON::Object, FlagsError>& flags)
          -> Future<Response> {
      if (flags.isError()) {
        switch (flags.error().type) {
          case FlagsError::Type::UNAUTHORIZED:
            return Forbidden();
        }

        return InternalServerError(flags.error().message);
      }

      return OK(flags.get(), jsonp);
    });
}


Future<Try<JSON::Object, Master::Http::FlagsError>> Master::Http::_flags(
    const Option<Principal>& principal) const
{
  if (master->authorizer.isNone()) {
    return __flags();
  }

  authorization::Request authRequest;
  authRequest.set_action(authorization::VIEW_FLAGS);

  Option<authorization::Subject> subject = createSubject(principal);
  if (subject.isSome()) {
    authRequest.mutable_subject()->CopyFrom(subject.get());
  }

  return master->authorizer.get()->authorized(authRequest)
    .then(defer(
        master->self(),
        [this](bool authorized) -> Future<Try<JSON::Object, FlagsError>> {
      if (authorized) {
        return __flags();
      } else {
        return FlagsError(FlagsError::Type::UNAUTHORIZED);
      }
    }));
}


JSON::Object Master::Http::__flags() const
{
  JSON::Object object;

  {
    JSON::Object flags;
    foreachvalue (const flags::Flag& flag, master->flags) {
      Option<string> value = flag.stringify(master->flags);
      if (value.isSome()) {
        flags.values[flag.effective_name().value] = value.get();
      }
    }
    object.values["flags"] = std::move(flags);
  }

  return object;
}


Future<Response> Master::Http::getFlags(
    const mesos::master::Call& call,
    const Option<Principal>& principal,
    ContentType contentType) const
{
  CHECK_EQ(mesos::master::Call::GET_FLAGS, call.type());

  return _flags(principal)
    .then([contentType](const Try<JSON::Object, FlagsError>& flags)
          -> Future<Response> {
      if (flags.isError()) {
        switch (flags.error().type) {
          case FlagsError::Type::UNAUTHORIZED:
            return Forbidden();
        }

        return InternalServerError(flags.error().message);
      }

      return OK(
          serialize(contentType,
                    evolve<v1::master::Response::GET_FLAGS>(flags.get())),
          stringify(contentType));
    });
}


string Master::Http::HEALTH_HELP()
{
  return HELP(
    TLDR(
        "Health status of the Master."),
    DESCRIPTION(
        "Returns 200 OK iff the Master is healthy.",
        "Delayed responses are also indicative of poor health."),
    AUTHENTICATION(false));
}


Future<Response> Master::Http::health(const Request& request) const
{
  return OK();
}


Future<Response> Master::Http::getHealth(
    const mesos::master::Call& call,
    const Option<Principal>& principal,
    ContentType contentType) const
{
  CHECK_EQ(mesos::master::Call::GET_HEALTH, call.type());

  mesos::master::Response response;
  response.set_type(mesos::master::Response::GET_HEALTH);
  response.mutable_get_health()->set_healthy(true);

  return OK(serialize(contentType, evolve(response)),
            stringify(contentType));
}


Future<Response> Master::Http::getVersion(
    const mesos::master::Call& call,
    const Option<Principal>& principal,
    ContentType contentType) const
{
  CHECK_EQ(mesos::master::Call::GET_VERSION, call.type());

  return OK(serialize(contentType,
                      evolve<v1::master::Response::GET_VERSION>(version())),
            stringify(contentType));
}


Future<Response> Master::Http::getMetrics(
    const mesos::master::Call& call,
    const Option<Principal>& principal,
    ContentType contentType) const
{
  CHECK_EQ(mesos::master::Call::GET_METRICS, call.type());
  CHECK(call.has_get_metrics());

  Option<Duration> timeout;
  if (call.get_metrics().has_timeout()) {
    timeout = Nanoseconds(call.get_metrics().timeout().nanoseconds());
  }

  return process::metrics::snapshot(timeout)
    .then([contentType](const map<string, double>& metrics) -> Response {
      mesos::master::Response response;
      response.set_type(mesos::master::Response::GET_METRICS);
      mesos::master::Response::GetMetrics* _getMetrics =
        response.mutable_get_metrics();

      foreachpair (const string& key, double value, metrics) {
        Metric* metric = _getMetrics->add_metrics();
        metric->set_name(key);
        metric->set_value(value);
      }

      return OK(serialize(contentType, evolve(response)),
                stringify(contentType));
    });
}


Future<Response> Master::Http::getLoggingLevel(
    const mesos::master::Call& call,
    const Option<Principal>& principal,
    ContentType contentType) const
{
  CHECK_EQ(mesos::master::Call::GET_LOGGING_LEVEL, call.type());

  mesos::master::Response response;
  response.set_type(mesos::master::Response::GET_LOGGING_LEVEL);
  response.mutable_get_logging_level()->set_level(FLAGS_v);

  return OK(serialize(contentType, evolve(response)),
            stringify(contentType));
}


Future<Response> Master::Http::setLoggingLevel(
    const mesos::master::Call& call,
    const Option<Principal>& principal,
    ContentType /*contentType*/) const
{
  CHECK_EQ(mesos::master::Call::SET_LOGGING_LEVEL, call.type());
  CHECK(call.has_set_logging_level());

  uint32_t level = call.set_logging_level().level();
  Duration duration =
    Nanoseconds(call.set_logging_level().duration().nanoseconds());

  return ObjectApprovers::create(master->authorizer, principal, {SET_LOG_LEVEL})
    .then([level, duration](const Owned<ObjectApprovers>& approvers)
        -> Future<Response> {
       if (!approvers->approved<SET_LOG_LEVEL>()) {
        return Forbidden();
      }

      return dispatch(process::logging(), &Logging::set_level, level, duration)
        .then([]() -> Response {
          return OK();
        });
    });
}


Future<Response> Master::Http::getMaster(
    const mesos::master::Call& call,
    const Option<Principal>& principal,
    ContentType contentType) const
{
  CHECK_EQ(mesos::master::Call::GET_MASTER, call.type());

  mesos::master::Response response;
  response.set_type(mesos::master::Response::GET_MASTER);

  // It is guaranteed that this master has been elected as the leader.
  CHECK(master->elected());

  mesos::master::Response::GetMaster* getMaster = response.mutable_get_master();

  getMaster->mutable_master_info()->CopyFrom(master->info());

  getMaster->set_start_time(master->startTime.secs());
  if (master->electedTime.isSome()) {
    getMaster->set_elected_time(master->electedTime->secs());
  }

  return OK(serialize(contentType, evolve(response)),
            stringify(contentType));
}


string Master::Http::REDIRECT_HELP()
{
  return HELP(
    TLDR(
        "Redirects to the leading Master."),
    DESCRIPTION(
        "Returns 307 TEMPORARY_REDIRECT redirect to the leading master when",
        "current master is not the leader.",
        "",
        "Returns 503 SERVICE_UNAVAILABLE if the leading master cannot be",
        "found.",
        "",
        "**NOTES:**",
        "1. This is the recommended way to bookmark the WebUI when "
        "running multiple Masters.",
        "2. This is broken currently \"on the cloud\" (e.g., EC2) as "
        "this will attempt to redirect to the private IP address, unless "
        "`advertise_ip` points to an externally accessible IP"),
    AUTHENTICATION(false));
}


Future<Response> Master::Http::redirect(const Request& request) const
{
  // If there's no leader, return `ServiceUnavailable`.
  if (master->leader.isNone()) {
    LOG(WARNING) << "Current master is not elected as leader, and leader "
                 << "information is unavailable. Failed to redirect the "
                 << "request url: " << request.url;
    return ServiceUnavailable("No leader elected");
  }

  MasterInfo info = master->leader.get();

  // NOTE: Currently, 'info.ip()' stores ip in network order, which
  // should be fixed. See MESOS-1201 for details.
  Try<string> hostname = info.has_hostname()
    ? info.hostname()
    : net::getHostname(net::IP(ntohl(info.ip())));

  if (hostname.isError()) {
    return InternalServerError(hostname.error());
  }

  LOG(INFO) << "Redirecting request for " << request.url
            << " to the leading master " << hostname.get();

  // NOTE: We can use a protocol-relative URL here in order to allow
  // the browser (or other HTTP client) to prefix with 'http:' or
  // 'https:' depending on the original request. See
  // https://tools.ietf.org/html/rfc7231#section-7.1.2 as well as
  // http://stackoverflow.com/questions/12436669/using-protocol-relative-uris-within-location-headers
  // which discusses this.
  string basePath = "//" + hostname.get() + ":" + stringify(info.port());

  string redirectPath = "/redirect";
  string masterRedirectPath = "/" + master->self().id + "/redirect";

  if (request.url.path == redirectPath ||
      request.url.path == masterRedirectPath) {
    // When request url is '/redirect' or '/master/redirect', redirect to the
    // base url of leading master to avoid infinite redirect loop.
    return TemporaryRedirect(basePath);
  } else if (strings::startsWith(request.url.path, redirectPath + "/") ||
             strings::startsWith(request.url.path, masterRedirectPath + "/")) {
    // Prevent redirection loop.
    return NotFound();
  } else {
    // `request.url` is not absolute so we can safely append it to
    // `basePath`. See https://tools.ietf.org/html/rfc2616#section-5.1.2
    // for details.
    CHECK(!request.url.isAbsolute());
    return TemporaryRedirect(basePath + stringify(request.url));
  }
}


string Master::Http::RESERVE_HELP()
{
  return HELP(
    TLDR(
        "Reserve resources dynamically on a specific agent."),
    DESCRIPTION(
        "Returns 202 ACCEPTED which indicates that the reserve",
        "operation has been validated successfully by the master.",
        "",
        "Returns 307 TEMPORARY_REDIRECT redirect to the leading master when",
        "current master is not the leader.",
        "",
        "Returns 503 SERVICE_UNAVAILABLE if the leading master cannot be",
        "found.",
        "",
        "The request is then forwarded asynchronously to the Mesos",
        "agent where the reserved resources are located.",
        "That asynchronous message may not be delivered or",
        "reserving resources at the agent might fail.",
        "",
        "Please provide \"slaveId\" and \"resources\" values describing",
        "the resources to be reserved."),
    AUTHENTICATION(true),
    AUTHORIZATION(
        "Using this endpoint to reserve resources requires that the",
        "current principal is authorized to reserve resources for the",
        "specific role.",
        "See the authorization documentation for details."));
}


Future<Response> Master::Http::reserve(
    const Request& request,
    const Option<Principal>& principal) const
{
  // TODO(greggomann): Remove this check once the `Principal` type is used in
  // `ReservationInfo`, `DiskInfo`, and within the master's `principals` map.
  // See MESOS-7202.
  if (principal.isSome() && principal->value.isNone()) {
    return Forbidden(
        "The request's authenticated principal contains claims, but no value "
        "string. The master currently requires that principals have a value");
  }

  // When current master is not the leader, redirect to the leading master.
  if (!master->elected()) {
    return redirect(request);
  }

  if (request.method != "POST") {
    return MethodNotAllowed({"POST"}, request.method);
  }

  // Parse the query string in the request body.
  Try<hashmap<string, string>> decode =
    process::http::query::decode(request.body);

  if (decode.isError()) {
    return BadRequest("Unable to decode query string: " + decode.error());
  }

  const hashmap<string, string>& values = decode.get();

  Option<string> value;

  value = values.get("slaveId");
  if (value.isNone()) {
    return BadRequest("Missing 'slaveId' query parameter in the request body");
  }

  SlaveID slaveId;
  slaveId.set_value(value.get());

  value = values.get("resources");
  if (value.isNone()) {
    return BadRequest(
        "Missing 'resources' query parameter in the request body");
  }

  Try<JSON::Array> parse =
    JSON::parse<JSON::Array>(value.get());

  if (parse.isError()) {
    return BadRequest(
        "Error in parsing 'resources' query parameter in the request body: " +
        parse.error());
  }

  RepeatedPtrField<Resource> resources;
  foreach (const JSON::Value& value, parse->values) {
    Try<Resource> resource = ::protobuf::parse<Resource>(value);
    if (resource.isError()) {
      return BadRequest(
          "Error in parsing 'resources' query parameter in the request body: " +
          resource.error());
    }

    resources.Add()->CopyFrom(resource.get());
  }

  return _reserve(slaveId, resources, principal);
}


Future<Response> Master::Http::_reserve(
    const SlaveID& slaveId,
    const RepeatedPtrField<Resource>& resources,
    const Option<Principal>& principal) const
{
  Slave* slave = master->slaves.registered.get(slaveId);
  if (slave == nullptr) {
    return BadRequest("No agent found with specified ID");
  }

  // Create an operation.
  Offer::Operation operation;
  operation.set_type(Offer::Operation::RESERVE);
  operation.mutable_reserve()->mutable_resources()->CopyFrom(resources);

  Option<Error> error = validateAndUpgradeResources(&operation);
  if (error.isSome()) {
    return BadRequest(error->message);
  }

  error = validation::operation::validate(
      operation.reserve(), principal, slave->capabilities);

  if (error.isSome()) {
    return BadRequest(
        "Invalid RESERVE operation on agent " + stringify(*slave) + ": " +
        error->message);
  }

  return master->authorizeReserveResources(operation.reserve(), principal)
    .then(defer(master->self(), [=](bool authorized) -> Future<Response> {
      if (!authorized) {
        return Forbidden();
      }

      // We only allow "pushing" a single reservation at a time, so we require
      // the resources with one reservation "popped" to be present on the agent.
      Resources required =
        Resources(operation.reserve().resources()).popReservation();

      return _operation(slaveId, required, operation);
    }));
}


Future<Response> Master::Http::reserveResources(
    const mesos::master::Call& call,
    const Option<Principal>& principal,
    ContentType contentType) const
{
  CHECK_EQ(mesos::master::Call::RESERVE_RESOURCES, call.type());

  const SlaveID& slaveId = call.reserve_resources().slave_id();
  const RepeatedPtrField<Resource>& resources =
    call.reserve_resources().resources();

  return _reserve(slaveId, resources, principal);
}


string Master::Http::SLAVES_HELP()
{
  return HELP(
    TLDR(
        "Information about agents."),
    DESCRIPTION(
        "Returns 200 OK when the request was processed successfully.",
        "",
        "Returns 307 TEMPORARY_REDIRECT redirect to the leading master when",
        "current master is not the leader.",
        "",
        "Returns 503 SERVICE_UNAVAILABLE if the leading master cannot be",
        "found.",
        "",
        "This endpoint shows information about the agents which are registered",
        "in this master or recovered from registry, formatted as a JSON",
        "object.",
        "",
        "Query parameters:",
        ">        slave_id=VALUE       The ID of the slave returned "
        "(when no slave_id is specified, all slaves will be returned)."),
    AUTHENTICATION(true));
}


Future<Response> Master::Http::slaves(
    const Request& request,
    const Option<Principal>& principal) const
{
  // When current master is not the leader, redirect to the leading master.
  if (!master->elected()) {
    return redirect(request);
  }

  return ObjectApprovers::create(master->authorizer, principal, {VIEW_ROLE})
    .then(defer(
        master->self(),
        [this, request](const Owned<ObjectApprovers>& approvers) {
          return deferBatchedRequest(
              &Master::ReadOnlyHandler::slaves, request, approvers);
        }));
}


Future<Response> Master::Http::getAgents(
    const mesos::master::Call& call,
    const Option<Principal>& principal,
    ContentType contentType) const
{
  CHECK_EQ(mesos::master::Call::GET_AGENTS, call.type());

  return ObjectApprovers::create(master->authorizer, principal, {VIEW_ROLE})
    .then(defer(
        master->self(),
        [=](const Owned<ObjectApprovers>& approvers) -> Response {
          mesos::master::Response response;
          response.set_type(mesos::master::Response::GET_AGENTS);
          *response.mutable_get_agents() = _getAgents(approvers);

          return OK(
              serialize(contentType, evolve(response)), stringify(contentType));
    }));
}


mesos::master::Response::GetAgents Master::Http::_getAgents(
    const Owned<ObjectApprovers>& approvers) const
{
  mesos::master::Response::GetAgents getAgents;
  foreachvalue (const Slave* slave, master->slaves.registered) {
    mesos::master::Response::GetAgents::Agent* agent = getAgents.add_agents();
    *agent =
        protobuf::master::event::createAgentResponse(*slave, approvers);
  }

  foreachvalue (const SlaveInfo& slaveInfo, master->slaves.recovered) {
    SlaveInfo* agent = getAgents.add_recovered_agents();
    agent->CopyFrom(slaveInfo);
    agent->clear_resources();
    foreach (const Resource& resource, slaveInfo.resources()) {
      if (approvers->approved<VIEW_ROLE>(resource)) {
        agent->add_resources()->CopyFrom(resource);
      }
    }
  }

  return getAgents;
}


string Master::Http::QUOTA_HELP()
{
  return HELP(
    TLDR(
        "Gets or updates quota for roles."),
    DESCRIPTION(
        "Returns 200 OK when the quota was queried or updated successfully.",
        "",
        "Returns 307 TEMPORARY_REDIRECT redirect to the leading master when",
        "current master is not the leader.",
        "",
        "Returns 503 SERVICE_UNAVAILABLE if the leading master cannot be",
        "found.",
        "",
        "GET: Returns the currently set quotas as JSON.",
        "",
        "POST: Validates the request body as JSON",
        " and sets quota for a role.",
        "",
        "DELETE: Validates the request body as JSON",
        " and removes quota for a role."),
    AUTHENTICATION(true),
    AUTHORIZATION(
        "Using this endpoint to set a quota for a certain role requires that",
        "the current principal is authorized to set quota for the target role.",
        "Similarly, removing quota requires that the principal is authorized",
        "to remove quota created by the quota_principal.",
        "Getting quota information for a certain role requires that the",
        "current principal is authorized to get quota for the target role,",
        "otherwise the entry for the target role could be silently filtered.",
        "See the authorization documentation for details."));
}


Future<Response> Master::Http::quota(
    const Request& request,
    const Option<Principal>& principal) const
{
  // TODO(greggomann): Remove this check once the `Principal` type is used in
  // `ReservationInfo`, `DiskInfo`, and within the master's `principals` map.
  // See MESOS-7202.
  if (principal.isSome() && principal->value.isNone()) {
    return Forbidden(
        "The request's authenticated principal contains claims, but no value "
        "string. The master currently requires that principals have a value");
  }

  // When current master is not the leader, redirect to the leading master.
  if (!master->elected()) {
    return redirect(request);
  }

  // Dispatch based on HTTP method to separate `QuotaHandler`.
  if (request.method == "GET") {
    return quotaHandler.status(request, principal);
  }

  if (request.method == "POST") {
    return quotaHandler.set(request, principal);
  }

  if (request.method == "DELETE") {
    return quotaHandler.remove(request, principal);
  }

  // TODO(joerg84): Add update logic for PUT requests
  // once Quota supports updates.

  return MethodNotAllowed({"GET", "POST", "DELETE"}, request.method);
}


string Master::Http::WEIGHTS_HELP()
{
  return HELP(
    TLDR(
        "Updates weights for the specified roles."),
    DESCRIPTION(
        "Returns 200 OK when the weights update was successful.",
        "",
        "Returns 307 TEMPORARY_REDIRECT redirect to the leading master when",
        "current master is not the leader.",
        "",
        "Returns 503 SERVICE_UNAVAILABLE if the leading master cannot be",
        "found.",
        "",
        "PUT: Validates the request body as JSON",
        "and updates the weights for the specified roles."),
    AUTHENTICATION(true),
    AUTHORIZATION(
        "Getting weight information for a role requires that the current",
        "principal is authorized to get weights for the target role,",
        "otherwise the entry for the target role could be silently filtered.",
        "See the authorization documentation for details."));
}


Future<Response> Master::Http::weights(
    const Request& request,
    const Option<Principal>& principal) const
{
  // TODO(greggomann): Remove this check once the `Principal` type is used in
  // `ReservationInfo`, `DiskInfo`, and within the master's `principals` map.
  // See MESOS-7202.
  if (principal.isSome() && principal->value.isNone()) {
    return Forbidden(
        "The request's authenticated principal contains claims, but no value "
        "string. The master currently requires that principals have a value");
  }

  // When current master is not the leader, redirect to the leading master.
  if (!master->elected()) {
    return redirect(request);
  }

  if (request.method == "GET") {
    return weightsHandler.get(request, principal);
  }

  // Dispatch based on HTTP method to separate `WeightsHandler`.
  if (request.method == "PUT") {
    return weightsHandler.update(request, principal);
  }

  return MethodNotAllowed({"GET", "PUT"}, request.method);
}


string Master::Http::STATE_HELP()
{
  return HELP(
    TLDR(
        "Information about state of master."),
    DESCRIPTION(
        "Returns 200 OK when the state of the master was queried successfully.",
        "",
        "Returns 307 TEMPORARY_REDIRECT redirect to the leading master when",
        "current master is not the leader.",
        "",
        "Returns 503 SERVICE_UNAVAILABLE if the leading master cannot be",
        "found.",
        "",
        "This endpoint shows information about the frameworks, tasks,",
        "executors, and agents running in the cluster as a JSON object.",
        "The information shown might be filtered based on the user",
        "accessing the endpoint.",
        "",
        "Example (**Note**: this is not exhaustive):",
        "",
        "```",
        "{",
        "    \"version\" : \"0.28.0\",",
        "    \"git_sha\" : \"9d5889b5a265849886a533965f4aefefd1fbd103\",",
        "    \"git_branch\" : \"refs/heads/master\",",
        "    \"git_tag\" : \"0.28.0\",",
        "    \"build_date\" : \"2016-02-15 10:00:28\",",
        "    \"build_time\" : 1455559228,",
        "    \"build_user\" : \"mesos-user\",",
        "    \"start_time\" : 1455643643.42422,",
        "    \"elected_time\" : 1455643643.43457,",
        "    \"id\" : \"b5eac2c5-609b-4ca1-a352-61941702fc9e\",",
        "    \"pid\" : \"master@127.0.0.1:5050\",",
        "    \"hostname\" : \"localhost\",",
        "    \"activated_slaves\" : 0,",
        "    \"deactivated_slaves\" : 0,",
        "    \"cluster\" : \"test-cluster\",",
        "    \"leader\" : \"master@127.0.0.1:5050\",",
        "    \"log_dir\" : \"/var/log\",",
        "    \"external_log_file\" : \"mesos.log\",",
        "    \"flags\" : {",
        "         \"framework_sorter\" : \"drf\",",
        "         \"authenticate\" : \"false\",",
        "         \"logbufsecs\" : \"0\",",
        "         \"initialize_driver_logging\" : \"true\",",
        "         \"work_dir\" : \"/var/lib/mesos\",",
        "         \"http_authenticators\" : \"basic\",",
        "         \"authorizers\" : \"local\",",
        "         \"agent_reregister_timeout\" : \"10mins\",",
        "         \"logging_level\" : \"INFO\",",
        "         \"help\" : \"false\",",
        "         \"root_submissions\" : \"true\",",
        "         \"ip\" : \"127.0.0.1\",",
        "         \"user_sorter\" : \"drf\",",
        "         \"version\" : \"false\",",
        "         \"max_agent_ping_timeouts\" : \"5\",",
        "         \"agent_ping_timeout\" : \"15secs\",",
        "         \"registry_store_timeout\" : \"20secs\",",
        "         \"max_completed_frameworks\" : \"50\",",
        "         \"quiet\" : \"false\",",
        "         \"allocator\" : \"HierarchicalDRF\",",
        "         \"hostname_lookup\" : \"true\",",
        "         \"authenticators\" : \"crammd5\",",
        "         \"max_completed_tasks_per_framework\" : \"1000\",",
        "         \"registry\" : \"replicated_log\",",
        "         \"registry_strict\" : \"false\",",
        "         \"log_auto_initialize\" : \"true\",",
        "         \"authenticate_agents\" : \"false\",",
        "         \"registry_fetch_timeout\" : \"1mins\",",
        "         \"allocation_interval\" : \"1secs\",",
        "         \"authenticate_http\" : \"false\",",
        "         \"port\" : \"5050\",",
        "         \"zk_session_timeout\" : \"10secs\",",
        "         \"recovery_agent_removal_limit\" : \"100%\",",
        "         \"webui_dir\" : \"/path/to/mesos/build/../src/webui\",",
        "         \"cluster\" : \"mycluster\",",
        "         \"leader\" : \"master@127.0.0.1:5050\",",
        "         \"log_dir\" : \"/var/log\",",
        "         \"external_log_file\" : \"mesos.log\"",
        "    },",
        "    \"slaves\" : [],",
        "    \"frameworks\" : [],",
        "    \"completed_frameworks\" : [],",
        "    \"orphan_tasks\" : [],",
        "    \"unregistered_frameworks\" : []",
        "}",
        "```"),
    AUTHENTICATION(true),
    AUTHORIZATION(
        "This endpoint might be filtered based on the user accessing it.",
        "For example a user might only see the subset of frameworks,",
        "tasks, and executors they are allowed to view.",
        "See the authorization documentation for details."));
}


Future<Response> Master::Http::state(
    const Request& request,
    const Option<Principal>& principal) const
{
  // TODO(greggomann): Remove this check once the `Principal` type is used in
  // `ReservationInfo`, `DiskInfo`, and within the master's `principals` map.
  // See MESOS-7202.
  if (principal.isSome() && principal->value.isNone()) {
    return Forbidden(
        "The request's authenticated principal contains claims, but no value "
        "string. The master currently requires that principals have a value");
  }

  // When current master is not the leader, redirect to the leading master.
  if (!master->elected()) {
    return redirect(request);
  }

  // TODO(alexr): De-duplicate response processing when the principal is
  // identical, e.g., if "bob" asks for state three times in one batch,
  // ideally we only compute the response for "bob" once since they're all
  // identical within a principal.
  return ObjectApprovers::create(
      master->authorizer,
      principal,
      {VIEW_ROLE, VIEW_FRAMEWORK, VIEW_TASK, VIEW_EXECUTOR, VIEW_FLAGS})
    .then(defer(
        master->self(),
        [this, request](const Owned<ObjectApprovers>& approvers) {
          return deferBatchedRequest(
              &Master::ReadOnlyHandler::state,
              request,
              approvers);
        }));
}


Future<Response> Master::Http::deferBatchedRequest(
    ReadOnlyRequestHandler handler,
    const Request& request,
    const Owned<ObjectApprovers>& approvers) const
{
  bool scheduleBatch = batchedRequests.empty();

  // Add an element to the batched state requests.
  Promise<Response> promise;
  Future<Response> future = promise.future();
  batchedRequests.push_back(
      BatchedRequest{handler, request, approvers, std::move(promise)});

  // Schedule processing of batched requests if not yet scheduled.
  if (scheduleBatch) {
    dispatch(master->self(), [this]() {
      processRequestsBatch();
    });
  }

  return future;
}


void Master::Http::processRequestsBatch() const
{
  CHECK(!batchedRequests.empty())
    << "Bug in state batching logic: No requests to process";

  // Produce the responses in parallel.
  //
  // TODO(alexr): Consider abstracting this into `parallel_async` or
  // `foreach_parallel`, see MESOS-8587.
  //
  // TODO(alexr): Consider moving `BatchedStateRequest`'s fields into
  // `process::async` once it supports moving.
  foreach (BatchedRequest& request, batchedRequests) {
    request.promise.associate(process::async(
        [this](ReadOnlyRequestHandler handler,
               const process::http::Request& request,
               const process::Owned<ObjectApprovers>& approvers) {
          return (readonlyHandler.*handler)(request, approvers);
        },
        request.handler,
        request.request,
        request.approvers));
  }

  // Block the master actor until all workers have generated state responses.
  // It is crucial not to allow the master actor to continue and possibly
  // modify its state while a worker is reading it.
  //
  // NOTE: There is the potential for deadlock since we are blocking 1 working
  // thread here, see MESOS-8256.
  vector<Future<Response>> responses;
  foreach (const BatchedRequest& request, batchedRequests) {
    responses.push_back(request.promise.future());
  }
  process::await(responses).await();

  batchedRequests.clear();
}


Future<Response> Master::Http::readFile(
    const mesos::master::Call& call,
    const Option<Principal>& principal,
    ContentType contentType) const
{
  CHECK_EQ(mesos::master::Call::READ_FILE, call.type());

  const size_t offset = call.read_file().offset();
  const string& path = call.read_file().path();

  Option<size_t> length;
  if (call.read_file().has_length()) {
    length = call.read_file().length();
  }

  return master->files->read(offset, length, path, principal)
    .then([contentType](const Try<tuple<size_t, string>, FilesError>& result)
        -> Future<Response> {
      if (result.isError()) {
        const FilesError& error = result.error();

        switch (error.type) {
          case FilesError::Type::INVALID:
            return BadRequest(error.message);

          case FilesError::Type::UNAUTHORIZED:
            return Forbidden(error.message);

          case FilesError::Type::NOT_FOUND:
            return NotFound(error.message);

          case FilesError::Type::UNKNOWN:
            return InternalServerError(error.message);
        }

        UNREACHABLE();
      }

      mesos::master::Response response;
      response.set_type(mesos::master::Response::READ_FILE);

      response.mutable_read_file()->set_size(std::get<0>(result.get()));
      response.mutable_read_file()->set_data(std::get<1>(result.get()));

      return OK(serialize(contentType, evolve(response)),
                stringify(contentType));
    });
}


string Master::Http::STATESUMMARY_HELP()
{
  return HELP(
    TLDR(
        "Summary of agents, tasks, and registered frameworks in cluster."),
    DESCRIPTION(
        "Returns 200 OK when a summary of the master's state was queried",
        "successfully.",
        "",
        "Returns 307 TEMPORARY_REDIRECT redirect to the leading master when",
        "current master is not the leader.",
        "",
        "Returns 503 SERVICE_UNAVAILABLE if the leading master cannot be",
        "found.",
        "",
        "This endpoint gives a summary of the agents, tasks, and",
        "registered frameworks in the cluster as a JSON object.",
        "The information shown might be filtered based on the user",
        "accessing the endpoint."),
    AUTHENTICATION(true),
    AUTHORIZATION(
        "This endpoint might be filtered based on the user accessing it.",
        "For example a user might only see the subset of frameworks",
        "they are allowed to view.",
        "See the authorization documentation for details."));
}


Future<Response> Master::Http::stateSummary(
    const Request& request,
    const Option<Principal>& principal) const
{
  // TODO(greggomann): Remove this check once the `Principal` type is used in
  // `ReservationInfo`, `DiskInfo`, and within the master's `principals` map.
  // See MESOS-7202.
  if (principal.isSome() && principal->value.isNone()) {
    return Forbidden(
        "The request's authenticated principal contains claims, but no value "
        "string. The master currently requires that principals have a value");
  }

  // When current master is not the leader, redirect to the leading master.
  if (!master->elected()) {
    return redirect(request);
  }

  return ObjectApprovers::create(
      master->authorizer,
      principal,
      {VIEW_ROLE, VIEW_FRAMEWORK})
    .then(defer(
        master->self(),
        [this, request](const Owned<ObjectApprovers>& approvers) {
          return deferBatchedRequest(
              &Master::ReadOnlyHandler::stateSummary, request, approvers);
        }));
}


string Master::Http::ROLES_HELP()
{
  return HELP(
    TLDR(
        "Information about roles."),
    DESCRIPTION(
        "Returns 200 OK when information about roles was queried successfully.",
        "",
        "Returns 307 TEMPORARY_REDIRECT redirect to the leading master when",
        "current master is not the leader.",
        "",
        "Returns 503 SERVICE_UNAVAILABLE if the leading master cannot be",
        "found.",
        "",
        "This endpoint provides information about roles as a JSON object.",
        "It returns information about every role that is on the role",
        "whitelist (if enabled), has one or more registered frameworks,",
        "or has a non-default weight or quota. For each role, it returns",
        "the weight, total allocated resources, and registered frameworks."),
    AUTHENTICATION(true));
}


Future<Response> Master::Http::roles(
    const Request& request,
    const Option<Principal>& principal) const
{
  // TODO(greggomann): Remove this check once the `Principal` type is used in
  // `ReservationInfo`, `DiskInfo`, and within the master's `principals` map.
  // See MESOS-7202.
  if (principal.isSome() && principal->value.isNone()) {
    return Forbidden(
        "The request's authenticated principal contains claims, but no value "
        "string. The master currently requires that principals have a value");
  }

  // When current master is not the leader, redirect to the leading master.
  if (!master->elected()) {
    return redirect(request);
  }

  return ObjectApprovers::create(master->authorizer, principal, {VIEW_ROLE})
    .then(defer(master->self(),
        [this, request](const Owned<ObjectApprovers>& approvers) {
            return deferBatchedRequest(
                &Master::ReadOnlyHandler::roles,
                request,
                approvers);
          }));
}


Future<Response> Master::Http::listFiles(
    const mesos::master::Call& call,
    const Option<Principal>& principal,
    ContentType contentType) const
{
  CHECK_EQ(mesos::master::Call::LIST_FILES, call.type());

  const string& path = call.list_files().path();

  return master->files->browse(path, principal)
    .then([contentType](const Try<list<FileInfo>, FilesError>& result)
      -> Future<Response> {
      if (result.isError()) {
        const FilesError& error = result.error();

        switch (error.type) {
          case FilesError::Type::INVALID:
            return BadRequest(error.message);

          case FilesError::Type::UNAUTHORIZED:
            return Forbidden(error.message);

          case FilesError::Type::NOT_FOUND:
            return NotFound(error.message);

          case FilesError::Type::UNKNOWN:
            return InternalServerError(error.message);
        }

        UNREACHABLE();
      }

      mesos::master::Response response;
      response.set_type(mesos::master::Response::LIST_FILES);

      mesos::master::Response::ListFiles* listFiles =
        response.mutable_list_files();

      foreach (const FileInfo& fileInfo, result.get()) {
        listFiles->add_file_infos()->CopyFrom(fileInfo);
      }

      return OK(serialize(contentType, evolve(response)),
                stringify(contentType));
    });
}


// This duplicates the functionality offered by `roles()`. This was necessary
// as the JSON object returned by `roles()` was not specified in a formal way
// i.e. via a corresponding protobuf object and would have been very hard to
// convert back into a `Resource` object.
Future<Response> Master::Http::getRoles(
    const mesos::master::Call& call,
    const Option<Principal>& principal,
    ContentType contentType) const
{
  CHECK_EQ(mesos::master::Call::GET_ROLES, call.type());
  return ObjectApprovers::create(master->authorizer, principal, {VIEW_ROLE})
    .then(defer(master->self(),
        [this, contentType](const Owned<ObjectApprovers>& approvers)
          -> Response {
      const vector<string> filteredRoles = master->filterRoles(approvers);

      mesos::master::Response response;
      response.set_type(mesos::master::Response::GET_ROLES);

      mesos::master::Response::GetRoles* getRoles =
        response.mutable_get_roles();

      foreach (const string& name, filteredRoles) {
        mesos::Role role;

        if (master->weights.contains(name)) {
          role.set_weight(master->weights[name]);
        } else {
          role.set_weight(1.0);
        }

        if (master->roles.contains(name)) {
          Role* role_ = master->roles.at(name);

          role.mutable_resources()->CopyFrom(role_->allocatedResources());

          foreachkey (const FrameworkID& frameworkId, role_->frameworks) {
            role.add_frameworks()->CopyFrom(frameworkId);
          }
        }

        role.set_name(name);

        getRoles->add_roles()->CopyFrom(role);
      }

      return OK(serialize(contentType, evolve(response)),
                stringify(contentType));
    }));
}


string Master::Http::TEARDOWN_HELP()
{
  return HELP(
    TLDR(
        "Tears down a running framework by shutting down all tasks/executors "
        "and removing the framework."),
    DESCRIPTION(
        "Returns 200 OK if the framework was torn down successfully.",
        "",
        "Returns 307 TEMPORARY_REDIRECT redirect to the leading master when",
        "current master is not the leader.",
        "",
        "Returns 503 SERVICE_UNAVAILABLE if the leading master cannot be",
        "found.",
        "",
        "Please provide a \"frameworkId\" value designating the running",
        "framework to tear down."),
    AUTHENTICATION(true),
    AUTHORIZATION(
        "Using this endpoint to teardown frameworks requires that the",
        "current principal is authorized to teardown frameworks created",
        "by the principal who created the framework.",
        "See the authorization documentation for details."));
}


Future<Response> Master::Http::teardown(
    const Request& request,
    const Option<Principal>& principal) const
{
  // TODO(greggomann): Remove this check once the `Principal` type is used in
  // `ReservationInfo`, `DiskInfo`, and within the master's `principals` map.
  // See MESOS-7202.
  if (principal.isSome() && principal->value.isNone()) {
    return Forbidden(
        "The request's authenticated principal contains claims, but no value "
        "string. The master currently requires that principals have a value");
  }

  // When current master is not the leader, redirect to the leading master.
  if (!master->elected()) {
    return redirect(request);
  }

  if (request.method != "POST") {
    return MethodNotAllowed({"POST"}, request.method);
  }

  // Parse the query string in the request body (since this is a POST)
  // in order to determine the framework ID to shutdown.
  Try<hashmap<string, string>> decode =
    process::http::query::decode(request.body);

  if (decode.isError()) {
    return BadRequest("Unable to decode query string: " + decode.error());
  }

  const hashmap<string, string>& values = decode.get();

  Option<string> value = values.get("frameworkId");
  if (value.isNone()) {
    return BadRequest(
        "Missing 'frameworkId' query parameter in the request body");
  }

  FrameworkID id;
  id.set_value(value.get());

  return _teardown(id, principal);
}


Future<Response> Master::Http::_teardown(
    const FrameworkID& id,
    const Option<Principal>& principal) const
{
  Framework* framework = master->getFramework(id);

  if (framework == nullptr) {
    return BadRequest("No framework found with specified ID");
  }

  // Skip authorization if no ACLs were provided to the master.
  if (master->authorizer.isNone()) {
    return __teardown(id);
  }

  authorization::Request teardown;
  teardown.set_action(authorization::TEARDOWN_FRAMEWORK);

  Option<authorization::Subject> subject = createSubject(principal);
  if (subject.isSome()) {
    teardown.mutable_subject()->CopyFrom(subject.get());
  }

  if (framework->info.has_principal()) {
    teardown.mutable_object()->mutable_framework_info()->CopyFrom(
        framework->info);
    teardown.mutable_object()->set_value(framework->info.principal());
  }

  return master->authorizer.get()->authorized(teardown)
    .then(defer(master->self(), [=](bool authorized) -> Future<Response> {
      if (!authorized) {
        return Forbidden();
      }

      return __teardown(id);
    }));
}


Future<Response> Master::Http::__teardown(const FrameworkID& id) const
{
  Framework* framework = master->getFramework(id);

  if (framework == nullptr) {
    return BadRequest("No framework found with ID " + stringify(id));
  }

  // TODO(ijimenez): Do 'removeFramework' asynchronously.
  master->removeFramework(framework);

  return OK();
}


Future<Response> Master::Http::teardown(
    const mesos::master::Call& call,
    const Option<Principal>& principal,
    ContentType contentType) const
{
  CHECK_EQ(mesos::master::Call::TEARDOWN, call.type());

  return _teardown(call.teardown().framework_id(), principal);
}


Future<Response> Master::Http::getOperations(
    const mesos::master::Call& call,
    const Option<Principal>& principal,
    ContentType contentType) const
{
  CHECK_EQ(mesos::master::Call::GET_OPERATIONS, call.type());

  return ObjectApprovers::create(master->authorizer, principal, {VIEW_ROLE})
    .then(defer(
        master->self(),
        [=](const Owned<ObjectApprovers>& approvers) -> Response {
          // We consider a principal to be authorized to view an operation if it
          // is authorized to view the resources the operation is performed on.
          auto approved = [&approvers](const Operation& operation) {
            Try<Resources> consumedResources =
              protobuf::getConsumedResources(operation.info());

            if (consumedResources.isError()) {
              LOG(WARNING)
                << "Could not approve operation " << operation.uuid()
                << " since its consumed resources could not be determined:"
                << consumedResources.error();

              return false;
            }

            foreach (const Resource& resource, consumedResources.get()) {
              if (!approvers->approved<VIEW_ROLE>(resource)) {
                return false;
              }
            }

            return true;
          };

          mesos::master::Response response;
          response.set_type(mesos::master::Response::GET_OPERATIONS);

          mesos::master::Response::GetOperations* operations =
            response.mutable_get_operations();

          foreachvalue (const Slave* slave, master->slaves.registered) {
            foreachvalue (Operation* operation, slave->operations) {
              if (approved(*operation)) {
                operations->add_operations()->CopyFrom(*operation);
              }
            }

            foreachvalue (
                const Slave::ResourceProvider resourceProvider,
                slave->resourceProviders) {
              foreachvalue (Operation* operation, resourceProvider.operations) {
                if (approved(*operation)) {
                  operations->add_operations()->CopyFrom(*operation);
                }
              }
            }
          }

          return OK(
              serialize(contentType, evolve(response)),
              stringify(contentType));
        }));
}


string Master::Http::TASKS_HELP()
{
  return HELP(
    TLDR(
        "Lists tasks from all active frameworks."),
    DESCRIPTION(
        "Returns 200 OK when task information was queried successfully.",
        "",
        "Returns 307 TEMPORARY_REDIRECT redirect to the leading master when",
        "current master is not the leader.",
        "",
        "Returns 503 SERVICE_UNAVAILABLE if the leading master cannot be",
        "found.",
        "",
        "Lists known tasks.",
        "The information shown might be filtered based on the user",
        "accessing the endpoint.",
        "",
        "Query parameters:",
        "",
        ">        framework_id=VALUE   Only return tasks belonging to the "
        "framework with this ID.",
        ">        limit=VALUE          Maximum number of tasks returned "
        "(default is " + stringify(TASK_LIMIT) + ").",
        ">        offset=VALUE         Starts task list at offset.",
        ">        order=(asc|desc)     Ascending or descending sort order "
        "(default is descending).",
        ">        task_id=VALUE        Only return tasks with this ID "
        "(should be used together with parameter 'framework_id')."
        ""),
    AUTHENTICATION(true),
    AUTHORIZATION(
        "This endpoint might be filtered based on the user accessing it.",
        "For example a user might only see the subset of tasks they are",
        "allowed to view.",
        "See the authorization documentation for details."));
}


Future<Response> Master::Http::tasks(
    const Request& request,
    const Option<Principal>& principal) const
{
  // TODO(greggomann): Remove this check once the `Principal` type is used in
  // `ReservationInfo`, `DiskInfo`, and within the master's `principals` map.
  // See MESOS-7202.
  if (principal.isSome() && principal->value.isNone()) {
    return Forbidden(
        "The request's authenticated principal contains claims, but no value "
        "string. The master currently requires that principals have a value");
  }

  // When current master is not the leader, redirect to the leading master.
  if (!master->elected()) {
    return redirect(request);
  }

  return ObjectApprovers::create(
      master->authorizer,
      principal,
      {VIEW_FRAMEWORK, VIEW_TASK})
    .then(defer(
        master->self(),
        [this, request](const Owned<ObjectApprovers>& approvers) {
          return deferBatchedRequest(
              &Master::ReadOnlyHandler::tasks, request, approvers);
        }));
}


Future<Response> Master::Http::getTasks(
    const mesos::master::Call& call,
    const Option<Principal>& principal,
    ContentType contentType) const
{
  CHECK_EQ(mesos::master::Call::GET_TASKS, call.type());

  return ObjectApprovers::create(
      master->authorizer,
      principal,
      {VIEW_FRAMEWORK, VIEW_TASK})
    .then(defer(
        master->self(),
        [=](const Owned<ObjectApprovers>& approvers) -> Response {
          mesos::master::Response response;
          response.set_type(mesos::master::Response::GET_TASKS);

          *response.mutable_get_tasks() = _getTasks(approvers);

          return OK(
              serialize(contentType, evolve(response)), stringify(contentType));
  }));
}


mesos::master::Response::GetTasks Master::Http::_getTasks(
    const Owned<ObjectApprovers>& approvers) const
{
  // Construct framework list with both active and completed frameworks.
  vector<const Framework*> frameworks;
  foreachvalue (Framework* framework, master->frameworks.registered) {
    // Skip unauthorized frameworks.
    if (!approvers->approved<VIEW_FRAMEWORK>(framework->info)) {
      continue;
    }

    frameworks.push_back(framework);
  }

  foreachvalue (const Owned<Framework>& framework,
                master->frameworks.completed) {
    // Skip unauthorized frameworks.
    if (!approvers->approved<VIEW_FRAMEWORK>(framework->info)) {
      continue;
    }

    frameworks.push_back(framework.get());
  }

  mesos::master::Response::GetTasks getTasks;

  vector<const Task*> tasks;
  foreach (const Framework* framework, frameworks) {
    // Pending tasks.
    foreachvalue (const TaskInfo& taskInfo, framework->pendingTasks) {
      // Skip unauthorized tasks.
      if (!approvers->approved<VIEW_TASK>(taskInfo, framework->info)) {
        continue;
      }

      *getTasks.add_pending_tasks() =
        protobuf::createTask(taskInfo, TASK_STAGING, framework->id());
    }

    // Active tasks.
    foreachvalue (Task* task, framework->tasks) {
      CHECK_NOTNULL(task);
      // Skip unauthorized tasks.
      if (!approvers->approved<VIEW_TASK>(*task, framework->info)) {
        continue;
      }

      getTasks.add_tasks()->CopyFrom(*task);
    }

    // Unreachable tasks.
    foreachvalue (const Owned<Task>& task, framework->unreachableTasks) {
      // Skip unauthorized tasks.
      if (!approvers->approved<VIEW_TASK>(*task, framework->info)) {
        continue;
      }

      getTasks.add_unreachable_tasks()->CopyFrom(*task);
    }

    // Completed tasks.
    foreach (const Owned<Task>& task, framework->completedTasks) {
      // Skip unauthorized tasks.
      if (!approvers->approved<VIEW_TASK>(*task, framework->info)) {
        continue;
      }

      getTasks.add_completed_tasks()->CopyFrom(*task);
    }
  }

  return getTasks;
}


// /master/maintenance/schedule endpoint help.
string Master::Http::MAINTENANCE_SCHEDULE_HELP()
{
  return HELP(
    TLDR(
        "Returns or updates the cluster's maintenance schedule."),
    DESCRIPTION(
        "Returns 200 OK when the requested maintenance operation was performed",
        "successfully.",
        "",
        "Returns 307 TEMPORARY_REDIRECT redirect to the leading master when",
        "current master is not the leader.",
        "",
        "Returns 503 SERVICE_UNAVAILABLE if the leading master cannot be",
        "found.",
        "",
        "GET: Returns the current maintenance schedule as JSON.",
        "",
        "POST: Validates the request body as JSON",
        "and updates the maintenance schedule."),
    AUTHENTICATION(true),
    AUTHORIZATION(
        "GET: The response will contain only the maintenance schedule for",
        "those machines the current principal is allowed to see. If none",
        "an empty response will be returned.",
        "",
        "POST: The current principal must be authorized to modify the",
        "maintenance schedule of all the machines in the request. If the",
        "principal is unauthorized to modify the schedule for at least one",
        "machine, the whole request will fail."));
}


// /master/maintenance/schedule endpoint handler.
Future<Response> Master::Http::maintenanceSchedule(
    const Request& request,
    const Option<Principal>& principal) const
{
  // When current master is not the leader, redirect to the leading master.
  if (!master->elected()) {
    return redirect(request);
  }

  if (request.method != "GET" && request.method != "POST") {
    return MethodNotAllowed({"GET", "POST"}, request.method);
  }

  // JSON-ify and return the current maintenance schedule.
  if (request.method == "GET") {
    Option<string> jsonp = request.url.query.get("jsonp");

    return ObjectApprovers::create(
        master->authorizer,
        principal,
        {GET_MAINTENANCE_SCHEDULE})
      .then(defer(
        master->self(),
        [this, jsonp](const Owned<ObjectApprovers>& approvers) -> Response {
          const mesos::maintenance::Schedule schedule =
            _getMaintenanceSchedule(approvers);
          return OK(JSON::protobuf(schedule), jsonp);
        }));
  }

  // Parse the POST body as JSON.
  Try<JSON::Object> jsonSchedule = JSON::parse<JSON::Object>(request.body);
  if (jsonSchedule.isError()) {
    return BadRequest(jsonSchedule.error());
  }

  // Convert the schedule to a protobuf.
  Try<mesos::maintenance::Schedule> protoSchedule =
    ::protobuf::parse<mesos::maintenance::Schedule>(jsonSchedule.get());

  if (protoSchedule.isError()) {
    return BadRequest(protoSchedule.error());
  }

  return _updateMaintenanceSchedule(protoSchedule.get(), principal);
}


mesos::maintenance::Schedule Master::Http::_getMaintenanceSchedule(
    const Owned<ObjectApprovers>& approvers) const
{
  // TODO(josephw): Return more than one schedule.
  if (master->maintenance.schedules.empty()) {
    return mesos::maintenance::Schedule();
  }

  mesos::maintenance::Schedule schedule;

  foreach (const mesos::maintenance::Window& window,
           master->maintenance.schedules.front().windows()) {
    mesos::maintenance::Window window_;

    foreach (const MachineID& machine_id, window.machine_ids()) {
      if (!approvers->approved<GET_MAINTENANCE_SCHEDULE>(machine_id)) {
        continue;
      }

      window_.add_machine_ids()->CopyFrom(machine_id);
    }

    if (window_.machine_ids_size() > 0) {
      window_.mutable_unavailability()->CopyFrom(window.unavailability());
      schedule.add_windows()->CopyFrom(window_);
    }
  }

  return schedule;
}


Future<Response> Master::Http::_updateMaintenanceSchedule(
    const mesos::maintenance::Schedule& schedule,
    const Option<process::http::authentication::Principal>& principal) const
{
  // Validate that the schedule only transitions machines between
  // `UP` and `DRAINING` modes.
  Try<Nothing> isValid = maintenance::validation::schedule(
      schedule,
      master->machines);

  if (isValid.isError()) {
    return BadRequest(isValid.error());
  }

  // TODO(alexr): Consider pulling this higher above before we even start
  // parsing request body.
  return ObjectApprovers::create(
      master->authorizer,
      principal,
      {UPDATE_MAINTENANCE_SCHEDULE})
    .then(defer(
      master->self(),
      [this, schedule](const Owned<ObjectApprovers>& approvers) {
        return __updateMaintenanceSchedule(schedule, approvers);
      }));
}

Future<Response> Master::Http::__updateMaintenanceSchedule(
    const mesos::maintenance::Schedule& schedule,
    const Owned<ObjectApprovers>& approvers) const
{
  foreach (const mesos::maintenance::Window& window, schedule.windows()) {
    foreach (const MachineID& machine, window.machine_ids()) {
      if (!approvers->approved<UPDATE_MAINTENANCE_SCHEDULE>(machine)) {
        return Forbidden();
      }
    }
  }

  return master->registrar->apply(Owned<RegistryOperation>(
      new maintenance::UpdateSchedule(schedule)))
    .onAny([](const Future<bool>& result) {
      // TODO(fiu): Consider changing/refactoring the registrar itself
      // so the individual call sites don't need to handle this separately.
      // All registrar failures that cause it to abort should instead
      // abort the process.
      CHECK_READY(result)
        << "Failed to update maintenance schedule in the registry";
    })
    .then(defer(master->self(), [this, schedule](bool result) {
      return ___updateMaintenanceSchedule(schedule, result);
    }));
}

Future<Response> Master::Http::___updateMaintenanceSchedule(
    const mesos::maintenance::Schedule& schedule,
    bool applied) const
{
  // See the top comment in "master/maintenance.hpp" for why this check
  // is here, and is appropriate.
  CHECK(applied);

  // Update the master's local state with the new schedule.
  //
  // NOTE: We only add or remove differences between the current schedule and
  // the new schedule.  This is because the `MachineInfo` struct holds more
  // information than a maintenance schedule. For example, the `mode` field is
  // not part of a maintenance schedule.
  //
  // TODO(josephw): allow more than one schedule.

  // Put the machines in the updated schedule into a set.
  // Save the unavailability, to help with updating some machines.
  hashmap<MachineID, Unavailability> unavailabilities;
  foreach (const mesos::maintenance::Window& window, schedule.windows()) {
    foreach (const MachineID& id, window.machine_ids()) {
      unavailabilities[id] = window.unavailability();
    }
  }

  // NOTE: Copies are needed because `updateUnavailability()` in this loop
  // modifies the container.
  foreachkey (const MachineID& id, utils::copy(master->machines)) {
    // Update the `unavailability` for each existing machine, except for
    // machines going from `UP` to `DRAINING` (handled in the next loop).
    // Each machine will only be touched by 1 of the 2 loops here to
    // avoid sending inverse offer twice for a single machine since
    // `updateUnavailability` will trigger an inverse offer.
    // TODO(gyliu513): Merge this logic with `Master::updateUnavailability`,
    // having it in two places results in more conditionals to handle.
    if (unavailabilities.contains(id)) {
      if (master->machines[id].info.mode() == MachineInfo::UP) {
        continue;
      }

      master->updateUnavailability(id, unavailabilities[id]);
      continue;
    }

    // Transition each removed machine back to the `UP` mode and remove the
    // unavailability.
    master->machines[id].info.set_mode(MachineInfo::UP);
    master->updateUnavailability(id, None());
  }

  // Save each new machine, with the unavailability
  // and starting in `DRAINING` mode.
  foreach (const mesos::maintenance::Window& window, schedule.windows()) {
    foreach (const MachineID& id, window.machine_ids()) {
      if (master->machines.contains(id) &&
          master->machines[id].info.mode() != MachineInfo::UP) {
        continue;
      }

      MachineInfo info;
      info.mutable_id()->CopyFrom(id);
      info.set_mode(MachineInfo::DRAINING);

      master->machines[id].info.CopyFrom(info);

      master->updateUnavailability(id, window.unavailability());
    }
  }

  // Replace the old schedule(s) with the new schedule.
  master->maintenance.schedules.clear();
  master->maintenance.schedules.push_back(schedule);

  return OK();
}


Future<Response> Master::Http::getMaintenanceSchedule(
    const mesos::master::Call& call,
    const Option<Principal>& principal,
    ContentType contentType) const
{
  CHECK_EQ(mesos::master::Call::GET_MAINTENANCE_SCHEDULE, call.type());

  return ObjectApprovers::create(
      master->authorizer,
      principal,
      {GET_MAINTENANCE_SCHEDULE})
    .then(defer(
      master->self(),
      [this, contentType](const Owned<ObjectApprovers>& approvers) -> Response {
        mesos::master::Response response;

        response.set_type(mesos::master::Response::GET_MAINTENANCE_SCHEDULE);

        *response.mutable_get_maintenance_schedule()->mutable_schedule() =
          _getMaintenanceSchedule(approvers);

        return OK(serialize(contentType, evolve(response)),
                  stringify(contentType));
      }));
}


Future<Response> Master::Http::updateMaintenanceSchedule(
    const mesos::master::Call& call,
    const Option<Principal>& principal,
    ContentType /*contentType*/) const
{
  CHECK_EQ(mesos::master::Call::UPDATE_MAINTENANCE_SCHEDULE, call.type());
  CHECK(call.has_update_maintenance_schedule());

  mesos::maintenance::Schedule schedule =
    call.update_maintenance_schedule().schedule();

  return _updateMaintenanceSchedule(schedule, principal);
}


// /master/machine/down endpoint help.
string Master::Http::MACHINE_DOWN_HELP()
{
  return HELP(
    TLDR(
        "Brings a set of machines down."),
    DESCRIPTION(
        "Returns 200 OK when the operation was successful.",
        "",
        "Returns 307 TEMPORARY_REDIRECT redirect to the leading master when",
        "current master is not the leader.",
        "",
        "Returns 503 SERVICE_UNAVAILABLE if the leading master cannot be",
        "found.",
        "",
        "POST: Validates the request body as JSON and transitions",
        "  the list of machines into DOWN mode.  Currently, only",
        "  machines in DRAINING mode are allowed to be brought down."),
    AUTHENTICATION(true),
    AUTHORIZATION(
        "The current principal must be allowed to bring down all the machines",
        "in the request, otherwise the request will fail."));
}


// /master/machine/down endpoint handler.
Future<Response> Master::Http::machineDown(
    const Request& request,
    const Option<Principal>& principal) const
{
  // When current master is not the leader, redirect to the leading master.
  if (!master->elected()) {
    return redirect(request);
  }

  if (request.method != "POST") {
    return MethodNotAllowed({"POST"}, request.method);
  }

  // Parse the POST body as JSON.
  Try<JSON::Array> jsonIds = JSON::parse<JSON::Array>(request.body);
  if (jsonIds.isError()) {
    return BadRequest(jsonIds.error());
  }

  // Convert the machines to a protobuf.
  auto ids = ::protobuf::parse<RepeatedPtrField<MachineID>>(jsonIds.get());
  if (ids.isError()) {
    return BadRequest(ids.error());
  }

  return ObjectApprovers::create(
      master->authorizer,
      principal,
      {START_MAINTENANCE})
    .then(defer(
      master->self(),
      [this, ids](const Owned<ObjectApprovers>& approvers) {
        return _startMaintenance(ids.get(), approvers);
      }));
}


Future<Response> Master::Http::_startMaintenance(
    const RepeatedPtrField<MachineID>& machineIds,
    const Owned<ObjectApprovers>& approvers) const
{
  // Validate every machine in the list.
  Try<Nothing> isValid = maintenance::validation::machines(machineIds);
  if (isValid.isError()) {
    return BadRequest(isValid.error());
  }

  // Check that all machines are part of a maintenance schedule.
  // TODO(josephw): Allow a transition from `UP` to `DOWN`.
  foreach (const MachineID& id, machineIds) {
    if (!master->machines.contains(id)) {
      return BadRequest(
          "Machine '" + stringify(JSON::protobuf(id)) +
            "' is not part of a maintenance schedule");
    }

    if (master->machines[id].info.mode() != MachineInfo::DRAINING) {
      return BadRequest(
          "Machine '" + stringify(JSON::protobuf(id)) +
            "' is not in DRAINING mode and cannot be brought down");
    }

    if (!approvers->approved<START_MAINTENANCE>(id)) {
      return Forbidden();
    }
  }

  return master->registrar->apply(Owned<RegistryOperation>(
      new maintenance::StartMaintenance(machineIds)))
    .then(defer(master->self(), [=](bool result) -> Response {
      // See the top comment in "master/maintenance.hpp" for why this check
      // is here, and is appropriate.
      CHECK(result);

      // We currently send a `ShutdownMessage` to each slave. This terminates
      // all the executors for all the frameworks running on that slave.
      // We also manually remove the slave to force sending TASK_LOST updates
      // for all the tasks that were running on the slave and `LostSlaveMessage`
      // messages to the framework. This guards against the slave having dropped
      // the `ShutdownMessage`.
      foreach (const MachineID& machineId, machineIds) {
        // The machine may not be in machines. This means no slaves are
        // currently registered on that machine so this is a no-op.
        if (master->machines.contains(machineId)) {
          // NOTE: Copies are needed because removeSlave modifies
          // master->machines.
          foreach (
              const SlaveID& slaveId,
              utils::copy(master->machines[machineId].slaves)) {
            Slave* slave = master->slaves.registered.get(slaveId);
            CHECK_NOTNULL(slave);

            // Tell the slave to shut down.
            ShutdownMessage shutdownMessage;
            shutdownMessage.set_message("Operator initiated 'Machine DOWN'");
            master->send(slave->pid, shutdownMessage);

            // Immediately remove the slave to force sending `TASK_LOST` status
            // updates as well as `LostSlaveMessage` messages to the frameworks.
            // See comment above.
            master->removeSlave(slave, "Operator initiated 'Machine DOWN'");
          }
        }
      }

      // Update the master's local state with the downed machines.
      foreach (const MachineID& id, machineIds) {
        master->machines[id].info.set_mode(MachineInfo::DOWN);
      }

      return OK();
    }));
}


Future<Response> Master::Http::startMaintenance(
    const mesos::master::Call& call,
    const Option<Principal>& principal,
    ContentType /*contentType*/) const
{
  CHECK_EQ(mesos::master::Call::START_MAINTENANCE, call.type());
  CHECK(call.has_start_maintenance());

  RepeatedPtrField<MachineID> machineIds = call.start_maintenance().machines();

  return ObjectApprovers::create(
      master->authorizer,
      principal,
      {START_MAINTENANCE})
    .then(defer(
      master->self(),
      [this, machineIds](const Owned<ObjectApprovers>& approvers) {
        return _startMaintenance(machineIds, approvers);
      }));
}


// /master/machine/up endpoint help.
string Master::Http::MACHINE_UP_HELP()
{
  return HELP(
    TLDR(
        "Brings a set of machines back up."),
    DESCRIPTION(
        "Returns 200 OK when the operation was successful.",
        "",
        "Returns 307 TEMPORARY_REDIRECT redirect to the leading master when",
        "current master is not the leader.",
        "",
        "Returns 503 SERVICE_UNAVAILABLE if the leading master cannot be",
        "found.",
        "",
        "POST: Validates the request body as JSON and transitions",
        "  the list of machines into UP mode.  This also removes",
        "  the list of machines from the maintenance schedule."),
    AUTHENTICATION(true),
    AUTHORIZATION(
        "The current principal must be allowed to bring up all the machines",
        "in the request, otherwise the request will fail."));
}


// /master/machine/up endpoint handler.
Future<Response> Master::Http::machineUp(
    const Request& request,
    const Option<Principal>& principal) const
{
  // When current master is not the leader, redirect to the leading master.
  if (!master->elected()) {
    return redirect(request);
  }

  if (request.method != "POST") {
    return MethodNotAllowed({"POST"}, request.method);
  }

  // Parse the POST body as JSON.
  Try<JSON::Array> jsonIds = JSON::parse<JSON::Array>(request.body);
  if (jsonIds.isError()) {
    return BadRequest(jsonIds.error());
  }

  // Convert the machines to a protobuf.
  auto ids = ::protobuf::parse<RepeatedPtrField<MachineID>>(jsonIds.get());
  if (ids.isError()) {
    return BadRequest(ids.error());
  }

  return ObjectApprovers::create(
      master->authorizer,
      principal,
      {STOP_MAINTENANCE})
    .then(defer(
      master->self(),
      [this, ids](const Owned<ObjectApprovers>& approvers) {
        return _stopMaintenance(ids.get(), approvers);
      }));
}


Future<Response> Master::Http::_stopMaintenance(
    const RepeatedPtrField<MachineID>& machineIds,
    const Owned<ObjectApprovers>& approvers) const
{
  // Validate every machine in the list.
  Try<Nothing> isValid = maintenance::validation::machines(machineIds);
  if (isValid.isError()) {
    return BadRequest(isValid.error());
  }

  // Check that all machines are part of a maintenance schedule.
  foreach (const MachineID& id, machineIds) {
    if (!master->machines.contains(id)) {
      return BadRequest(
          "Machine '" + stringify(JSON::protobuf(id)) +
            "' is not part of a maintenance schedule");
    }

    if (master->machines[id].info.mode() != MachineInfo::DOWN) {
      return BadRequest(
          "Machine '" + stringify(JSON::protobuf(id)) +
            "' is not in DOWN mode and cannot be brought up");
    }

    if (!approvers->approved<STOP_MAINTENANCE>(id)) {
      return Forbidden();
    }
  }

  return master->registrar->apply(Owned<RegistryOperation>(
      new maintenance::StopMaintenance(machineIds)))
    .then(defer(master->self(), [=](bool result) -> Future<Response> {
      // See the top comment in "master/maintenance.hpp" for why this check
      // is here, and is appropriate.
      CHECK(result);

      // Update the master's local state with the reactivated machines.
      hashset<MachineID> updated;
      foreach (const MachineID& id, machineIds) {
        master->machines[id].info.set_mode(MachineInfo::UP);
        master->machines[id].info.clear_unavailability();
        updated.insert(id);
      }

      // Delete the machines from the schedule.
      for (list<mesos::maintenance::Schedule>::iterator schedule =
          master->maintenance.schedules.begin();
          schedule != master->maintenance.schedules.end();) {
        for (int j = schedule->windows().size() - 1; j >= 0; j--) {
          mesos::maintenance::Window* window = schedule->mutable_windows(j);

          // Delete individual machines.
          for (int k = window->machine_ids().size() - 1; k >= 0; k--) {
            if (updated.contains(window->machine_ids(k))) {
              window->mutable_machine_ids()->DeleteSubrange(k, 1);
            }
          }

          // If the resulting window is empty, delete it.
          if (window->machine_ids().size() == 0) {
            schedule->mutable_windows()->DeleteSubrange(j, 1);
          }
        }

        // If the resulting schedule is empty, delete it.
        if (schedule->windows().size() == 0) {
          schedule = master->maintenance.schedules.erase(schedule);
        } else {
          ++schedule;
        }
      }

      return OK();
    }));
}


Future<Response> Master::Http::stopMaintenance(
    const mesos::master::Call& call,
    const Option<Principal>& principal,
    ContentType /*contentType*/) const
{
  CHECK_EQ(mesos::master::Call::STOP_MAINTENANCE, call.type());
  CHECK(call.has_stop_maintenance());

  RepeatedPtrField<MachineID> machineIds = call.stop_maintenance().machines();

  return ObjectApprovers::create(
      master->authorizer,
      principal,
      {STOP_MAINTENANCE})
    .then(defer(
      master->self(),
      [this, machineIds](const Owned<ObjectApprovers>& approvers) {
        return _stopMaintenance(machineIds, approvers);
      }));
}


// /master/maintenance/status endpoint help.
string Master::Http::MAINTENANCE_STATUS_HELP()
{
  return HELP(
    TLDR(
        "Retrieves the maintenance status of the cluster."),
    DESCRIPTION(
        "Returns 200 OK when the maintenance status was queried successfully.",
        "",
        "Returns 307 TEMPORARY_REDIRECT redirect to the leading master when",
        "current master is not the leader.",
        "",
        "Returns 503 SERVICE_UNAVAILABLE if the leading master cannot be",
        "found.",
        "",
        "Returns an object with one list of machines per machine mode.",
        "For draining machines, this list includes the frameworks' responses",
        "to inverse offers.",
        "**NOTE**:",
        "Inverse offer responses are cleared if the master fails over.",
        "However, new inverse offers will be sent once the master recovers."),
    AUTHENTICATION(true),
    AUTHORIZATION(
        "The response will contain only the maintenance status for those",
        "machines the current principal is allowed to see. If none, an empty",
        "response will be returned."));
}


// /master/maintenance/status endpoint handler.
Future<Response> Master::Http::maintenanceStatus(
    const Request& request,
    const Option<Principal>& principal) const
{
  // When current master is not the leader, redirect to the leading master.
  if (!master->elected()) {
    return redirect(request);
  }

  if (request.method != "GET") {
    return MethodNotAllowed({"GET"}, request.method);
  }

  Option<string> jsonp = request.url.query.get("jsonp");

  return ObjectApprovers::create(
      master->authorizer,
      principal,
      {GET_MAINTENANCE_STATUS})
    .then(defer(
      master->self(),
      [this](const Owned<ObjectApprovers>& approvers) {
        return _getMaintenanceStatus(approvers);
      }))
    .then([jsonp](const mesos::maintenance::ClusterStatus& status) -> Response {
      return OK(JSON::protobuf(status), jsonp);
    });
}


Future<mesos::maintenance::ClusterStatus> Master::Http::_getMaintenanceStatus(
    const Owned<ObjectApprovers>& approvers) const
{
  return master->allocator->getInverseOfferStatuses()
    .then(defer(
        master->self(),
        [=](hashmap<
            SlaveID,
            hashmap<FrameworkID, mesos::allocator::InverseOfferStatus>> result)
          -> Future<mesos::maintenance::ClusterStatus> {
    // Unwrap the master's machine information into two arrays of machines.
    // The data is coming from the allocator and therefore could be stale.
    // Also, if the master fails over, this data is cleared.
    mesos::maintenance::ClusterStatus status;
    foreachpair (
        const MachineID& id,
        const Machine& machine,
        master->machines) {
      if (!approvers->approved<GET_MAINTENANCE_STATUS>(id)) {
        continue;
      }

      switch (machine.info.mode()) {
        case MachineInfo::DRAINING: {
          mesos::maintenance::ClusterStatus::DrainingMachine* drainingMachine =
            status.add_draining_machines();

          drainingMachine->mutable_id()->CopyFrom(id);

          // Unwrap inverse offer status information from the allocator.
          foreach (const SlaveID& slave, machine.slaves) {
            if (result.contains(slave)) {
              foreachvalue (
                  const mesos::allocator::InverseOfferStatus& status,
                  result[slave]) {
                drainingMachine->add_statuses()->CopyFrom(status);
              }
            }
          }
          break;
        }

        case MachineInfo::DOWN: {
          status.add_down_machines()->CopyFrom(id);
          break;
        }

        // Currently, `UP` machines are not specifically tracked in the master.
        case MachineInfo::UP: {}
        default: {
          break;
        }
      }
    }

    return status;
  }));
}


Future<Response> Master::Http::getMaintenanceStatus(
    const mesos::master::Call& call,
    const Option<Principal>& principal,
    ContentType contentType) const
{
  CHECK_EQ(mesos::master::Call::GET_MAINTENANCE_STATUS, call.type());

  return ObjectApprovers::create(
      master->authorizer,
      principal,
      {GET_MAINTENANCE_STATUS})
    .then(defer(
      master->self(),
      [this](const Owned<ObjectApprovers>& approvers) {
        return _getMaintenanceStatus(approvers);
      }))
    .then([contentType](const mesos::maintenance::ClusterStatus& status)
          -> Response {
        mesos::master::Response response;
        response.set_type(mesos::master::Response::GET_MAINTENANCE_STATUS);
        response.mutable_get_maintenance_status()->mutable_status()
            ->CopyFrom(status);

        return OK(serialize(contentType, evolve(response)),
                  stringify(contentType));
      });
}


string Master::Http::UNRESERVE_HELP()
{
  return HELP(
    TLDR(
        "Unreserve resources dynamically on a specific agent."),
    DESCRIPTION(
        "Returns 202 ACCEPTED which indicates that the unreserve",
        "operation has been validated successfully by the master.",
        "",
        "Returns 307 TEMPORARY_REDIRECT redirect to the leading master when",
        "current master is not the leader.",
        "",
        "Returns 503 SERVICE_UNAVAILABLE if the leading master cannot be",
        "found.",
        "",
        "The request is then forwarded asynchronously to the Mesos",
        "agent where the reserved resources are located.",
        "That asynchronous message may not be delivered or",
        "unreserving resources at the agent might fail.",
        "",
        "Please provide \"slaveId\" and \"resources\" values describing",
        "the resources to be unreserved."),
    AUTHENTICATION(true),
    AUTHORIZATION(
        "Using this endpoint to unreserve resources requires that the",
        "current principal is authorized to unreserve resources created",
        "by the principal who reserved the resources.",
        "See the authorization documentation for details."));
}


Future<Response> Master::Http::unreserve(
    const Request& request,
    const Option<Principal>& principal) const
{
  // TODO(greggomann): Remove this check once the `Principal` type is used in
  // `ReservationInfo`, `DiskInfo`, and within the master's `principals` map.
  // See MESOS-7202.
  if (principal.isSome() && principal->value.isNone()) {
    return Forbidden(
        "The request's authenticated principal contains claims, but no value "
        "string. The master currently requires that principals have a value");
  }

  // When current master is not the leader, redirect to the leading master.
  if (!master->elected()) {
    return redirect(request);
  }

  if (request.method != "POST") {
    return MethodNotAllowed({"POST"}, request.method);
  }

  // Parse the query string in the request body.
  Try<hashmap<string, string>> decode =
    process::http::query::decode(request.body);

  if (decode.isError()) {
    return BadRequest("Unable to decode query string: " + decode.error());
  }

  const hashmap<string, string>& values = decode.get();

  Option<string> value;

  value = values.get("slaveId");
  if (value.isNone()) {
    return BadRequest("Missing 'slaveId' query parameter in the request body");
  }

  SlaveID slaveId;
  slaveId.set_value(value.get());

  value = values.get("resources");
  if (value.isNone()) {
    return BadRequest(
        "Missing 'resources' query parameter in the request body");
  }

  Try<JSON::Array> parse =
    JSON::parse<JSON::Array>(value.get());

  if (parse.isError()) {
    return BadRequest(
        "Error in parsing 'resources' query parameter in the request body: " +
        parse.error());
  }

  RepeatedPtrField<Resource> resources;
  foreach (const JSON::Value& value, parse->values) {
    Try<Resource> resource = ::protobuf::parse<Resource>(value);
    if (resource.isError()) {
      return BadRequest(
          "Error in parsing 'resources' query parameter in the request body: " +
          resource.error());
    }

    resources.Add()->CopyFrom(resource.get());
  }

  return _unreserve(slaveId, resources, principal);
}


Future<Response> Master::Http::_unreserve(
    const SlaveID& slaveId,
    const RepeatedPtrField<Resource>& resources,
    const Option<Principal>& principal) const
{
  Slave* slave = master->slaves.registered.get(slaveId);
  if (slave == nullptr) {
    return BadRequest("No agent found with specified ID");
  }

  // Create an operation.
  Offer::Operation operation;
  operation.set_type(Offer::Operation::UNRESERVE);
  operation.mutable_unreserve()->mutable_resources()->CopyFrom(resources);

  Option<Error> error = validateAndUpgradeResources(&operation);
  if (error.isSome()) {
    return BadRequest(error->message);
  }

  error = validation::operation::validate(operation.unreserve());
  if (error.isSome()) {
    return BadRequest("Invalid UNRESERVE operation: " + error->message);
  }

  return master->authorizeUnreserveResources(operation.unreserve(), principal)
    .then(defer(master->self(), [=](bool authorized) -> Future<Response> {
      if (!authorized) {
        return Forbidden();
      }

      return _operation(slaveId, operation.unreserve().resources(), operation);
    }));
}


Future<Response> Master::Http::_operation(
    const SlaveID& slaveId,
    Resources required,
    const Offer::Operation& operation) const
{
  Slave* slave = master->slaves.registered.get(slaveId);
  if (slave == nullptr) {
    return BadRequest("No agent found with specified ID");
  }

  // The resources recovered by rescinding outstanding offers.
  Resources totalRecovered;

  // We pessimistically assume that what seems like "available"
  // resources in the allocator will be gone. This can happen due to
  // the race between the allocator scheduling an 'allocate' call to
  // itself vs master's request to schedule 'updateAvailable'.
  // We greedily rescind one offer at time until we've rescinded
  // enough offers to cover 'operation'.
  foreach (Offer* offer, utils::copy(slave->offers)) {
    // If rescinding the offer would not contribute to satisfying
    // the required resources, skip it.
    Resources recovered = offer->resources();
    recovered.unallocate();

    if (required == required - recovered) {
      continue;
    }

    totalRecovered += recovered;
    required -= recovered;

    // We explicitly pass 'Filters()' which has a default 'refuse_seconds'
    // of 5 seconds rather than 'None()' here, so that we can virtually
    // always win the race against 'allocate' if these resources are to
    // be offered back to these frameworks.
    // NOTE: However it's entirely possible that these resources are
    // offered to other frameworks in the next 'allocate' and the filter
    // cannot prevent it.
    master->allocator->recoverResources(
        offer->framework_id(),
        offer->slave_id(),
        offer->resources(),
        Filters());

    master->removeOffer(offer, true); // Rescind!

    // If we've rescinded enough offers to cover 'operation', we're done.
    Try<Resources> updatedRecovered = totalRecovered.apply(operation);
    if (updatedRecovered.isSome()) {
      break;
    }
  }

  // Propagate the 'Future<Nothing>' as 'Future<Response>' where
  // 'Nothing' -> 'Accepted' and Failed -> 'Conflict'.
  return master->apply(slave, operation)
    .then([]() -> Response { return Accepted(); })
    .repair([](const Future<Response>& result) {
       return Conflict(result.failure());
    });
}


Future<Response> Master::Http::unreserveResources(
    const mesos::master::Call& call,
    const Option<Principal>& principal,
    ContentType contentType) const
{
  CHECK_EQ(mesos::master::Call::UNRESERVE_RESOURCES, call.type());

  const SlaveID& slaveId = call.unreserve_resources().slave_id();
  const RepeatedPtrField<Resource>& resources =
    call.unreserve_resources().resources();

  return _unreserve(slaveId, resources, principal);
}


Future<Response> Master::Http::markAgentGone(
    const mesos::master::Call& call,
    const Option<Principal>& principal,
    ContentType contentType) const
{
  CHECK_EQ(mesos::master::Call::MARK_AGENT_GONE, call.type());

  const SlaveID& slaveId = call.mark_agent_gone().slave_id();

  return ObjectApprovers::create(
      master->authorizer,
      principal,
      {MARK_AGENT_GONE})
    .then(defer(
      master->self(),
      [this, slaveId](const Owned<ObjectApprovers>& approvers)
          -> Future<Response> {
        if (!approvers->approved<MARK_AGENT_GONE>()) {
          return Forbidden();
        }

        return _markAgentGone(slaveId);
      }));
}


Future<Response> Master::Http::_markAgentGone(const SlaveID& slaveId) const
{
  LOG(INFO) << "Marking agent '" << slaveId << "' as gone";

  if (master->slaves.gone.contains(slaveId)) {
    LOG(WARNING) << "Not marking agent '" << slaveId
                 << "' as gone because it has already transitioned to gone";
    return OK();
  }

  // We return a `ServiceUnavailable` (retryable error) if there is
  // an ongoing registry transition to gone/removed/unreachable.
  if (master->slaves.markingGone.contains(slaveId)) {
    LOG(WARNING) << "Not marking agent '" << slaveId
                 << "' as gone because another gone transition"
                 << " is already in progress";

    return ServiceUnavailable(
        "Agent '" + stringify(slaveId) + "' is already being transitioned"
        + " to gone");
  }

  if (master->slaves.removing.contains(slaveId)) {
    LOG(WARNING) << "Not marking agent '" << slaveId
                 << "' as gone because another remove transition"
                 << " is already in progress";

    return ServiceUnavailable(
        "Agent '" + stringify(slaveId) + "' is being transitioned to removed");
  }

  if (master->slaves.markingUnreachable.contains(slaveId)) {
    LOG(WARNING) << "Not marking agent '" << slaveId
                 << "' as gone because another unreachable transition"
                 << " is already in progress";

    return ServiceUnavailable(
        "Agent '" + stringify(slaveId) + "' is being transitioned to"
        + " unreachable");
  }

  // We currently support marking an agent gone if the agent
  // is present in the list of active, unreachable or recovered agents.
  bool found = false;

  if (master->slaves.registered.contains(slaveId)) {
    found = true;
  } else if(master->slaves.recovered.contains(slaveId)) {
    found = true;
  } else if (master->slaves.unreachable.contains(slaveId)) {
    found = true;
  }

  if (!found) {
    return NotFound("Agent '" + stringify(slaveId) + "' not found");
  }

  master->slaves.markingGone.insert(slaveId);

  TimeInfo goneTime = protobuf::getCurrentTime();

  Future<bool> gone = master->registrar->apply(Owned<RegistryOperation>(
      new MarkSlaveGone(slaveId, goneTime)));

  gone.onAny(defer(
      master->self(), [this, slaveId, goneTime](Future<bool> registrarResult) {
    CHECK(!registrarResult.isDiscarded());

    if (registrarResult.isFailed()) {
      LOG(FATAL) << "Failed to mark agent " << slaveId
                 << " as gone in the registry: "
                 << registrarResult.failure();
    }

    Slave* slave = master->slaves.registered.get(slaveId);

    // This can happen if the agent that is being marked as
    // gone is not currently registered (unreachable/recovered).
    if (slave == nullptr) {
      return;
    }

    master->markGone(slave, goneTime);
  }));

  return gone.then([]() -> Future<Response> {
    return OK();
  });
}


Future<Response> Master::Http::reconcileOperations(
    Framework* framework,
    const scheduler::Call::ReconcileOperations& call,
    ContentType contentType) const
{
  mesos::scheduler::Response response;
  response.set_type(mesos::scheduler::Response::RECONCILE_OPERATIONS);
  *response.mutable_reconcile_operations() =
    master->reconcileOperations(framework, call);

  return OK(serialize(contentType, evolve(response)), stringify(contentType));
}

} // namespace master {
} // namespace internal {
} // namespace mesos {
