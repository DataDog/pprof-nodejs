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

#include <algorithm>
#include <vector>

using namespace v8;

namespace dd {

template <typename F>
void AllocationProfileNodeView::mapAllocationProfileNode(
    const Nan::PropertyCallbackInfo<Value>& info, F&& mapper) {
  auto* node = static_cast<AllocationProfile::Node*>(
      Nan::GetInternalFieldPointer(info.Holder(), 0));
  auto* allocation_stats = static_cast<const AllocationProfileNodeStatsMap*>(
      Nan::GetInternalFieldPointer(info.Holder(), 1));
  info.GetReturnValue().Set(mapper(node, allocation_stats));
}

NAN_MODULE_INIT(AllocationProfileNodeView::Init) {
  Local<FunctionTemplate> tpl = Nan::New<FunctionTemplate>();
  tpl->SetClassName(Nan::New("AllocationProfileNode").ToLocalChecked());
  tpl->InstanceTemplate()->SetInternalFieldCount(2);

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

Local<Object> AllocationProfileNodeView::New(
    AllocationProfile::Node* node,
    const AllocationProfileNodeStatsMap* allocation_stats) {
  auto* isolate = Isolate::GetCurrent();

  Local<Function> constructor =
      Nan::New(PerIsolateData::For(isolate)->AllocationNodeConstructor());

  Local<Object> obj = Nan::NewInstance(constructor).ToLocalChecked();

  Nan::SetInternalFieldPointer(obj, 0, node);
  Nan::SetInternalFieldPointer(
      obj, 1, const_cast<AllocationProfileNodeStatsMap*>(allocation_stats));

  return obj;
}

NAN_GETTER(AllocationProfileNodeView::GetName) {
  mapAllocationProfileNode(
      info,
      [](AllocationProfile::Node* node, const AllocationProfileNodeStatsMap*) {
        return node->name;
      });
}

NAN_GETTER(AllocationProfileNodeView::GetScriptName) {
  mapAllocationProfileNode(
      info,
      [](AllocationProfile::Node* node, const AllocationProfileNodeStatsMap*) {
        return node->script_name;
      });
}

NAN_GETTER(AllocationProfileNodeView::GetScriptId) {
  mapAllocationProfileNode(
      info,
      [](AllocationProfile::Node* node, const AllocationProfileNodeStatsMap*) {
        return node->script_id;
      });
}

NAN_GETTER(AllocationProfileNodeView::GetLineNumber) {
  mapAllocationProfileNode(
      info,
      [](AllocationProfile::Node* node, const AllocationProfileNodeStatsMap*) {
        return node->line_number;
      });
}

NAN_GETTER(AllocationProfileNodeView::GetColumnNumber) {
  mapAllocationProfileNode(
      info,
      [](AllocationProfile::Node* node, const AllocationProfileNodeStatsMap*) {
        return node->column_number;
      });
}

NAN_GETTER(AllocationProfileNodeView::GetAllocations) {
  mapAllocationProfileNode(
      info,
      [](AllocationProfile::Node* node,
         const AllocationProfileNodeStatsMap* allocation_stats) {
        auto* isolate = Isolate::GetCurrent();
        auto context = isolate->GetCurrentContext();

        if (allocation_stats) {
          const auto it = allocation_stats->find(node->node_id);
          if (it == allocation_stats->end() || it->second.empty()) {
            return Local<Array>(Array::New(isolate, 0));
          }

          std::vector<size_t> sizes;
          sizes.reserve(it->second.size());
          for (const auto& allocation : it->second) {
            sizes.push_back(allocation.first);
          }
          std::sort(sizes.begin(), sizes.end());

          Local<Array> arr = Array::New(isolate, sizes.size());
          for (size_t i = 0; i < sizes.size(); i++) {
            const auto size = sizes[i];
            const auto& stats = it->second.at(size);
            Local<Object> alloc_obj = Object::New(isolate);
            Nan::Set(alloc_obj,
                     String::NewFromUtf8Literal(isolate, "sizeBytes"),
                     Number::New(isolate, static_cast<double>(size)));
            Nan::Set(
                alloc_obj,
                String::NewFromUtf8Literal(isolate, "count"),
                Number::New(isolate, static_cast<double>(stats.total_count)));
            Nan::Set(
                alloc_obj,
                String::NewFromUtf8Literal(isolate, "liveSizeBytes"),
                Number::New(isolate, static_cast<double>(stats.live_size)));
            Nan::Set(
                alloc_obj,
                String::NewFromUtf8Literal(isolate, "liveCount"),
                Number::New(isolate, static_cast<double>(stats.live_count)));
            Nan::Set(
                alloc_obj,
                String::NewFromUtf8Literal(isolate, "totalSizeBytes"),
                Number::New(isolate, static_cast<double>(stats.total_size)));
            Nan::Set(
                alloc_obj,
                String::NewFromUtf8Literal(isolate, "totalCount"),
                Number::New(isolate, static_cast<double>(stats.total_count)));
            arr->Set(context, i, alloc_obj).Check();
          }
          return arr;
        }

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
  mapAllocationProfileNode(
      info,
      [](AllocationProfile::Node* node,
         const AllocationProfileNodeStatsMap* allocation_stats) {
        auto* isolate = Isolate::GetCurrent();
        auto context = isolate->GetCurrentContext();

        const auto& children = node->children;
        Local<Array> arr = Array::New(isolate, children.size());
        for (size_t i = 0; i < children.size(); i++) {
          arr->Set(
                 context,
                 i,
                 AllocationProfileNodeView::New(children[i], allocation_stats))
              .Check();
        }
        return arr;
      });
}

}  // namespace dd
