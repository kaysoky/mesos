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

#include "slave/containerizer/mesos/cni_setup.hpp"

#include <iostream>

#include <stout/net.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>

#include "common/status_utils.hpp"

#include "linux/fs.hpp"
#include "linux/ns.hpp"

using std::cerr;
using std::endl;
using std::map;
using std::string;

namespace mesos {
namespace internal {
namespace slave {

// Implementation of subcommand to setup relevant network files and
// hostname in the container UTS and mount namespace.
const char* NetworkCniIsolatorSetup::NAME = "network-cni-setup";


NetworkCniIsolatorSetup::Flags::Flags()
{
  add(&Flags::pid, "pid", "PID of the container");

  add(&Flags::hostname, "hostname", "Hostname of the container");

  add(&Flags::rootfs,
      "rootfs",
      "Path to rootfs for the container on the host-file system");

  add(&Flags::etc_hosts_path,
      "etc_hosts_path",
      "Path in the host file system for 'hosts' file");

  add(&Flags::etc_hostname_path,
      "etc_hostname_path",
      "Path in the host file system for 'hostname' file");

  add(&Flags::etc_resolv_conf,
      "etc_resolv_conf",
      "Path in the host file system for 'resolv.conf'");

  add(&Flags::bind_host_files,
      "bind_host_files",
      "Bind mount the container's network files to the network files "
      "present on host filesystem",
      false);

  add(&Flags::bind_readonly,
      "bind_readonly",
      "Bind mount the container's network files read-only to protect the "
      "originals",
      false);
}


int NetworkCniIsolatorSetup::execute()
{
  // NOTE: This method has to be run in a new mount namespace.

  if (flags.help) {
    cerr << flags.usage();
    return EXIT_SUCCESS;
  }

  if (flags.pid.isNone()) {
    cerr << "Container PID not specified" << endl;
    return EXIT_FAILURE;
  }

  // Initialize the host path and container path for the set of files
  // that need to be setup in the container file system.
  hashmap<string, string> files;

  if (flags.etc_hosts_path.isNone()) {
    // This is the case where host network is used, container has an
    // image, and `/etc/hosts` does not exist in the system.
  } else if (!os::exists(flags.etc_hosts_path.get())) {
    cerr << "Unable to find '" << flags.etc_hosts_path.get() << "'" << endl;
    return EXIT_FAILURE;
  } else {
    files["/etc/hosts"] = flags.etc_hosts_path.get();
  }

  if (flags.etc_hostname_path.isNone()) {
    // This is the case where host network is used, container has an
    // image, and `/etc/hostname` does not exist in the system.
  } else if (!os::exists(flags.etc_hostname_path.get())) {
    cerr << "Unable to find '" << flags.etc_hostname_path.get() << "'" << endl;
    return EXIT_FAILURE;
  } else {
    files["/etc/hostname"] = flags.etc_hostname_path.get();
  }

  if (flags.etc_resolv_conf.isNone()) {
    cerr << "Path to 'resolv.conf' not specified." << endl;
    return EXIT_FAILURE;
  } else if (!os::exists(flags.etc_resolv_conf.get())) {
    cerr << "Unable to find '" << flags.etc_resolv_conf.get() << "'" << endl;
    return EXIT_FAILURE;
  } else {
    files["/etc/resolv.conf"] = flags.etc_resolv_conf.get();
  }

  // Enter the mount namespace.
  Try<Nothing> setns = ns::setns(flags.pid.get(), "mnt");
  if (setns.isError()) {
    cerr << "Failed to enter the mount namespace of pid "
         << flags.pid.get() << ": " << setns.error() << endl;
    return EXIT_FAILURE;
  }

  // TODO(jieyu): Currently there seems to be a race between the
  // filesystem isolator and other isolators to execute the `isolate`
  // method. This results in the rootfs of the container not being
  // marked as slave + recursive which can result in the mounts in the
  // container mnt namespace propagating back into the host mnt
  // namespace. This is dangerous, since these mounts won't be cleared
  // in the host mnt namespace once the container mnt namespace is
  // destroyed (when the process dies). To avoid any leakage we mark
  // the root as a SLAVE recursively to avoid any propagation of
  // mounts in the container mnt namespace back into the host mnt
  // namespace.
  Try<Nothing> mount = fs::mount(
      None(),
      "/",
      None(),
      MS_SLAVE | MS_REC,
      nullptr);

  if (mount.isError()) {
    cerr << "Failed to mark `/` as a SLAVE mount: " << mount.error() << endl;
    return EXIT_FAILURE;
  }

  // If we are in a user namespace, then our copy of the mount tree is
  // marked unprivileged and the kernel will required us to propagate
  // any additional flags from the underlying mount to the bind mount
  // when we do the MS_RDONLY remount. To save the bother of reading
  // the mount table to find the flags to propagate, we just always
  // use the most restrictive flags here.
  const unsigned bindflags = MS_BIND | MS_NOEXEC | MS_NODEV | MS_NOSUID |
    (flags.bind_readonly ? MS_RDONLY : 0);

  foreachpair (const string& file, const string& source, files) {
    // Do the bind mount for network files in the host filesystem if
    // the container joins non-host network since no process in the
    // new network namespace should be seeing the original network
    // files from the host filesystem. The container's hostname will
    // be changed to the `ContainerID` and this information needs to
    // be reflected in the /etc/hosts and /etc/hostname files seen by
    // processes in the new network namespace.
    //
    // Specifically, the command executor will be launched with the
    // rootfs of the host filesystem. The command executor may later
    // pivot to the rootfs of the container filesystem when launching
    // the task.
    if (flags.bind_host_files) {
      if (!os::exists(file)) {
        // We need /etc/hosts and /etc/hostname to be present in order
        // to bind mount the container's /etc/hosts and /etc/hostname.
        // The container's network files will be different than the host's
        // files. Since these target mount points do not exist in the host
        // filesystem it should be fine to "touch" these files in
        // order to create them. We see this scenario specifically in
        // CoreOS (see MESOS-6052).
        //
        // In case of /etc/resolv.conf, however, we can't populate the
        // nameservers if they are not present, and rely on the hosts
        // IPAM to populate the /etc/resolv.conf. Hence, if
        // /etc/resolv.conf is not present we bail out.
        if (file == "/etc/hosts" || file == "/etc/hostname") {
          Try<Nothing> touch = os::touch(file);
          if (touch.isError()) {
            cerr << "Unable to create missing mount point " + file + " on "
                 << "host filesystem: " << touch.error() << endl;
            return EXIT_FAILURE;
          }
        } else {
          // '/etc/resolv.conf'.
          cerr << "Mount point '" << file << "' does not exist "
               << "on the host filesystem" << endl;
          return EXIT_FAILURE;
        }
      }

      mount = fs::mount(
          source,
          file,
          None(),
          bindflags,
          nullptr);
      if (mount.isError()) {
        cerr << "Failed to bind mount from '" << source << "' to '"
             << file << "': " << mount.error() << endl;
        return EXIT_FAILURE;
      }
    }

    // Do the bind mount in the container filesystem.
    if (flags.rootfs.isSome()) {
      const string target = path::join(flags.rootfs.get(), file);

      if (!os::exists(target)) {
        // Create the parent directory of the mount point.
        Try<Nothing> mkdir = os::mkdir(Path(target).dirname());
        if (mkdir.isError()) {
          cerr << "Failed to create directory '" << Path(target).dirname()
               << "' for the mount point: " << mkdir.error() << endl;
          return EXIT_FAILURE;
        }

        // Create the mount point in the container filesystem.
        Try<Nothing> touch = os::touch(target);
        if (touch.isError()) {
          cerr << "Failed to create the mount point '" << target
               << "' in the container filesystem" << endl;
          return EXIT_FAILURE;
        }
      } else if (os::stat::islink(target)) {
        Try<Nothing> remove = os::rm(target);
        if (remove.isError()) {
          cerr << "Failed to remove '" << target << "' "
               << "as it's a symbolic link" << endl;
          return EXIT_FAILURE;
        }

        Try<Nothing> touch = os::touch(target);
        if (touch.isError()) {
          cerr << "Failed to create the mount point '" << target
               << "' in the container filesystem" << endl;
          return EXIT_FAILURE;
        }
      }

      mount = fs::mount(
          source,
          target,
          None(),
          bindflags,
          nullptr);

      if (mount.isError()) {
        cerr << "Failed to bind mount from '" << source << "' to '"
             << target << "': " << mount.error() << endl;
        return EXIT_FAILURE;
      }
    }
  }

  if (flags.hostname.isSome()) {
    // Enter the UTS namespace.
    setns = ns::setns(flags.pid.get(), "uts");
    if (setns.isError()) {
      cerr << "Failed to enter the UTS namespace of pid "
           << flags.pid.get() << ": " << setns.error() << endl;
      return EXIT_FAILURE;
    }

    // Setup hostname in container's UTS namespace.
    Try<Nothing> setHostname = net::setHostname(flags.hostname.get());
    if (setHostname.isError()) {
      cerr << "Failed to set the hostname of the container to '"
           << flags.hostname.get() << "': " << setHostname.error() << endl;
      return EXIT_FAILURE;
    }

    // Since, the hostname is set, this is a top-level container in a
    // new network namespace. This implies that we have to bring up
    // the loopback interface as well.
    setns = ns::setns(flags.pid.get(), "net");
    if (setns.isError()) {
      cerr << "Failed to enter the network namespace of pid "
           << flags.pid.get() << ": " << setns.error() << endl;
      return EXIT_FAILURE;
    }

    // TODO(urbanserj): To get rid of all external dependencies such as
    // `iproute2` and `net-tools`, use Netlink Protocol Library (libnl).
    Option<int> status = os::spawn(
        "ip", {"ip", "link", "set", "dev", "lo", "up"});

    const string message =
      "Failed to bring up the loopback interface in the new "
      "network namespace of pid " + stringify(flags.pid.get());

    if (status.isNone()) {
      cerr << message << ": os::spawn 'ip link set dev lo up' failed: "
           << os::strerror(errno) << endl;

      // Fall back on `ifconfig` if `ip` command fails to start.
      status = os::spawn("ifconfig", {"ifconfig", "lo", "up"});
    }

    if (status.isNone()) {
      cerr << message << ": os::spawn 'ifconfig lo up' failed: "
           << os::strerror(errno) << endl;
      return EXIT_FAILURE;
    }

    if (!WSUCCEEDED(status.get())) {
      cerr << message << ": " << WSTRINGIFY(status.get()) << endl;
      return EXIT_FAILURE;
    }
  }

  return EXIT_SUCCESS;
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
