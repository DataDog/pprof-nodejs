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

#include <nan.h>
#include <memory>

#include "translate-heap-profile.hh"

namespace dd {

class ExternalAllocationNode : public Nan::ObjectWrap {
 public:
  static NAN_MODULE_INIT(Init);

  static v8::Local<v8::Object> New(std::shared_ptr<ExternalNode> node);

 private:
  ExternalAllocationNode(std::shared_ptr<ExternalNode> node) : node_(node) {}

  static NAN_GETTER(GetName);
  static NAN_GETTER(GetScriptName);
  static NAN_GETTER(GetScriptId);
  static NAN_GETTER(GetLineNumber);
  static NAN_GETTER(GetColumnNumber);
  static NAN_GETTER(GetAllocations);
  static NAN_GETTER(GetChildren);

  std::shared_ptr<ExternalNode> node_;
};

}  // namespace dd
