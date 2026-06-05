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
                                     size_t size,
                                     const AllocationProfileNodeStats& stats) {
  Local<Object> alloc_obj = Object::New(isolate);
  Nan::Set(alloc_obj,
           String::NewFromUtf8Literal(isolate, "sizeBytes"),
           Number::New(isolate, static_cast<double>(stats.live_size)));
  Nan::Set(alloc_obj,
           String::NewFromUtf8Literal(isolate, "count"),
           Number::New(isolate, static_cast<double>(stats.live_count)));
  Nan::Set(alloc_obj,
           String::NewFromUtf8Literal(isolate, "allocSpaceBytes"),
           Number::New(isolate, static_cast<double>(stats.total_size)));
  Nan::Set(alloc_obj,
           String::NewFromUtf8Literal(isolate, "allocObjects"),
           Number::New(isolate, static_cast<double>(stats.total_count)));
  return alloc_obj;
}
}  // namespace

AllocationProfileNodeStatsMap BuildAllocationStatsByNodeId(
    const std::vector<AllocationProfile::Sample>& samples) {
  AllocationProfileNodeStatsMap stats_by_node_id;
  for (const auto& sample : samples) {
    auto& stats = stats_by_node_id[sample.node_id][sample.size];
    stats.total_count += sample.count;

#if NODE_MAJOR_VERSION >= 26
    const bool live = sample.is_live;
#else
    constexpr bool live = true;
#endif
    if (live) {
      stats.live_count += sample.count;
    }
  }

  for (auto& node_stats : stats_by_node_id) {
    for (auto& size_stats : node_stats.second) {
      const auto size = size_stats.first;
      auto& stats = size_stats.second;
      stats.live_size = stats.live_count * static_cast<uint64_t>(size);
      stats.total_size = stats.total_count * static_cast<uint64_t>(size);
    }
  }

  return stats_by_node_id;
}

Local<Array> TranslateAllocationStats(
    const AllocationProfileSizeStatsMap* allocation_stats) {
  auto* isolate = Isolate::GetCurrent();
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
    arr->Set(context, i, CreateAllocationObject(isolate, size, stats)).Check();
  }

  return arr;
}

}  // namespace dd
