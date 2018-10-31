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

#ifndef __MESOS_CONTAINERIZER_LAUNCH_HPP__
#define __MESOS_CONTAINERIZER_LAUNCH_HPP__

#include <string>

#include <stout/json.hpp>
#include <stout/option.hpp>
#include <stout/subcommand.hpp>

#include <mesos/mesos.hpp>


namespace mesos {
namespace internal {
namespace slave {

const char MESOS_CONTAINERIZER_LAUNCH_NAME[] = "launch";

class MesosContainerizerLaunch : public Subcommand
{
public:
  struct Flags : public virtual flags::FlagsBase
  {
    Flags()
    {
      add(&Flags::launch_info,
          "launch_info",
          "");

      add(&Flags::pipe_read,
          "pipe_read",
          "The read end of the control pipe. This is a file descriptor \n"
          "on Posix, or a handle on Windows. It's caller's responsibility \n"
          "to make sure the file descriptor or the handle is inherited \n"
          "properly in the subprocess. It's used to synchronize with the \n"
          "parent process. If not specified, no synchronization will happen.");

      add(&Flags::pipe_write,
          "pipe_write",
          "The write end of the control pipe. This is a file descriptor \n"
          "on Posix, or a handle on Windows. It's caller's responsibility \n"
          "to make sure the file descriptor or the handle is inherited \n"
          "properly in the subprocess. It's used to synchronize with the \n"
          "parent process. If not specified, no synchronization will happen.");

      add(&Flags::runtime_directory,
          "runtime_directory",
          "The runtime directory for the container (used for checkpointing)");

#ifdef __linux__
      add(&Flags::namespace_mnt_target,
          "namespace_mnt_target",
          "The target 'pid' of the process whose mount namespace we'd like\n"
          "to enter before executing the command.");

      add(&Flags::unshare_namespace_mnt,
          "unshare_namespace_mnt",
          "Whether to launch the command in a new mount namespace.",
          false);
#endif // __linux__
    }

    Option<JSON::Object> launch_info;
    Option<int_fd> pipe_read;
    Option<int_fd> pipe_write;
    Option<std::string> runtime_directory;
#ifdef __linux__
    Option<pid_t> namespace_mnt_target;
    bool unshare_namespace_mnt;
#endif // __linux__
  };

  MesosContainerizerLaunch() : Subcommand(MESOS_CONTAINERIZER_LAUNCH_NAME) {}

  Flags flags;

protected:
  int execute() override;
  flags::FlagsBase* getFlags() override { return &flags; }
};

} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __MESOS_CONTAINERIZER_LAUNCH_HPP__
