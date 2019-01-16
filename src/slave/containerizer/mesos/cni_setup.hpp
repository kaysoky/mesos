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

#ifndef __MESOS_CONTAINERIZER_CNI_SETUP_HPP__
#define __MESOS_CONTAINERIZER_CNI_SETUP_HPP__

#include <process/id.hpp>
#include <process/subprocess.hpp>

#include <stout/flags.hpp>
#include <stout/subcommand.hpp>

namespace mesos {
namespace internal {
namespace slave {

// A subcommand to setup container hostname and mount the hosts,
// resolv.conf and hostname from the host file system into the
// container's file system.  The hostname needs to be setup in the
// container's UTS namespace, and the files need to be bind mounted in
// the container's mnt namespace.
class NetworkCniIsolatorSetup : public Subcommand
{
public:
  static const char* NAME;

  struct Flags : public virtual flags::FlagsBase
  {
    Flags();

    Option<pid_t> pid;
    Option<std::string> hostname;
    Option<std::string> rootfs;
    Option<std::string> etc_hosts_path;
    Option<std::string> etc_hostname_path;
    Option<std::string> etc_resolv_conf;
    bool bind_host_files;
    bool bind_readonly;
  };

  NetworkCniIsolatorSetup() : Subcommand(NAME) {}

  Flags flags;

protected:
  int execute() override;
  flags::FlagsBase* getFlags() override { return &flags; }
};

} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __MESOS_CONTAINERIZER_CNI_SETUP_HPP__
