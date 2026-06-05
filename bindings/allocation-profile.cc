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

#include <algorithm>

#include <node_version.h>

using namespace v8;

namespace dd {
namespace {
Local<Object> CreateAllocationObject(Isolate* isolate,
                                     const AllocationProfileNodeStats& stats) {
  Local<Object> alloc_obj = Object::New(isolate);
  Nan::Set(alloc_obj,
           String::NewFromUtf8Literal(isolate, "inuseObjects"),
           Number::New(isolate, static_cast<double>(stats.inuse_objects)));
  Nan::Set(alloc_obj,
           String::NewFromUtf8Literal(isolate, "inuseSpaceBytes"),
           Number::New(isolate, static_cast<double>(stats.inuse_space_bytes)));
  Nan::Set(alloc_obj,
           String::NewFromUtf8Literal(isolate, "allocObjects"),
           Number::New(isolate, static_cast<double>(stats.alloc_objects)));
  Nan::Set(alloc_obj,
           String::NewFromUtf8Literal(isolate, "allocSpaceBytes"),
           Number::New(isolate, static_cast<double>(stats.alloc_space_bytes)));
  return alloc_obj;
}
}  // namespace

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

Local<Array> TranslateAllocationStats(
    Isolate* isolate,
    const AllocationProfileSizeStatsMap* allocation_stats) {
  auto context = isolate->GetCurrentContext();

  if (!allocation_stats || allocation_stats->empty()) {
    return Array::New(isolate, 0);
  }

  std::vector<size_t> sizes;
  sizes.reserve(allocation_stats->size());
  for (const auto& allocation : *allocation_stats) {
    sizes.push_back(allocation.first);
  }
  std::sort(sizes.begin(), sizes.end());

  Local<Array> arr = Array::New(isolate, sizes.size());
  for (size_t i = 0; i < sizes.size(); i++) {
    const auto size = sizes[i];
    const auto& stats = allocation_stats->at(size);
    arr->Set(context, i, CreateAllocationObject(isolate, stats)).Check();
  }

  return arr;
}

}  // namespace dd
