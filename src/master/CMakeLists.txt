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

# MASTER LIBRARY.
#################
# NOTE: These are separate from the executable below for the purpose
# of linking these sources into tests.
set(MASTER_SRC
  flags.cpp
  framework.cpp
  http.cpp
  maintenance.cpp
  master.cpp
  metrics.cpp
  quota.cpp
  quota_handler.cpp
  readonly_handler.cpp
  registrar.cpp
  registry_operations.cpp
  weights.cpp
  weights_handler.cpp
  validation.cpp
  allocator/allocator.cpp
  allocator/mesos/hierarchical.cpp
  allocator/mesos/metrics.cpp
  allocator/sorter/drf/metrics.cpp
  allocator/sorter/drf/sorter.cpp
  allocator/sorter/random/sorter.cpp
  contender/contender.cpp
  contender/standalone.cpp
  contender/zookeeper.cpp

  # NOTE: This intentionally appears twice (see `SCHEDULER_SRC`), as it is
  # a small helper method used in both Master and Scheduler.
  ../scheduler/validation.cpp

  ../watcher/whitelist_watcher.cpp

  ../zookeeper/contender.cpp)

add_library(mesos-libmaster ${MASTER_SRC})

target_link_libraries(
  mesos-libmaster PUBLIC
  mesos-replicated-log
  mesos-master-agent)


# MASTER EXECUTABLE.
####################
add_executable(mesos-master main.cpp)

target_link_libraries(
  mesos-master PRIVATE
  mesos-libmaster
  $<$<BOOL:${ENABLE_JEMALLOC_ALLOCATOR}>:jemalloc>)
