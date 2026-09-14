/*
 * Copyright 2024 Datadog, Inc
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

#include "allocation-profile.hh"

#include <v8-profiler.h>
#include <v8.h>
#include <memory>
#include <string>
#include <vector>

namespace dd {

struct Node {
  struct Allocation {
    size_t size = 0;
    // v8's per-size count, which the stderr dump and the export render.
    uint32_t count = 0;
    // Rebuilt from the profile's samples, and equal to count outside
    // allocation mode, where v8 reports no live/allocated split.
    uint64_t inuse_objects = 0;
    uint64_t alloc_objects = 0;
  };

  std::string name;
  std::string script_name;
  int line_number;
  int column_number;
  int script_id;
  std::vector<std::shared_ptr<Node>> children;
  std::vector<Allocation> allocations;
  // Set on every node iff the profile was captured in allocation mode.
  bool has_allocation_stats = false;
};

std::shared_ptr<Node> TranslateAllocationProfileToCpp(
    v8::AllocationProfile::Node* node,
    const AllocationProfileNodeStatsMap* allocation_stats);

v8::Local<v8::Value> TranslateAllocationProfile(Node* node);
v8::Local<v8::Value> TranslateAllocationProfile(
    v8::AllocationProfile::Node* node,
    const AllocationProfileNodeStatsMap* allocation_stats);

}  // namespace dd
