# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

add_subdirectory(container_loggers)
add_subdirectory(qos_controllers)
add_subdirectory(resource_estimators)


# AGENT SOURCES.
################
set(AGENT_SRC
  compatibility.cpp
  constants.cpp
  container_daemon.cpp
  flags.cpp
  gc.cpp
  http.cpp
  metrics.cpp
  paths.cpp
  qos_controller.cpp
  qos_controllers/noop.cpp
  resource_estimator.cpp
  slave.cpp
  state.cpp
  task_status_update_manager.cpp
  validation.cpp
  containerizer/composing.cpp
  containerizer/containerizer.cpp
  containerizer/docker.cpp
  containerizer/fetcher.cpp
  containerizer/mesos/containerizer.cpp
  containerizer/mesos/isolator.cpp
  containerizer/mesos/launcher.cpp
  containerizer/mesos/paths.cpp
  containerizer/mesos/io/switchboard.cpp
  containerizer/mesos/provisioner/backend.cpp
  containerizer/mesos/provisioner/paths.cpp
  containerizer/mesos/provisioner/provisioner.cpp
  containerizer/mesos/provisioner/store.cpp
  containerizer/mesos/provisioner/appc/cache.cpp
  containerizer/mesos/provisioner/appc/fetcher.cpp
  containerizer/mesos/provisioner/appc/paths.cpp
  containerizer/mesos/provisioner/appc/store.cpp
  containerizer/mesos/provisioner/backends/copy.cpp
  containerizer/mesos/provisioner/docker/image_tar_puller.cpp
  containerizer/mesos/provisioner/docker/metadata_manager.cpp
  containerizer/mesos/provisioner/docker/paths.cpp
  containerizer/mesos/provisioner/docker/puller.cpp
  containerizer/mesos/provisioner/docker/registry_puller.cpp
  containerizer/mesos/provisioner/docker/store.cpp
  resource_estimators/noop.cpp

  ../appc/spec.cpp

  ../csi/client.cpp
  ../csi/paths.cpp
  ../csi/rpc.cpp
  ../csi/utils.cpp

  ../docker/spec.cpp

  ../oci/spec.cpp

  ../resource_provider/daemon.cpp
  ../resource_provider/detector.cpp
  ../resource_provider/driver.cpp
  ../resource_provider/local.cpp
  ../resource_provider/manager.cpp
  ../resource_provider/registrar.cpp
  ../resource_provider/validation.cpp
  ../resource_provider/storage/disk_profile_adaptor.cpp
  ../resource_provider/storage/disk_profile_utils.cpp

  ../secret/resolver.cpp

  ../status_update_manager/operation.cpp

  ../uri/fetcher.cpp
  ../uri/utils.cpp
  ../uri/fetchers/copy.cpp
  ../uri/fetchers/curl.cpp
  ../uri/fetchers/docker.cpp
  ../uri/fetchers/hadoop.cpp

  ../usage/usage.cpp)

if (NOT WIN32)
  list(APPEND AGENT_SRC

    # NOTE: The storage local resource provider uses Unix domain sockets to talk to
    # CSI plugins, thus it is currently not supported on Windows.
    ../resource_provider/storage/provider.cpp

    containerizer/mesos/utils.cpp
    containerizer/mesos/isolators/environment_secret.cpp
    containerizer/mesos/isolators/docker/volume/driver.cpp
    containerizer/mesos/isolators/docker/volume/paths.cpp
    containerizer/mesos/isolators/filesystem/posix.cpp
    containerizer/mesos/isolators/network/cni/paths.cpp
    containerizer/mesos/isolators/network/cni/spec.cpp
    containerizer/mesos/isolators/posix/disk.cpp
    containerizer/mesos/isolators/posix/rlimits.cpp
    containerizer/mesos/isolators/volume/sandbox_path.cpp
    containerizer/mesos/provisioner/utils.cpp)
endif ()

set(LINUX_SRC
  containerizer/mesos/cni_setup.cpp
  containerizer/mesos/linux_launcher.cpp
  containerizer/mesos/isolators/appc/runtime.cpp
  containerizer/mesos/isolators/cgroups/cgroups.cpp
  containerizer/mesos/isolators/cgroups/subsystem.cpp
  containerizer/mesos/isolators/cgroups/subsystems/blkio.cpp
  containerizer/mesos/isolators/cgroups/subsystems/cpu.cpp
  containerizer/mesos/isolators/cgroups/subsystems/cpuacct.cpp
  containerizer/mesos/isolators/cgroups/subsystems/cpuset.cpp
  containerizer/mesos/isolators/cgroups/subsystems/devices.cpp
  containerizer/mesos/isolators/cgroups/subsystems/hugetlb.cpp
  containerizer/mesos/isolators/cgroups/subsystems/memory.cpp
  containerizer/mesos/isolators/cgroups/subsystems/net_cls.cpp
  containerizer/mesos/isolators/cgroups/subsystems/net_prio.cpp
  containerizer/mesos/isolators/cgroups/subsystems/perf_event.cpp
  containerizer/mesos/isolators/cgroups/subsystems/pids.cpp
  containerizer/mesos/isolators/docker/runtime.cpp
  containerizer/mesos/isolators/docker/volume/isolator.cpp
  containerizer/mesos/isolators/filesystem/linux.cpp
  containerizer/mesos/isolators/filesystem/shared.cpp
  containerizer/mesos/isolators/gpu/allocator.cpp
  containerizer/mesos/isolators/gpu/isolator.cpp
  containerizer/mesos/isolators/gpu/nvml.cpp
  containerizer/mesos/isolators/gpu/volume.cpp
  containerizer/mesos/isolators/linux/capabilities.cpp
  containerizer/mesos/isolators/linux/devices.cpp
  containerizer/mesos/isolators/namespaces/ipc.cpp
  containerizer/mesos/isolators/namespaces/pid.cpp
  containerizer/mesos/isolators/network/cni/cni.cpp
  containerizer/mesos/isolators/volume/host_path.cpp
  containerizer/mesos/isolators/volume/image.cpp
  containerizer/mesos/isolators/volume/secret.cpp
  containerizer/mesos/provisioner/backends/aufs.cpp
  containerizer/mesos/provisioner/backends/bind.cpp
  containerizer/mesos/provisioner/backends/overlay.cpp)

if (ENABLE_XFS_DISK_ISOLATOR)
  list(APPEND LINUX_SRC
    containerizer/mesos/isolators/xfs/disk.cpp
    containerizer/mesos/isolators/xfs/utils.cpp)
endif ()

if (ENABLE_LINUX_ROUTING)
  list(APPEND LINUX_SRC
    ../linux/routing/handle.cpp
    ../linux/routing/route.cpp
    ../linux/routing/utils.cpp
    ../linux/routing/diagnosis/diagnosis.cpp
    ../linux/routing/filter/basic.cpp
    ../linux/routing/filter/icmp.cpp
    ../linux/routing/filter/ip.cpp
    ../linux/routing/link/link.cpp
    ../linux/routing/link/veth.cpp
    ../linux/routing/queueing/fq_codel.cpp
    ../linux/routing/queueing/htb.cpp
    ../linux/routing/queueing/ingress.cpp)
endif ()

if (ENABLE_NETWORK_PORTS_ISOLATOR)
  list(APPEND LINUX_SRC
    containerizer/mesos/isolators/network/ports.cpp)
endif ()

if (ENABLE_PORT_MAPPING_ISOLATOR)
  list(APPEND LINUX_SRC
    containerizer/mesos/isolators/network/port_mapping.cpp)
endif()

# Include source for linux build.
if (LINUX)
  list(APPEND AGENT_SRC ${LINUX_SRC})
endif ()

set(WIN32_SRC
  containerizer/mesos/isolators/docker/runtime.cpp
  containerizer/mesos/isolators/windows/cpu.cpp
  containerizer/mesos/isolators/windows/mem.cpp
  containerizer/mesos/isolators/environment_secret.cpp
  containerizer/mesos/isolators/filesystem/posix.cpp
  containerizer/mesos/isolators/filesystem/windows.cpp)

# Include source for Windows build.
if (WIN32)
  list(APPEND AGENT_SRC ${WIN32_SRC})
endif ()


# AGENT LIBRARY.
################
# NOTE: These are separate from the executable below for the purpose
# of linking these sources into tests.
add_library(mesos-libagent ${AGENT_SRC})

target_link_libraries(
  mesos-libagent PUBLIC
  mesos-detector
  mesos-docker
  mesos-hdfs
  mesos-master-agent
  mesos-rlimit
  mesos-sandbox-logger

  $<$<PLATFORM_ID:Linux>:mesos-linux>
  $<$<PLATFORM_ID:Linux>:nvml>)

target_compile_definitions(
  mesos-libagent PUBLIC
  $<$<BOOL:${ENABLE_XFS_DISK_ISOLATOR}>:ENABLE_XFS_DISK_ISOLATOR>
  $<$<BOOL:${ENABLE_PORT_MAPPING_ISOLATOR}>:ENABLE_PORT_MAPPING_ISOLATOR>
  $<$<BOOL:${ENABLE_NETWORK_PORTS_ISOLATOR}>:ENABLE_NETWORK_PORTS_ISOLATOR>)

# Add binary dependencies.
add_dependencies(
  mesos-libagent
  mesos-containerizer
  mesos-default-executor
  mesos-docker-executor
  mesos-executor
  mesos-fetcher
  mesos-tcp-connect
  mesos-usage)

if (WIN32)
  # NOTE: The agent depends on `curl` on non-Windows platforms too,
  # but not as part of the build. This is necessary because we use
  # CMake to deploy `curl.exe` on Windows for use in the HTTP health
  # checks. Similarly we depend on `wclayer` to be deployed.
  add_dependencies(
    mesos-libagent
    curl
    wclayer)
else ()
  add_dependencies(
    mesos-libagent
    mesos-cni-port-mapper
    mesos-io-switchboard)
endif ()


# AGENT EXECUTABLE.
###################
add_executable(mesos-agent main.cpp)

target_link_libraries(
  mesos-agent PRIVATE
  mesos-libagent
  $<$<BOOL:${ENABLE_JEMALLOC_ALLOCATOR}>:jemalloc>)
