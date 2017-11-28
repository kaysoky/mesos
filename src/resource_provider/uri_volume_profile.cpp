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

#include <map>
#include <string>
#include <tuple>

#include <mesos/mesos.hpp>

#include <mesos/module/volume_profile.hpp>

#include <mesos/resource_provider/volume_profile.hpp>

#include <process/defer.hpp>
#include <process/delay.hpp>
#include <process/dispatch.hpp>
#include <process/future.hpp>
#include <process/owned.hpp>
#include <process/socket.hpp>

#include <stout/duration.hpp>
#include <stout/error.hpp>
#include <stout/json.hpp>
#include <stout/option.hpp>
#include <stout/protobuf.hpp>
#include <stout/result.hpp>
#include <stout/strings.hpp>

#include <csi/spec.hpp>

#include "resource_provider/uri_volume_profile.hpp"

using namespace mesos;
using namespace process;

using std::map;
using std::string;
using std::tuple;

using google::protobuf::Map;


namespace csi {

bool operator==(const VolumeCapability& left, const VolumeCapability& right) {
  // NOTE: This enumeration is set when `block` or `mount` are set and
  // covers the case where neither are set.
  if (left.access_type_case() != right.access_type_case()) {
    return false;
  }

  // NOTE: No need to check `block` for equality as that object is empty.

  if (left.has_mount()) {
    if (left.mount().fs_type() != right.mount().fs_type()) {
      return false;
    }

    if (left.mount().mount_flags_size() != right.mount().mount_flags_size()) {
      return false;
    }

    // NOTE: Ordering may or may not matter for these flags, but this helper
    // only checks for complete equality.
    for (int i = 0; i < left.mount().mount_flags_size(); i++) {
      if (left.mount().mount_flags(i) != right.mount().mount_flags(i)) {
        return false;
      }
    }
  }

  if (left.has_access_mode() != right.has_access_mode()) {
    return false;
  }

  if (left.has_access_mode()) {
    if (left.access_mode().mode() != right.access_mode().mode()) {
      return false;
    }
  }

  return true;
}

} // namespace csi {

namespace mesos {
namespace internal {
namespace profile {

bool operator==(
    const Map<string, string>& left,
    const Map<string, string>& right) {
  if (left.size() != right.size()) {
    return false;
  }

  typename Map<string, string>::const_iterator iterator = left.begin();
  while (iterator != left.end()) {
    if (right.count(iterator->first) != 1) {
      return false;
    }

    if (iterator->second != right.at(iterator->first)) {
      return false;
    }
  }

  return true;
}


UriVolumeProfileAdaptor::UriVolumeProfileAdaptor(const Flags& _flags)
  : flags(_flags),
    process(new UriVolumeProfileAdaptorProcess(flags))
{
  spawn(process.get());
}


UriVolumeProfileAdaptor::~UriVolumeProfileAdaptor()
{
  terminate(process.get());
  wait(process.get());
}


Future<VolumeProfileAdaptor::ProfileInfo> UriVolumeProfileAdaptor::translate(
    const string& profile,
    const std::string& csiPluginInfoType)
{
  return dispatch(
      process.get(),
      &UriVolumeProfileAdaptorProcess::translate,
      profile,
      csiPluginInfoType);
}


Future<hashset<string>> UriVolumeProfileAdaptor::watch(
    const hashset<string>& knownProfiles,
    const std::string& csiPluginInfoType)
{
  return dispatch(
      process.get(),
      &UriVolumeProfileAdaptorProcess::watch,
      knownProfiles,
      csiPluginInfoType);
}


UriVolumeProfileAdaptorProcess::UriVolumeProfileAdaptorProcess(
    const Flags& _flags)
  : ProcessBase(ID::generate("uri-volume-profile")),
    flags(_flags),
    watchPromise(new Promise<hashset<string>>()) {}


void UriVolumeProfileAdaptorProcess::initialize()
{
  poll();
}


Future<VolumeProfileAdaptor::ProfileInfo>
  UriVolumeProfileAdaptorProcess::translate(
      const string& profile,
      const std::string& csiPluginInfoType)
{
  if (data.count(profile) != 1) {
    return Failure("Profile '" + profile + "' not found");
  }

  return data.at(profile);
}


Future<hashset<string>> UriVolumeProfileAdaptorProcess::watch(
    const hashset<string>& knownProfiles,
    const std::string& csiPluginInfoType)
{
  if (profiles != knownProfiles) {
    return profiles;
  }

  return watchPromise->future();
}


void UriVolumeProfileAdaptorProcess::poll()
{
  // NOTE: The flags do not allow relative paths, so this is guaranteed to
  // be either 'http://' or 'https://'.
  if (strings::startsWith(flags.uri, "http")) {
    // NOTE: We already validated that this URI is parsable in the flags.
    Try<http::URL> url = http::URL::parse(flags.uri.string());
    CHECK_SOME(url);

    http::get(url.get())
      .onAny(defer(self(), [=](const Future<http::Response>& future) {
        if (future.isReady()) {
          // NOTE: We don't check the HTTP status code because we don't know
          // what potential codes are considered successful.
          _poll(future->body);
        } else if (future.isFailed()) {
          _poll(Error(future.failure()));
        } else {
          _poll(Error("Future discarded or abandoned"));
        }
      }));
  } else {
    _poll(os::read(flags.uri.string()));
  }
}


void UriVolumeProfileAdaptorProcess::_poll(const Try<string>& fetched)
{
  if (fetched.isSome()) {
    Try<map<string, VolumeProfileAdaptor::ProfileInfo>> parsed
      = parse(fetched.get());

    if (parsed.isSome()) {
      notify(parsed.get());
    } else {
      LOG(ERROR) << "Failed to parse result: " << parsed.error();
    }
  } else {
    LOG(WARNING) << "Failed to poll URI: " << fetched.error();
  }

  // TODO(josephw): Do we want to retry if polling fails and no polling
  // interval is set? Or perhaps we should exit in that case?
  if (flags.poll_interval.isSome()) {
    delay(flags.poll_interval.get(), self(), &Self::poll);
  }
}


void UriVolumeProfileAdaptorProcess::notify(
    const map<string, VolumeProfileAdaptor::ProfileInfo>& parsed)
{
  bool hasErrors = false;

  foreachkey (const string& profile, data) {
    if (parsed.count(profile) != 1) {
      hasErrors = true;

      LOG(WARNING)
        << "Fetched profile mapping does not contain profile '" << profile
        << "'. The fetched mapping will be ignored entirely";
      continue;
    }

    if (!(data.at(profile).capability == parsed.at(profile).capability &&
        data.at(profile).parameters == parsed.at(profile).parameters)) {
      hasErrors = true;

      LOG(WARNING)
        << "Fetched profile mapping for profile '" << profile << "'"
        << " does not match earlier data."
        << " The fetched mapping will be ignored entirely";
    }
  }

  // When encountering a data conflict, this module assumes there is a
  // problem upstream (i.e. in the `--uri`). It is up to the operator
  // to notice and resolve this.
  if (hasErrors) {
    return;
  }

  // The fetched mapping satisfies our invariants.
  data = parsed;

  // Update the convenience set of profile names.
  profiles.clear();
  foreachkey (const string& profile, parsed) {
    profiles.insert(profile);
  }

  // Notify any watchers and then prepare a new promise for the next
  // iteration of polling.
  //
  // TODO(josephw): Delay this based on the `--max_random_wait` option.
  watchPromise->set(profiles);
  watchPromise.reset(new Promise<hashset<string>>());

  LOG(INFO)
    << "Updated volume profile mapping to " << profiles.size()
    << " total profiles";
}


Try<map<string, VolumeProfileAdaptor::ProfileInfo>>
  UriVolumeProfileAdaptorProcess::parse(const string& data)
{
  Try<JSON::Object> object = JSON::parse<JSON::Object>(data);
  if (object.isError()) {
    return Error("Failed to parse as JSON: " + object.error());
  }

  map<string, VolumeProfileAdaptor::ProfileInfo> parsed;
  foreachpair (const string& key, const JSON::Value& value, object->values) {
    if (!value.is<JSON::Object>()) {
      return Error("Expected a JSON object for profile entries");
    }

    const JSON::Object& profile = value.as<JSON::Object>();

    // Parse and then validate the profile's VolumeCapability.
    csi::VolumeCapability capability;
    Result<JSON::Object> jsonCapability =
      profile.at<JSON::Object>("volume_capabilities");

    if (jsonCapability.isError()) {
      return Error(
          "Failed to retrieve 'volume_capabilities' for profile '" +
          key + "': " + jsonCapability.error());
    } else if (jsonCapability.isNone()) {
      return Error(
          "Failed to find 'volume_capabilities' for profile '" + key + "'");
    } else {
      Try<csi::VolumeCapability> protoCapability =
        protobuf::parse<csi::VolumeCapability>(jsonCapability.get());

      if (protoCapability.isError()) {
        return Error(
            "Failed to parse VolumeCapability for profile '" + key
            + "': " + protoCapability.error());
      }

      Option<Error> error = validate(protoCapability.get());
      if (error.isSome()) {
        return Error(
            "Parsed invalid VolumeCapability for profile '" + key
            + "': " + error->message);
      }

      capability = protoCapability.get();
    }

    // Copy the profile's volume creation parameters.
    //
    // TODO(josephw): Stout's protobuf helpers are for Proto2 and therefore
    // do not have helpers for parsing protobuf Maps.
    Map<string, string> parameters;
    Result<JSON::Object> jsonParameters =
      profile.at<JSON::Object>("create_parameters");

    if (jsonParameters.isError()) {
      return Error(
          "Failed to retrieve 'create_parameters' for profile '" +
          key + "': " + jsonParameters.error());
    } else if (jsonParameters.isSome()) {
      foreachpair (
          const string& paramKey,
          const JSON::Value& paramValue,
          jsonParameters->values) {
        if (!paramValue.is<JSON::String>()) {
          return Error(
              "Parsed invalid 'create_parameters' for profile '" + key
              + "': Expected JSON string values");
        }

        parameters[paramKey] = paramValue.as<JSON::String>().value;
      }
    }

    parsed[key] = {capability, parameters};
  }

  return parsed;
}


Option<Error> UriVolumeProfileAdaptorProcess::validate(
    const csi::VolumeCapability& capability) {
  // NOTE: Stout's protobuf helpers are parsing this object as Proto2 and
  // therefore do not know about the `oneof` syntax for unions. If both
  // values are specified, the last value to be set will be used because
  // the Proto3 generated code implements an actual union.

  if (!capability.has_block() && !capability.has_mount()) {
    return Error("One of 'block' or 'mount' must be set");
  }

  if (capability.has_mount()) {
    // The total size of this repeated field may not exceed 4 KB.
    //
    // TODO(josephw): The specification does not state how this maximum
    // size is calculated. So this check is conservative and does not
    // include padding or array separators in the size calculation.
    size_t size = 0;
    foreach (const string& flag, capability.mount().mount_flags()) {
      size += flag.size();
    }

    if (Bytes(size) > Kilobytes(4)) {
      return Error("Size of 'mount_flags' may not exceed 4 KB");
    }
  }

  if (!capability.has_access_mode()) {
    return Error("'access_mode' is a required field");
  }

  if (capability.access_mode().mode() ==
      csi::VolumeCapability::AccessMode::UNKNOWN) {
    return Error("'access_mode.mode' is unknown or not set");
  }

  return None();
}

} // namespace profile {
} // namespace internal {
} // namespace mesos {


mesos::modules::Module<VolumeProfileAdaptor>
org_apache_mesos_UriVolumeProfileAdaptor(
    MESOS_MODULE_API_VERSION,
    MESOS_VERSION,
    "Apache Mesos",
    "modules@mesos.apache.org",
    "URI Volume Profile Adaptor module.",
    nullptr,
    [](const Parameters& parameters) -> VolumeProfileAdaptor* {
      // Convert `parameters` into a map.
      map<string, string> values;
      foreach (const Parameter& parameter, parameters.parameter()) {
        values[parameter.key()] = parameter.value();
      }

      // Load and validate flags from the map.
      mesos::internal::profile::Flags flags;
      Try<flags::Warnings> load = flags.load(values);

      if (load.isError()) {
        LOG(ERROR) << "Failed to parse parameters: " << load.error();
        return nullptr;
      }

      // Log any flag warnings.
      foreach (const flags::Warning& warning, load->warnings) {
        LOG(WARNING) << warning.message;
      }

      return new mesos::internal::profile::UriVolumeProfileAdaptor(flags);
    });
