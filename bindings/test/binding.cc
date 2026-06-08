/*
 * Copyright 2023 Datadog, Inc
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

#include <cstdlib>
#include <sstream>
#include <unordered_map>
#include <vector>

#include "allocation-profile.hh"
#include "nan.h"
#include "node.h"
#include "node_version.h"
#include "tap.h"
#include "v8.h"

namespace {
v8::AllocationProfile::Sample MakeAllocationSample(uint32_t node_id,
                                                   size_t size,
                                                   unsigned int count,
                                                   uint64_t sample_id,
                                                   bool is_live) {
#if NODE_MAJOR_VERSION >= 26
  return {node_id, size, count, sample_id, is_live};
#else
  (void)is_live;
  return {node_id, size, count, sample_id};
#endif
}

void TestBuildAllocationStatsByNodeId(Tap& t) {
  std::vector<v8::AllocationProfile::Sample> samples = {
      MakeAllocationSample(1, 100, 3, 1, true),
      MakeAllocationSample(1, 100, 2, 2, false),
      MakeAllocationSample(1, 50, 5, 3, true),
      MakeAllocationSample(2, 100, 7, 4, true),
  };

  auto stats_by_node_id = dd::BuildAllocationStatsByNodeId(samples);
  t.equal(stats_by_node_id.size(), static_cast<size_t>(2));

  const auto& node1_size100 = stats_by_node_id.at(1).at(100);
  t.equal(node1_size100.alloc_objects, static_cast<uint64_t>(5));
  t.equal(node1_size100.alloc_space_bytes, static_cast<uint64_t>(500));
#if NODE_MAJOR_VERSION >= 26
  t.equal(node1_size100.inuse_objects, static_cast<uint64_t>(3));
  t.equal(node1_size100.inuse_space_bytes, static_cast<uint64_t>(300));
#else
  t.equal(node1_size100.inuse_objects, static_cast<uint64_t>(5));
  t.equal(node1_size100.inuse_space_bytes, static_cast<uint64_t>(500));
#endif

  const auto& node1_size50 = stats_by_node_id.at(1).at(50);
  t.equal(node1_size50.alloc_objects, static_cast<uint64_t>(5));
  t.equal(node1_size50.alloc_space_bytes, static_cast<uint64_t>(250));

  const auto& node2_size100 = stats_by_node_id.at(2).at(100);
  t.equal(node2_size100.alloc_objects, static_cast<uint64_t>(7));
  t.equal(node2_size100.alloc_space_bytes, static_cast<uint64_t>(700));
}
}  // namespace

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-function-type"
#endif
NODE_MODULE_INIT(/* exports, module, context */) {
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

  Tap t;
  const char* env_var = std::getenv("TEST");
  std::string name(env_var == nullptr ? "" : env_var);

  std::unordered_map<std::string, std::function<void(Tap&)>> tests = {
      {"BuildAllocationStatsByNodeId", TestBuildAllocationStatsByNodeId},
  };

  if (name.empty()) {
    t.plan(tests.size());
    for (auto test : tests) {
      t.test(test.first, test.second);
    }
  } else {
    t.plan(1);
    if (tests.count(name)) {
      t.test(name, tests[name]);
    } else {
      std::ostringstream s;
      s << "Unknown test: " << name;
      t.fail(s.str());
    }
  }

  // End test and set `process.exitCode`
  int exitCode = t.end();
  auto processKey = Nan::New<v8::String>("process").ToLocalChecked();
  auto process = Nan::Get(context->Global(), processKey).ToLocalChecked();
  Nan::Set(process.As<v8::Object>(),
           Nan::New<v8::String>("exitCode").ToLocalChecked(),
           Nan::New<v8::Number>(exitCode));
}
