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

struct AllocationProfileHolder {
  std::shared_ptr<Node> profile;

  void Dispose() {
    profile.reset();
  }
};

class AllocationNodeWrapper : public Nan::ObjectWrap {
 public:
  static NAN_MODULE_INIT(Init);

  static v8::Local<v8::Object> New(
      std::shared_ptr<AllocationProfileHolder> holder,
      Node* node);

 private:
  AllocationNodeWrapper(std::shared_ptr<AllocationProfileHolder> holder,
                        Node* node)
      : holder_(holder), node_(node) {}

  void PopulateFields(v8::Local<v8::Object> obj);

  static NAN_METHOD(GetChildrenCount);
  static NAN_METHOD(GetChild);
  static NAN_METHOD(Dispose);

  std::shared_ptr<AllocationProfileHolder> holder_;
  Node* node_;
};

}  // namespace dd
