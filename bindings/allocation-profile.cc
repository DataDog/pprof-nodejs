/*
 * Copyright 2026 Datadog, Inc
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "allocation-profile.hh"

#include <node_version.h>

using namespace v8;

namespace dd {
AllocationProfileNodeStatsMap BuildAllocationStatsByNodeId(
    const std::vector<AllocationProfile::Sample>& samples) {
  AllocationProfileNodeStatsMap stats_by_node_id;
  for (const auto& sample : samples) {
    auto& stats = stats_by_node_id[sample.node_id][sample.size];
    stats.alloc_objects += sample.count;

#if NODE_MAJOR_VERSION >= 26
    const bool live = sample.is_live;
#else
    constexpr bool live = true;
#endif
    if (live) {
      stats.inuse_objects += sample.count;
    }
  }

  for (auto& node_stats : stats_by_node_id) {
    for (auto& size_stats : node_stats.second) {
      const auto size = size_stats.first;
      auto& stats = size_stats.second;
      stats.inuse_space_bytes =
          stats.inuse_objects * static_cast<uint64_t>(size);
      stats.alloc_space_bytes =
          stats.alloc_objects * static_cast<uint64_t>(size);
    }
  }

  return stats_by_node_id;
}

}  // namespace dd
