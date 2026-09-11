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

#include <nan.h>
#include <node.h>
#include <v8-version.h>
#include <v8.h>
#include <array>
#include <cstddef>
#include <memory>
#include <string_view>

// v8::DictionaryTemplate landed in V8 12.3, i.e. Node.js >= 22.
#if V8_MAJOR_VERSION > 12 || (V8_MAJOR_VERSION == 12 && V8_MINOR_VERSION >= 3)
#define DD_V8_HAS_DICTIONARY_TEMPLATE 1
#else
#define DD_V8_HAS_DICTIONARY_TEMPLATE 0
#endif

namespace dd {

struct HeapProfilerState;

#if DD_V8_HAS_DICTIONARY_TEMPLATE
enum class DictionaryTemplateId : size_t {
  kWallSampleContext,
  kCount,
};

constexpr size_t kDictionaryTemplateCount =
    static_cast<size_t>(DictionaryTemplateId::kCount);
#endif

class PerIsolateData {
 private:
  Nan::Global<v8::Function> wall_profiler_constructor;
  Nan::Global<v8::Function> allocation_node_constructor;
  Nan::Global<v8::Function> time_profile_node_constructor;
  std::shared_ptr<HeapProfilerState> heap_profiler_state;
#if DD_V8_HAS_DICTIONARY_TEMPLATE
  std::array<Nan::Global<v8::DictionaryTemplate>, kDictionaryTemplateCount>
      dictionary_templates;
  std::array<size_t, kDictionaryTemplateCount> dictionary_template_arities{};
#endif

  PerIsolateData() {}

 public:
  static PerIsolateData* For(v8::Isolate* isolate);

  Nan::Global<v8::Function>& WallProfilerConstructor();
  Nan::Global<v8::Function>& AllocationNodeConstructor();
  Nan::Global<v8::Function>& TimeProfileNodeConstructor();
  std::shared_ptr<HeapProfilerState>& GetHeapProfilerState();
#if DD_V8_HAS_DICTIONARY_TEMPLATE
  v8::Local<v8::DictionaryTemplate> GetDictionaryTemplate(
      v8::Isolate* isolate,
      DictionaryTemplateId id,
      v8::MemorySpan<const std::string_view> names);
#endif
};

}  // namespace dd
