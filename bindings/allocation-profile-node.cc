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

NAN_MODULE_INIT(AllocationNodeWrapper::Init) {
  v8::Local<v8::FunctionTemplate> tpl = Nan::New<v8::FunctionTemplate>();
  tpl->SetClassName(Nan::New("AllocationProfileNode").ToLocalChecked());
  tpl->InstanceTemplate()->SetInternalFieldCount(1);

  Nan::SetPrototypeMethod(tpl, "getChildrenCount", GetChildrenCount);
  Nan::SetPrototypeMethod(tpl, "getChild", GetChild);
  Nan::SetPrototypeMethod(tpl, "dispose", Dispose);

  PerIsolateData::For(v8::Isolate::GetCurrent())
      ->AllocationNodeConstructor()
      .Reset(Nan::GetFunction(tpl).ToLocalChecked());
}

v8::Local<v8::Object> AllocationNodeWrapper::New(
    std::shared_ptr<AllocationProfileHolder> holder, Node* node) {
  auto* isolate = v8::Isolate::GetCurrent();

  v8::Local<v8::Function> constructor =
      Nan::New(PerIsolateData::For(isolate)->AllocationNodeConstructor());

  v8::Local<v8::Object> obj = Nan::NewInstance(constructor).ToLocalChecked();

  auto* wrapper = new AllocationNodeWrapper(holder, node);
  wrapper->Wrap(obj);
  wrapper->PopulateFields(obj);

  return obj;
}

void AllocationNodeWrapper::PopulateFields(v8::Local<v8::Object> obj) {
  auto* isolate = v8::Isolate::GetCurrent();
  auto context = isolate->GetCurrentContext();

  Nan::Set(obj,
           Nan::New("name").ToLocalChecked(),
           Nan::New(node_->name).ToLocalChecked());
  Nan::Set(obj,
           Nan::New("scriptName").ToLocalChecked(),
           Nan::New(node_->script_name).ToLocalChecked());
  Nan::Set(
      obj, Nan::New("scriptId").ToLocalChecked(), Nan::New(node_->script_id));
  Nan::Set(obj,
           Nan::New("lineNumber").ToLocalChecked(),
           Nan::New(node_->line_number));
  Nan::Set(obj,
           Nan::New("columnNumber").ToLocalChecked(),
           Nan::New(node_->column_number));

  v8::Local<v8::Array> allocations =
      v8::Array::New(isolate, node_->allocations.size());
  for (size_t i = 0; i < node_->allocations.size(); i++) {
    const auto& alloc = node_->allocations[i];
    v8::Local<v8::Object> alloc_obj = v8::Object::New(isolate);
    Nan::Set(alloc_obj,
             Nan::New("sizeBytes").ToLocalChecked(),
             Nan::New<v8::Number>(static_cast<double>(alloc.size)));
    Nan::Set(alloc_obj,
             Nan::New("count").ToLocalChecked(),
             Nan::New<v8::Number>(static_cast<double>(alloc.count)));
    allocations->Set(context, i, alloc_obj).Check();
  }
  Nan::Set(obj, Nan::New("allocations").ToLocalChecked(), allocations);
}

NAN_METHOD(AllocationNodeWrapper::GetChildrenCount) {
  auto* wrapper = Nan::ObjectWrap::Unwrap<AllocationNodeWrapper>(info.Holder());
  info.GetReturnValue().Set(
      Nan::New(static_cast<uint32_t>(wrapper->node_->children.size())));
}

NAN_METHOD(AllocationNodeWrapper::GetChild) {
  auto* wrapper = Nan::ObjectWrap::Unwrap<AllocationNodeWrapper>(info.Holder());

  if (info.Length() < 1 || !info[0]->IsUint32()) {
    return Nan::ThrowTypeError("Index must be a uint32.");
  }

  uint32_t index = Nan::To<uint32_t>(info[0]).FromJust();
  const auto& children = wrapper->node_->children;

  if (index >= children.size()) {
    return Nan::ThrowRangeError("Child index out of bounds");
  }

  auto child =
      AllocationNodeWrapper::New(wrapper->holder_, children[index].get());
  info.GetReturnValue().Set(child);
}

NAN_METHOD(AllocationNodeWrapper::Dispose) {
  auto* wrapper = Nan::ObjectWrap::Unwrap<AllocationNodeWrapper>(info.Holder());
  if (wrapper->holder_) {
    wrapper->holder_->Dispose();
  }
}

v8::Local<v8::Object> V8AllocationNodeWrapper::New(
    std::shared_ptr<V8AllocationProfileHolder> holder,
    v8::AllocationProfile::Node* node) {
  v8::Local<v8::FunctionTemplate> tpl = Nan::New<v8::FunctionTemplate>();
  tpl->SetClassName(Nan::New("V8AllocationProfileNode").ToLocalChecked());
  tpl->InstanceTemplate()->SetInternalFieldCount(1);

  Nan::SetPrototypeMethod(tpl, "getChildrenCount", GetChildrenCount);
  Nan::SetPrototypeMethod(tpl, "getChild", GetChild);
  Nan::SetPrototypeMethod(tpl, "dispose", Dispose);

  v8::Local<v8::Object> obj =
      Nan::NewInstance(Nan::GetFunction(tpl).ToLocalChecked()).ToLocalChecked();

  auto* wrapper = new V8AllocationNodeWrapper(holder, node);
  wrapper->Wrap(obj);
  wrapper->PopulateFields(obj);

  return obj;
}

void V8AllocationNodeWrapper::PopulateFields(v8::Local<v8::Object> obj) {
  auto* isolate = v8::Isolate::GetCurrent();
  auto context = isolate->GetCurrentContext();

  Nan::Set(obj, Nan::New("name").ToLocalChecked(), node_->name);
  Nan::Set(obj, Nan::New("scriptName").ToLocalChecked(), node_->script_name);
  Nan::Set(
      obj, Nan::New("scriptId").ToLocalChecked(), Nan::New(node_->script_id));
  Nan::Set(obj,
           Nan::New("lineNumber").ToLocalChecked(),
           Nan::New(node_->line_number));
  Nan::Set(obj,
           Nan::New("columnNumber").ToLocalChecked(),
           Nan::New(node_->column_number));

  v8::Local<v8::Array> allocations =
      v8::Array::New(isolate, node_->allocations.size());
  for (size_t i = 0; i < node_->allocations.size(); i++) {
    const auto& alloc = node_->allocations[i];
    v8::Local<v8::Object> alloc_obj = v8::Object::New(isolate);
    Nan::Set(alloc_obj,
             Nan::New("sizeBytes").ToLocalChecked(),
             Nan::New<v8::Number>(static_cast<double>(alloc.size)));
    Nan::Set(alloc_obj,
             Nan::New("count").ToLocalChecked(),
             Nan::New<v8::Number>(static_cast<double>(alloc.count)));
    allocations->Set(context, i, alloc_obj).Check();
  }
  Nan::Set(obj, Nan::New("allocations").ToLocalChecked(), allocations);
}

NAN_METHOD(V8AllocationNodeWrapper::GetChildrenCount) {
  auto* wrapper =
      Nan::ObjectWrap::Unwrap<V8AllocationNodeWrapper>(info.Holder());

  info.GetReturnValue().Set(
      Nan::New(static_cast<uint32_t>(wrapper->node_->children.size())));
}

NAN_METHOD(V8AllocationNodeWrapper::GetChild) {
  auto* wrapper =
      Nan::ObjectWrap::Unwrap<V8AllocationNodeWrapper>(info.Holder());

  if (info.Length() < 1 || !info[0]->IsUint32()) {
    return Nan::ThrowTypeError("Index must be a uint32.");
  }

  uint32_t index = Nan::To<uint32_t>(info[0]).FromJust();
  const auto& children = wrapper->node_->children;

  if (index >= children.size()) {
    return Nan::ThrowRangeError("Child index out of bounds");
  }

  auto child = V8AllocationNodeWrapper::New(wrapper->holder_, children[index]);
  info.GetReturnValue().Set(child);
}

NAN_METHOD(V8AllocationNodeWrapper::Dispose) {
  auto* wrapper =
      Nan::ObjectWrap::Unwrap<V8AllocationNodeWrapper>(info.Holder());
  if (wrapper->holder_) {
    wrapper->holder_->Dispose();
  }
}

}  // namespace dd
