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

#include "scheduler/validation.hpp"

#include <glog/logging.h>

#include <mesos/type_utils.hpp>

#include <process/authenticator.hpp>

#include <stout/none.hpp>
#include <stout/uuid.hpp>

using process::http::authentication::Principal;

namespace mesos {
namespace internal {
namespace master {
namespace validation {
namespace scheduler {
namespace call {

Option<Error> validate(
    const mesos::scheduler::Call& call,
    const Option<Principal>& principal)
{
  if (!call.IsInitialized()) {
    return Error("Not initialized: " + call.InitializationErrorString());
  }

  if (!call.has_type()) {
    return Error("Expecting 'type' to be present");
  }

  if (call.type() == mesos::scheduler::Call::SUBSCRIBE) {
    if (!call.has_subscribe()) {
      return Error("Expecting 'subscribe' to be present");
    }

    const FrameworkInfo& frameworkInfo = call.subscribe().framework_info();

    if (frameworkInfo.id() != call.framework_id()) {
      return Error("'framework_id' differs from 'subscribe.framework_info.id'");
    }

    if (principal.isSome() &&
        frameworkInfo.has_principal() &&
        principal != frameworkInfo.principal()) {
      // We assume that `principal->value.isSome()` is true. The master's HTTP
      // handlers enforce this constraint, and V0 authenticators will only
      // return principals of that form.
      CHECK_SOME(principal->value);

      return Error(
          "Authenticated principal '" + stringify(principal.get()) +
          "' does not match principal '" + frameworkInfo.principal() +
          "' set in `FrameworkInfo`");
    }

    return None();
  }

  // All calls except SUBSCRIBE should have framework id set.
  if (!call.has_framework_id()) {
    return Error("Expecting 'framework_id' to be present");
  }

  switch (call.type()) {
    case mesos::scheduler::Call::SUBSCRIBE:
      // SUBSCRIBE call should have been handled above.
      LOG(FATAL) << "Unexpected 'SUBSCRIBE' call";

    case mesos::scheduler::Call::TEARDOWN:
      return None();

    case mesos::scheduler::Call::ACCEPT:
      if (!call.has_accept()) {
        return Error("Expecting 'accept' to be present");
      }
      return None();

    case mesos::scheduler::Call::DECLINE:
      if (!call.has_decline()) {
        return Error("Expecting 'decline' to be present");
      }
      return None();

    case mesos::scheduler::Call::ACCEPT_INVERSE_OFFERS:
      if (!call.has_accept_inverse_offers()) {
        return Error("Expecting 'accept_inverse_offers' to be present");
      }
      return None();

    case mesos::scheduler::Call::DECLINE_INVERSE_OFFERS:
      if (!call.has_decline_inverse_offers()) {
        return Error("Expecting 'decline_inverse_offers' to be present");
      }
      return None();

    case mesos::scheduler::Call::REVIVE:
      return None();

    case mesos::scheduler::Call::SUPPRESS:
      return None();

    case mesos::scheduler::Call::KILL:
      if (!call.has_kill()) {
        return Error("Expecting 'kill' to be present");
      }
      return None();

    case mesos::scheduler::Call::SHUTDOWN:
      if (!call.has_shutdown()) {
        return Error("Expecting 'shutdown' to be present");
      }
      return None();

    case mesos::scheduler::Call::ACKNOWLEDGE: {
      if (!call.has_acknowledge()) {
        return Error("Expecting 'acknowledge' to be present");
      }

      Try<id::UUID> uuid = id::UUID::fromBytes(call.acknowledge().uuid());
      if (uuid.isError()) {
        return uuid.error();
      }
      return None();
    }

    case mesos::scheduler::Call::ACKNOWLEDGE_OPERATION_STATUS: {
      if (!call.has_acknowledge_operation_status()) {
        return Error("Expecting 'acknowledge_operation_status' to be present");
      }

      const mesos::scheduler::Call::AcknowledgeOperationStatus& acknowledge =
        call.acknowledge_operation_status();

      Try<id::UUID> uuid = id::UUID::fromBytes(acknowledge.uuid());
      if (uuid.isError()) {
        return uuid.error();
      }

      // TODO(gkleiman): Revisit this once we introduce support for external
      // resource providers.
      if (!acknowledge.has_slave_id()) {
        return Error("Expecting 'agent_id' to be present");
      }

      // TODO(gkleiman): Revisit this once agent supports sending status
      // updates for operations affecting default resources (MESOS-8194).
      if (!acknowledge.has_resource_provider_id()) {
        return Error("Expecting 'resource_provider_id' to be present");
      }

      return None();
    }

    case mesos::scheduler::Call::RECONCILE:
      if (!call.has_reconcile()) {
        return Error("Expecting 'reconcile' to be present");
      }
      return None();

    case mesos::scheduler::Call::RECONCILE_OPERATIONS:
      if (!call.has_reconcile_operations()) {
        return Error("Expecting 'reconcile_operations' to be present");
      }
      return None();

    case mesos::scheduler::Call::MESSAGE:
      if (!call.has_message()) {
        return Error("Expecting 'message' to be present");
      }
      return None();

    case mesos::scheduler::Call::REQUEST:
      if (!call.has_request()) {
        return Error("Expecting 'request' to be present");
      }
      return None();

    case mesos::scheduler::Call::UNKNOWN:
      return None();
  }

  UNREACHABLE();
}

} // namespace call {
} // namespace scheduler {
} // namespace validation {
} // namespace master {
} // namespace internal {
} // namespace mesos {
