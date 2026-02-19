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

#include "allocation-profile-node.hh"
#include "per-isolate-data.hh"

using namespace v8;

namespace dd {

template <typename F>
void AllocationProfileNodeView::mapAllocationProfileNode(
    const Nan::PropertyCallbackInfo<Value>& info, F&& mapper) {
  auto* node = static_cast<AllocationProfile::Node*>(
      Nan::GetInternalFieldPointer(info.Holder(), 0));
  info.GetReturnValue().Set(mapper(node));
}

NAN_MODULE_INIT(AllocationProfileNodeView::Init) {
  Local<FunctionTemplate> tpl = Nan::New<FunctionTemplate>();
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

  PerIsolateData::For(Isolate::GetCurrent())
      ->AllocationNodeConstructor()
      .Reset(Nan::GetFunction(tpl).ToLocalChecked());
}

Local<Object> AllocationProfileNodeView::New(AllocationProfile::Node* node) {
  auto* isolate = Isolate::GetCurrent();

  Local<Function> constructor =
      Nan::New(PerIsolateData::For(isolate)->AllocationNodeConstructor());

  Local<Object> obj = Nan::NewInstance(constructor).ToLocalChecked();

  Nan::SetInternalFieldPointer(obj, 0, node);

  return obj;
}

NAN_GETTER(AllocationProfileNodeView::GetName) {
  mapAllocationProfileNode(
      info, [](AllocationProfile::Node* node) { return node->name; });
}

NAN_GETTER(AllocationProfileNodeView::GetScriptName) {
  mapAllocationProfileNode(
      info, [](AllocationProfile::Node* node) { return node->script_name; });
}

NAN_GETTER(AllocationProfileNodeView::GetScriptId) {
  mapAllocationProfileNode(
      info, [](AllocationProfile::Node* node) { return node->script_id; });
}

NAN_GETTER(AllocationProfileNodeView::GetLineNumber) {
  mapAllocationProfileNode(
      info, [](AllocationProfile::Node* node) { return node->line_number; });
}

NAN_GETTER(AllocationProfileNodeView::GetColumnNumber) {
  mapAllocationProfileNode(
      info, [](AllocationProfile::Node* node) { return node->column_number; });
}

NAN_GETTER(AllocationProfileNodeView::GetAllocations) {
  mapAllocationProfileNode(info, [](AllocationProfile::Node* node) {
    auto* isolate = Isolate::GetCurrent();
    auto context = isolate->GetCurrentContext();

    const auto& allocations = node->allocations;
    Local<Array> arr = Array::New(isolate, allocations.size());
    auto sizeBytes = String::NewFromUtf8Literal(isolate, "sizeBytes");
    auto count = String::NewFromUtf8Literal(isolate, "count");

    for (size_t i = 0; i < allocations.size(); i++) {
      const auto& alloc = allocations[i];
      Local<Object> alloc_obj = Object::New(isolate);
      Nan::Set(alloc_obj,
               sizeBytes,
               Number::New(isolate, static_cast<double>(alloc.size)));
      Nan::Set(alloc_obj,
               count,
               Number::New(isolate, static_cast<double>(alloc.count)));
      arr->Set(context, i, alloc_obj).Check();
    }
    return arr;
  });
}

NAN_GETTER(AllocationProfileNodeView::GetChildren) {
  mapAllocationProfileNode(info, [](AllocationProfile::Node* node) {
    auto* isolate = Isolate::GetCurrent();
    auto context = isolate->GetCurrentContext();

    const auto& children = node->children;
    Local<Array> arr = Array::New(isolate, children.size());
    for (size_t i = 0; i < children.size(); i++) {
      arr->Set(context, i, AllocationProfileNodeView::New(children[i])).Check();
    }
    return arr;
  });
}

}  // namespace dd
