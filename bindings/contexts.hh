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

#pragma once

#include <v8-profiler.h>
#include <unordered_map>

namespace dd {

struct NodeInfo {
  v8::Local<v8::Array> contexts;
  uint32_t hitcount;
};

using ContextsByNode = std::unordered_map<const v8::CpuProfileNode*, NodeInfo>;
}  // namespace dd
