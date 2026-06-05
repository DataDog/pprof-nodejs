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

#pragma once

#include <nan.h>
#include <v8-profiler.h>

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace dd {

struct AllocationProfileNodeStats {
  uint64_t inuse_objects = 0;
  uint64_t alloc_objects = 0;
  uint64_t inuse_space_bytes = 0;
  uint64_t alloc_space_bytes = 0;
};

using AllocationProfileSizeStatsMap =
    std::unordered_map<size_t, AllocationProfileNodeStats>;
using AllocationProfileNodeStatsMap =
    std::unordered_map<uint32_t, AllocationProfileSizeStatsMap>;

AllocationProfileNodeStatsMap BuildAllocationStatsByNodeId(
    const std::vector<v8::AllocationProfile::Sample>& samples);

v8::Local<v8::Array> TranslateAllocationStats(
    v8::Isolate* isolate,
    const AllocationProfileSizeStatsMap* allocation_stats);

}  // namespace dd
