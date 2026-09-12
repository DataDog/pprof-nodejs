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
#include <algorithm>
#include <string_view>
#include "per-isolate-data.hh"
#include "profile-translator.hh"

namespace dd {

namespace {
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

#define ALLOCATION_STATS_FIELDS                                                \
  X(inuseObjects, inuse_objects)                                               \
  X(inuseSpaceBytes, inuse_space_bytes)                                        \
  X(allocObjects, alloc_objects)                                               \
  X(allocSpaceBytes, alloc_space_bytes)

#if DD_V8_HAS_DICTIONARY_TEMPLATE
  // NewInstance binds names and values by position, so both come from the same
  // field lists.
#define X(name) #name,
  static constexpr std::string_view kNodeNames[] = {NODE_FIELDS};
  static constexpr std::string_view kAllocationNames[] = {ALLOCATION_FIELDS};
#undef X
#define X(name, member) #name,
  static constexpr std::string_view kAllocationStatsNames[] = {
      ALLOCATION_STATS_FIELDS};
#undef X
  PerIsolateData* perIsolateData = PerIsolateData::For(isolate);
  v8::Local<v8::DictionaryTemplate> nodeTemplate =
      perIsolateData->GetDictionaryTemplate(
          isolate, DictionaryTemplateId::kHeapProfileNode, kNodeNames);
  v8::Local<v8::DictionaryTemplate> allocationTemplate =
      perIsolateData->GetDictionaryTemplate(
          isolate, DictionaryTemplateId::kHeapAllocation, kAllocationNames);
  v8::Local<v8::DictionaryTemplate> allocationStatsTemplate =
      perIsolateData->GetDictionaryTemplate(
          isolate,
          DictionaryTemplateId::kHeapAllocationStats,
          kAllocationStatsNames);
#else
#define X(name) v8::Local<v8::String> str_##name = NewString(#name);
  NODE_FIELDS
  ALLOCATION_FIELDS
#undef X
#endif

 public:
  v8::Local<v8::Value> TranslateAllocationProfile(
      v8::AllocationProfile::Node* node) {
    v8::Local<v8::Array> children = NewArray(node->children.size());
    for (size_t i = 0; i < node->children.size(); i++) {
      Set(children, i, TranslateAllocationProfile(node->children[i]));
    }

    v8::Local<v8::Array> allocations = NewArray(node->allocations.size());
    for (size_t i = 0; i < node->allocations.size(); i++) {
      auto alloc = node->allocations[i];
      Set(allocations,
          i,
          CreateAllocation(NewNumber(alloc.size), NewNumber(alloc.count)));
    }

    return CreateNode(node->name,
                      node->script_name,
                      NewInteger(node->script_id),
                      NewInteger(node->line_number),
                      NewInteger(node->column_number),
                      children,
                      allocations);
  }

  v8::Local<v8::Value> TranslateAllocationProfile(
      v8::AllocationProfile::Node* node,
      const AllocationProfileNodeStatsMap* allocation_stats) {
    if (!allocation_stats) {
      return TranslateAllocationProfile(node);
    }

    v8::Local<v8::Array> children = NewArray(node->children.size());
    for (size_t i = 0; i < node->children.size(); i++) {
      Set(children,
          i,
          TranslateAllocationProfile(node->children[i], allocation_stats));
    }

    auto node_stats = allocation_stats->find(node->node_id);
    v8::Local<v8::Array> allocations = TranslateAllocationStats(
        node_stats == allocation_stats->end() ? nullptr : &node_stats->second);

    return CreateNode(node->name,
                      node->script_name,
                      NewInteger(node->script_id),
                      NewInteger(node->line_number),
                      NewInteger(node->column_number),
                      children,
                      allocations);
  }

  v8::Local<v8::Value> TranslateAllocationProfile(Node* node) {
    v8::Local<v8::Array> children = NewArray(node->children.size());
    for (size_t i = 0; i < node->children.size(); i++) {
      Set(children, i, TranslateAllocationProfile(node->children[i].get()));
    }

    v8::Local<v8::Array> allocations = NewArray(node->allocations.size());
    for (size_t i = 0; i < node->allocations.size(); i++) {
      auto alloc = node->allocations[i];
      Set(allocations,
          i,
          CreateAllocation(NewNumber(alloc.size), NewNumber(alloc.count)));
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
  v8::Local<v8::Array> TranslateAllocationStats(
      const AllocationProfileSizeStatsMap* allocation_stats) {
    if (!allocation_stats || allocation_stats->empty()) {
      return v8::Array::New(isolate, 0);
    }

    std::vector<size_t> sizes;
    sizes.reserve(allocation_stats->size());
    for (const auto& allocation : *allocation_stats) {
      sizes.push_back(allocation.first);
    }
    std::sort(sizes.begin(), sizes.end());

    v8::Local<v8::Array> allocations = NewArray(sizes.size());
    for (size_t i = 0; i < sizes.size(); i++) {
      const auto& stats = allocation_stats->at(sizes[i]);
#if DD_V8_HAS_DICTIONARY_TEMPLATE
#define X(name, member) NewNumber(static_cast<double>(stats.member)),
      v8::MaybeLocal<v8::Value> values[] = {ALLOCATION_STATS_FIELDS};
#undef X
      Set(allocations,
          i,
          allocationStatsTemplate->NewInstance(Context(), values));
#else
      v8::Local<v8::Object> allocation = NewObject();
#define X(name, member)                                                        \
  Set(allocation,                                                              \
      NewString(#name),                                                        \
      NewNumber(static_cast<double>(stats.member)));
      ALLOCATION_STATS_FIELDS
#undef X
      Set(allocations, i, allocation);
#endif
    }

    return allocations;
  }
#undef ALLOCATION_STATS_FIELDS

  v8::Local<v8::Object> CreateNode(v8::Local<v8::String> name,
                                   v8::Local<v8::String> scriptName,
                                   v8::Local<v8::Integer> scriptId,
                                   v8::Local<v8::Integer> lineNumber,
                                   v8::Local<v8::Integer> columnNumber,
                                   v8::Local<v8::Array> children,
                                   v8::Local<v8::Array> allocations) {
#if DD_V8_HAS_DICTIONARY_TEMPLATE
#define X(name) name,
    v8::MaybeLocal<v8::Value> values[] = {NODE_FIELDS};
#undef X
#undef NODE_FIELDS
    return nodeTemplate->NewInstance(Context(), values);
#else
    v8::Local<v8::Object> js_node = NewObject();
#define X(name) Set(js_node, str_##name, name);
    NODE_FIELDS
#undef X
#undef NODE_FIELDS
    return js_node;
#endif
  }

  v8::Local<v8::Object> CreateAllocation(v8::Local<v8::Number> sizeBytes,
                                         v8::Local<v8::Number> count) {
#if DD_V8_HAS_DICTIONARY_TEMPLATE
#define X(name) name,
    v8::MaybeLocal<v8::Value> values[] = {ALLOCATION_FIELDS};
#undef X
#undef ALLOCATION_FIELDS
    return allocationTemplate->NewInstance(Context(), values);
#else
    v8::Local<v8::Object> js_alloc = NewObject();
#define X(name) Set(js_alloc, str_##name, name);
    ALLOCATION_FIELDS
#undef X
#undef ALLOCATION_FIELDS
    return js_alloc;
#endif
  }

 public:
  explicit HeapProfileTranslator() {}
};
}  // namespace

std::shared_ptr<Node> TranslateAllocationProfileToCpp(
    v8::AllocationProfile::Node* node) {
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
    new_node->children.push_back(TranslateAllocationProfileToCpp(child));
  }

  new_node->allocations.reserve(node->allocations.size());
  for (auto& allocation : node->allocations) {
    new_node->allocations.push_back(allocation);
  }
  return new_node;
}

v8::Local<v8::Value> TranslateAllocationProfile(
    v8::AllocationProfile::Node* node) {
  return HeapProfileTranslator().TranslateAllocationProfile(node);
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
