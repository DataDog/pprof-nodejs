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

#include "allocation-profile-node.hh"
#include "per-isolate-data.hh"

namespace dd {

template <typename F>
void AllocationProfileNodeView::mapAllocationProfileNode(
    const Nan::PropertyCallbackInfo<v8::Value>& info, F&& mapper) {
  auto* wrapper =
      Nan::ObjectWrap::Unwrap<AllocationProfileNodeView>(info.Holder());
  info.GetReturnValue().Set(mapper(wrapper->node_));
}

NAN_MODULE_INIT(AllocationProfileNodeView::Init) {
  v8::Local<v8::FunctionTemplate> tpl = Nan::New<v8::FunctionTemplate>();
  tpl->SetClassName(Nan::New("AllocationProfileNode").ToLocalChecked());
  tpl->InstanceTemplate()->SetInternalFieldCount(1);

  auto inst = tpl->InstanceTemplate();
  Nan::SetAccessor(inst, Nan::New("name").ToLocalChecked(), GetName);
  Nan::SetAccessor(
      inst, Nan::New("scriptName").ToLocalChecked(), GetScriptName);
  Nan::SetAccessor(inst, Nan::New("scriptId").ToLocalChecked(), GetScriptId);
  Nan::SetAccessor(
      inst, Nan::New("lineNumber").ToLocalChecked(), GetLineNumber);
  Nan::SetAccessor(
      inst, Nan::New("columnNumber").ToLocalChecked(), GetColumnNumber);
  Nan::SetAccessor(
      inst, Nan::New("allocations").ToLocalChecked(), GetAllocations);
  Nan::SetAccessor(inst, Nan::New("children").ToLocalChecked(), GetChildren);

  PerIsolateData::For(v8::Isolate::GetCurrent())
      ->AllocationNodeConstructor()
      .Reset(Nan::GetFunction(tpl).ToLocalChecked());
}

v8::Local<v8::Object> AllocationProfileNodeView::New(
    v8::AllocationProfile::Node* node) {
  auto* isolate = v8::Isolate::GetCurrent();

  v8::Local<v8::Function> constructor =
      Nan::New(PerIsolateData::For(isolate)->AllocationNodeConstructor());

  v8::Local<v8::Object> obj = Nan::NewInstance(constructor).ToLocalChecked();

  auto* wrapper = new AllocationProfileNodeView(node);
  wrapper->Wrap(obj);

  return obj;
}

NAN_GETTER(AllocationProfileNodeView::GetName) {
  mapAllocationProfileNode(
      info, [](v8::AllocationProfile::Node* node) { return node->name; });
}

NAN_GETTER(AllocationProfileNodeView::GetScriptName) {
  mapAllocationProfileNode(info, [](v8::AllocationProfile::Node* node) {
    return node->script_name;
  });
}

NAN_GETTER(AllocationProfileNodeView::GetScriptId) {
  mapAllocationProfileNode(
      info, [](v8::AllocationProfile::Node* node) { return node->script_id; });
}

NAN_GETTER(AllocationProfileNodeView::GetLineNumber) {
  mapAllocationProfileNode(info, [](v8::AllocationProfile::Node* node) {
    return node->line_number;
  });
}

NAN_GETTER(AllocationProfileNodeView::GetColumnNumber) {
  mapAllocationProfileNode(info, [](v8::AllocationProfile::Node* node) {
    return node->column_number;
  });
}

NAN_GETTER(AllocationProfileNodeView::GetAllocations) {
  mapAllocationProfileNode(info, [](v8::AllocationProfile::Node* node) {
    auto* isolate = v8::Isolate::GetCurrent();
    auto context = isolate->GetCurrentContext();

    const auto& allocations = node->allocations;
    v8::Local<v8::Array> arr = v8::Array::New(isolate, allocations.size());
    for (size_t i = 0; i < allocations.size(); i++) {
      const auto& alloc = allocations[i];
      v8::Local<v8::Object> alloc_obj = v8::Object::New(isolate);
      Nan::Set(alloc_obj,
               Nan::New("sizeBytes").ToLocalChecked(),
               Nan::New<v8::Number>(static_cast<double>(alloc.size)));
      Nan::Set(alloc_obj,
               Nan::New("count").ToLocalChecked(),
               Nan::New<v8::Number>(static_cast<double>(alloc.count)));
      arr->Set(context, i, alloc_obj).Check();
    }
    return arr;
  });
}

NAN_GETTER(AllocationProfileNodeView::GetChildren) {
  mapAllocationProfileNode(info, [](v8::AllocationProfile::Node* node) {
    auto* isolate = v8::Isolate::GetCurrent();
    auto context = isolate->GetCurrentContext();

    const auto& children = node->children;
    v8::Local<v8::Array> arr = v8::Array::New(isolate, children.size());
    for (size_t i = 0; i < children.size(); i++) {
      arr->Set(context, i, AllocationProfileNodeView::New(children[i])).Check();
    }
    return arr;
  });
}

}  // namespace dd
