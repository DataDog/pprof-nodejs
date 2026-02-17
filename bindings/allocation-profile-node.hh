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

namespace dd {

class AllocationProfileNodeView : public Nan::ObjectWrap {
 public:
  static NAN_MODULE_INIT(Init);

  static v8::Local<v8::Object> New(v8::AllocationProfile::Node* node);

 private:
  AllocationProfileNodeView(v8::AllocationProfile::Node* node) : node_(node) {}

  template <typename F>
  static void mapAllocationProfileNode(
      const Nan::PropertyCallbackInfo<v8::Value>& info, F&& mapper);

  static NAN_GETTER(GetName);
  static NAN_GETTER(GetScriptName);
  static NAN_GETTER(GetScriptId);
  static NAN_GETTER(GetLineNumber);
  static NAN_GETTER(GetColumnNumber);
  static NAN_GETTER(GetAllocations);
  static NAN_GETTER(GetChildren);

  v8::AllocationProfile::Node* node_;
};

}  // namespace dd
