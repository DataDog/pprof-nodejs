/**
 * Copyright 2018 Google Inc. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
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
#include <cstdint>
#include <memory>
#include <vector>
#include "contexts.hh"

namespace dd {

struct TimeProfileNodeInfo;

// Shared state for the lazy profile view.
// In line-info mode, owned_nodes keeps synthetic TimeProfileNodeInfo objects
// alive for as long as JS wrappers may reference them.
// In normal mode, owned_nodes stays empty - wrappers point directly to V8
// nodes.
struct TimeProfileViewState {
  bool include_line_info;
  ContextsByNode* contexts_by_node;
  std::vector<std::unique_ptr<TimeProfileNodeInfo>> owned_nodes;
};

// Line-info mode only: stored in internal field 0 of JS wrappers.
// Needed because line-info nodes have line/column/hitCount values
// and a metadata_node that differs from the traversal node.
struct TimeProfileNodeInfo {
  const v8::CpuProfileNode* node;
  const v8::CpuProfileNode* metadata_node;
  int line_number;
  int column_number;
  int hit_count;
  bool is_line_root;
  TimeProfileViewState* state;
};

class TimeProfileNodeView {
 public:
  static NAN_MODULE_INIT(Init);
};

v8::Local<v8::Value> BuildTimeProfileView(const v8::CpuProfile* profile,
                                          bool has_cpu_time,
                                          int64_t non_js_threads_cpu_time,
                                          TimeProfileViewState& state);

v8::Local<v8::Value> TranslateTimeProfile(
    const v8::CpuProfile* profile,
    bool includeLineInfo,
    ContextsByNode* contextsByNode = nullptr,
    bool hasCpuTime = false,
    int64_t nonJSThreadsCpuTime = 0);

}  // namespace dd
