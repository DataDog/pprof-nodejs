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

#include "translate-heap-profile.hh"
#include <nan.h>
#include "profile-translator.hh"

namespace dd {

namespace {
const AllocationProfileSizeStatsMap* FindNodeStats(
    const AllocationProfileNodeStatsMap& allocation_stats, uint32_t node_id) {
  auto node_stats = allocation_stats.find(node_id);
  return node_stats == allocation_stats.end() ? nullptr : &node_stats->second;
}

class HeapProfileTranslator : ProfileTranslator {
#define NODE_FIELDS                                                            \
  X(name)                                                                      \
  X(scriptName)                                                                \
  X(scriptId)                                                                  \
  X(lineNumber)                                                                \
  X(columnNumber)                                                              \
  X(children)                                                                  \
  X(allocations)

#define ALLOCATION_FIELDS                                                      \
  X(sizeBytes)                                                                 \
  X(count)

#define X(name) v8::Local<v8::String> str_##name = NewString(#name);
  NODE_FIELDS
  ALLOCATION_FIELDS
#undef X

 public:
  v8::Local<v8::Value> TranslateAllocationProfile(
      v8::AllocationProfile::Node* node,
      const AllocationProfileNodeStatsMap* allocation_stats) {
    v8::Local<v8::Array> children = NewArray(node->children.size());
    for (size_t i = 0; i < node->children.size(); i++) {
      Set(children,
          i,
          TranslateAllocationProfile(node->children[i], allocation_stats));
    }

    return CreateNode(
        node->name,
        node->script_name,
        NewInteger(node->script_id),
        NewInteger(node->line_number),
        NewInteger(node->column_number),
        children,
        allocation_stats
            ? TranslateAllocationStats(
                  isolate, FindNodeStats(*allocation_stats, node->node_id))
            : TranslateAllocations(node->allocations));
  }

  v8::Local<v8::Value> TranslateAllocationProfile(Node* node) {
    v8::Local<v8::Array> children = NewArray(node->children.size());
    for (size_t i = 0; i < node->children.size(); i++) {
      Set(children, i, TranslateAllocationProfile(node->children[i].get()));
    }

    v8::Local<v8::Array> allocations = NewArray(node->allocations.size());
    for (size_t i = 0; i < node->allocations.size(); i++) {
      const auto& alloc = node->allocations[i];
      Set(allocations,
          i,
          node->has_allocation_stats ? CreateAllocationStats(alloc)
                                     : CreateAllocation(NewNumber(alloc.count),
                                                        NewNumber(alloc.size)));
    }

    return CreateNode(NewString(node->name.c_str()),
                      NewString(node->script_name.c_str()),
                      NewInteger(node->script_id),
                      NewInteger(node->line_number),
                      NewInteger(node->column_number),
                      children,
                      allocations);
  }

 private:
  v8::Local<v8::Array> TranslateAllocations(
      const std::vector<v8::AllocationProfile::Allocation>& node_allocations) {
    v8::Local<v8::Array> allocations = NewArray(node_allocations.size());
    for (size_t i = 0; i < node_allocations.size(); i++) {
      auto alloc = node_allocations[i];
      Set(allocations,
          i,
          CreateAllocation(NewNumber(alloc.count), NewNumber(alloc.size)));
    }
    return allocations;
  }

  v8::Local<v8::Object> CreateNode(v8::Local<v8::String> name,
                                   v8::Local<v8::String> scriptName,
                                   v8::Local<v8::Integer> scriptId,
                                   v8::Local<v8::Integer> lineNumber,
                                   v8::Local<v8::Integer> columnNumber,
                                   v8::Local<v8::Array> children,
                                   v8::Local<v8::Array> allocations) {
    v8::Local<v8::Object> js_node = NewObject();
#define X(name) Set(js_node, str_##name, name);
    NODE_FIELDS
#undef X
#undef NODE_FIELDS
    return js_node;
  }

  v8::Local<v8::Object> CreateAllocation(v8::Local<v8::Number> count,
                                         v8::Local<v8::Number> sizeBytes) {
    v8::Local<v8::Object> js_alloc = NewObject();
#define X(name) Set(js_alloc, str_##name, name);
    ALLOCATION_FIELDS
#undef X
#undef ALLOCATION_FIELDS
    return js_alloc;
  }

  // Shares the object shape with the non-detached path in allocation-profile.
  v8::Local<v8::Object> CreateAllocationStats(const Node::Allocation& alloc) {
    AllocationProfileNodeStats stats;
    stats.inuse_objects = alloc.inuse_objects;
    stats.alloc_objects = alloc.alloc_objects;
    stats.inuse_space_bytes = alloc.inuse_objects * alloc.size;
    stats.alloc_space_bytes = alloc.alloc_objects * alloc.size;
    return CreateAllocationObject(isolate, stats);
  }

 public:
  explicit HeapProfileTranslator() {}
};
}  // namespace

std::shared_ptr<Node> TranslateAllocationProfileToCpp(
    v8::AllocationProfile::Node* node,
    const AllocationProfileNodeStatsMap* allocation_stats) {
  auto new_node = std::make_shared<Node>();
  new_node->line_number = node->line_number;
  new_node->column_number = node->column_number;
  new_node->script_id = node->script_id;
  Nan::Utf8String name(node->name);
  new_node->name.assign(*name, name.length());
  Nan::Utf8String script_name(node->script_name);
  new_node->script_name.assign(*script_name, script_name.length());

  new_node->children.reserve(node->children.size());
  for (auto& child : node->children) {
    new_node->children.push_back(
        TranslateAllocationProfileToCpp(child, allocation_stats));
  }

  // Join now: the samples these node_ids key into are freed on return.
  new_node->has_allocation_stats = allocation_stats != nullptr;
  const auto* stats = allocation_stats
                          ? FindNodeStats(*allocation_stats, node->node_id)
                          : nullptr;
  new_node->allocations.reserve(node->allocations.size());
  for (const auto& alloc : node->allocations) {
    Node::Allocation out;
    out.size = alloc.size;
    out.count = alloc.count;
    out.inuse_objects = alloc.count;
    out.alloc_objects = alloc.count;
    if (stats) {
      auto size_stats = stats->find(alloc.size);
      if (size_stats != stats->end()) {
        out.inuse_objects = size_stats->second.inuse_objects;
        out.alloc_objects = size_stats->second.alloc_objects;
      }
    }
    new_node->allocations.push_back(out);
  }
  return new_node;
}

v8::Local<v8::Value> TranslateAllocationProfile(
    v8::AllocationProfile::Node* node,
    const AllocationProfileNodeStatsMap* allocation_stats) {
  return HeapProfileTranslator().TranslateAllocationProfile(node,
                                                            allocation_stats);
}

v8::Local<v8::Value> TranslateAllocationProfile(Node* node) {
  return HeapProfileTranslator().TranslateAllocationProfile(node);
}

}  // namespace dd
