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

NAN_MODULE_INIT(ExternalAllocationNode::Init) {
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

v8::Local<v8::Object> ExternalAllocationNode::New(
    std::shared_ptr<ExternalNode> node) {
  auto* isolate = v8::Isolate::GetCurrent();

  v8::Local<v8::Function> constructor =
      Nan::New(PerIsolateData::For(isolate)->AllocationNodeConstructor());

  v8::Local<v8::Object> obj = Nan::NewInstance(constructor).ToLocalChecked();

  auto* wrapper = new ExternalAllocationNode(node);
  wrapper->Wrap(obj);

  return obj;
}

NAN_GETTER(ExternalAllocationNode::GetName) {
  auto* wrapper =
      Nan::ObjectWrap::Unwrap<ExternalAllocationNode>(info.Holder());
  auto* isolate = v8::Isolate::GetCurrent();
  info.GetReturnValue().Set(
      v8::Local<v8::String>::New(isolate, wrapper->node_->name));
}

NAN_GETTER(ExternalAllocationNode::GetScriptName) {
  auto* wrapper =
      Nan::ObjectWrap::Unwrap<ExternalAllocationNode>(info.Holder());
  auto* isolate = v8::Isolate::GetCurrent();
  info.GetReturnValue().Set(
      v8::Local<v8::String>::New(isolate, wrapper->node_->script_name));
}

NAN_GETTER(ExternalAllocationNode::GetScriptId) {
  auto* wrapper =
      Nan::ObjectWrap::Unwrap<ExternalAllocationNode>(info.Holder());
  info.GetReturnValue().Set(Nan::New(wrapper->node_->script_id));
}

NAN_GETTER(ExternalAllocationNode::GetLineNumber) {
  auto* wrapper =
      Nan::ObjectWrap::Unwrap<ExternalAllocationNode>(info.Holder());
  info.GetReturnValue().Set(Nan::New(wrapper->node_->line_number));
}

NAN_GETTER(ExternalAllocationNode::GetColumnNumber) {
  auto* wrapper =
      Nan::ObjectWrap::Unwrap<ExternalAllocationNode>(info.Holder());
  info.GetReturnValue().Set(Nan::New(wrapper->node_->column_number));
}

NAN_GETTER(ExternalAllocationNode::GetAllocations) {
  auto* wrapper =
      Nan::ObjectWrap::Unwrap<ExternalAllocationNode>(info.Holder());
  auto* isolate = v8::Isolate::GetCurrent();
  auto context = isolate->GetCurrentContext();

  const auto& allocations = wrapper->node_->allocations;
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
  info.GetReturnValue().Set(arr);
}

NAN_GETTER(ExternalAllocationNode::GetChildren) {
  auto* wrapper =
      Nan::ObjectWrap::Unwrap<ExternalAllocationNode>(info.Holder());
  auto* isolate = v8::Isolate::GetCurrent();
  auto context = isolate->GetCurrentContext();

  const auto& children = wrapper->node_->children;
  v8::Local<v8::Array> arr = v8::Array::New(isolate, children.size());
  for (size_t i = 0; i < children.size(); i++) {
    arr->Set(context, i, ExternalAllocationNode::New(children[i])).Check();
  }
  info.GetReturnValue().Set(arr);
}

}  // namespace dd
