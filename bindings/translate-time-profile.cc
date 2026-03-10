/**
 * Copyright 2018 Google Inc. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "translate-time-profile.hh"
#include <v8-version.h>
#include "per-isolate-data.hh"
#include "profile-translator.hh"

namespace dd {

namespace {

TimeProfileNodeInfo* AllocNode(TimeProfileViewState* state,
                               const v8::CpuProfileNode* node,
                               const v8::CpuProfileNode* metadata_node,
                               int line_number,
                               int column_number,
                               int hit_count,
                               bool is_line_root = false) {
  auto info = std::make_unique<TimeProfileNodeInfo>();
  info->node = node;
  info->metadata_node = metadata_node;
  info->line_number = line_number;
  info->column_number = column_number;
  info->hit_count = hit_count;
  info->is_line_root = is_line_root;
  info->state = state;
  auto* raw = info.get();
  state->owned_nodes.push_back(std::move(info));
  return raw;
}

// Line-info mode: for a given V8 node, append its line ticks (leaves with
// hit_count > 0) and child calls (intermediate nodes with hit_count 0).
void AppendLineChildren(TimeProfileViewState* state,
                        const v8::CpuProfileNode* node,
                        std::vector<TimeProfileNodeInfo*>& out) {
  unsigned int hitLineCount = node->GetHitLineCount();
  unsigned int hitCount = node->GetHitCount();

  if (hitLineCount > 0) {
    std::vector<v8::CpuProfileNode::LineTick> entries(hitLineCount);
    node->GetLineTicks(&entries[0], hitLineCount);
    for (const auto& entry : entries) {
      int column = 0;
#if V8_MAJOR_VERSION >= 14
      column = entry.column;
#endif
      out.push_back(
          AllocNode(state, node, node, entry.line, column, entry.hit_count));
    }
  } else if (hitCount > 0) {
    out.push_back(AllocNode(state,
                            node,
                            node,
                            node->GetLineNumber(),
                            node->GetColumnNumber(),
                            hitCount));
  }

  int32_t count = node->GetChildrenCount();
  for (int32_t i = 0; i < count; i++) {
    auto* child = node->GetChild(i);
    out.push_back(AllocNode(state,
                            child,
                            node,
                            child->GetLineNumber(),
                            child->GetColumnNumber(),
                            0));
  }
}

int ResolveHitCount(const v8::CpuProfileNode* node, ContextsByNode* cbn) {
  if (!cbn) return node->GetHitCount();
  auto it = cbn->find(node);
  return it != cbn->end() ? it->second.hitcount : 0;
}

int ComputeTotalHitCount(const v8::CpuProfileNode* node, ContextsByNode* cbn) {
  int total = ResolveHitCount(node, cbn);
  int32_t count = node->GetChildrenCount();
  for (int32_t i = 0; i < count; i++) {
    total += ComputeTotalHitCount(node->GetChild(i), cbn);
  }
  return total;
}

// WrapNode: create a JS wrapper for a profile node.
// Normal mode stores CpuProfileNode* directly.
// line-info mode stores owned info.
v8::Local<v8::Object> WrapNode(const v8::CpuProfileNode* node,
                               TimeProfileViewState* state) {
  auto* isolate = v8::Isolate::GetCurrent();
  auto ctor =
      Nan::New(PerIsolateData::For(isolate)->TimeProfileNodeConstructor());
  auto obj = Nan::NewInstance(ctor).ToLocalChecked();
  Nan::SetInternalFieldPointer(obj, 0, const_cast<v8::CpuProfileNode*>(node));
  Nan::SetInternalFieldPointer(obj, 1, state);
  return obj;
}

v8::Local<v8::Object> WrapNode(TimeProfileNodeInfo* info) {
  auto* isolate = v8::Isolate::GetCurrent();
  auto ctor =
      Nan::New(PerIsolateData::For(isolate)->TimeProfileNodeConstructor());
  auto obj = Nan::NewInstance(ctor).ToLocalChecked();
  Nan::SetInternalFieldPointer(obj, 0, info);
  Nan::SetInternalFieldPointer(obj, 1, info->state);
  return obj;
}

// Extracts the two internal fields from a JS wrapper object.
// field 0 represents the node data (depends on mode, see below).
// field 1 TimeProfileViewState* (shared state, always present).
//
// Line-info: field 0 is a TimeProfileNodeInfo* holding synthetic
// line/column/hitCount values. Normal: field 0 is a CpuProfileNode* pointing
// directly to V8.
struct NodeFields {
  void* ptr;
  TimeProfileViewState* state;

  bool is_line_info() const { return state->include_line_info; }

  TimeProfileNodeInfo* as_info() const {
    return static_cast<TimeProfileNodeInfo*>(ptr);
  }

  const v8::CpuProfileNode* as_node() const {
    return static_cast<const v8::CpuProfileNode*>(ptr);
  }
};

NodeFields GetFields(const Nan::PropertyCallbackInfo<v8::Value>& info) {
  return {Nan::GetInternalFieldPointer(info.Holder(), 0),
          static_cast<TimeProfileViewState*>(
              Nan::GetInternalFieldPointer(info.Holder(), 1))};
}

NAN_GETTER(GetName) {
  auto fields = GetFields(info);
  if (fields.is_line_info()) {
    info.GetReturnValue().Set(
        fields.as_info()->metadata_node->GetFunctionName());
  } else {
    info.GetReturnValue().Set(fields.as_node()->GetFunctionName());
  }
}

NAN_GETTER(GetScriptName) {
  auto fields = GetFields(info);
  if (fields.is_line_info()) {
    info.GetReturnValue().Set(
        fields.as_info()->metadata_node->GetScriptResourceName());
  } else {
    info.GetReturnValue().Set(fields.as_node()->GetScriptResourceName());
  }
}

NAN_GETTER(GetScriptId) {
  auto fields = GetFields(info);
  if (fields.is_line_info()) {
    info.GetReturnValue().Set(
        Nan::New<v8::Integer>(fields.as_info()->metadata_node->GetScriptId()));
  } else {
    info.GetReturnValue().Set(
        Nan::New<v8::Integer>(fields.as_node()->GetScriptId()));
  }
}

NAN_GETTER(GetLineNumber) {
  auto fields = GetFields(info);
  if (fields.is_line_info()) {
    info.GetReturnValue().Set(
        Nan::New<v8::Integer>(fields.as_info()->line_number));
  } else {
    info.GetReturnValue().Set(
        Nan::New<v8::Integer>(fields.as_node()->GetLineNumber()));
  }
}

NAN_GETTER(GetColumnNumber) {
  auto fields = GetFields(info);
  if (fields.is_line_info()) {
    info.GetReturnValue().Set(
        Nan::New<v8::Integer>(fields.as_info()->column_number));
  } else {
    info.GetReturnValue().Set(
        Nan::New<v8::Integer>(fields.as_node()->GetColumnNumber()));
  }
}

NAN_GETTER(GetHitCount) {
  auto fields = GetFields(info);
  if (fields.is_line_info()) {
    info.GetReturnValue().Set(
        Nan::New<v8::Integer>(fields.as_info()->hit_count));
  } else {
    info.GetReturnValue().Set(Nan::New<v8::Integer>(
        ResolveHitCount(fields.as_node(), fields.state->contexts_by_node)));
  }
}

NAN_GETTER(GetContexts) {
  auto fields = GetFields(info);
  auto* isolate = v8::Isolate::GetCurrent();
  // Line-info nodes and nodes without context tracking return empty contexts.
  if (fields.is_line_info() || !fields.state->contexts_by_node) {
    info.GetReturnValue().Set(v8::Array::New(isolate, 0));
    return;
  }
  auto it = fields.state->contexts_by_node->find(fields.as_node());
  if (it != fields.state->contexts_by_node->end()) {
    info.GetReturnValue().Set(it->second.contexts);
  } else {
    info.GetReturnValue().Set(v8::Array::New(isolate, 0));
  }
}

NAN_GETTER(GetChildren) {
  auto fields = GetFields(info);
  auto* isolate = info.GetIsolate();
  auto ctx = isolate->GetCurrentContext();

  if (fields.is_line_info()) {
    std::vector<TimeProfileNodeInfo*> children;

    // Root in line-info mode is flattened from each direct child to preserve
    // eager v1 top-level shape.
    if (fields.as_info()->is_line_root) {
      int32_t count = fields.as_info()->node->GetChildrenCount();
      for (int32_t i = 0; i < count; i++) {
        AppendLineChildren(
            fields.state, fields.as_info()->node->GetChild(i), children);
      }
      auto arr = v8::Array::New(isolate, children.size());
      for (size_t i = 0; i < children.size(); i++) {
        arr->Set(ctx, i, WrapNode(children[i])).Check();
      }
      info.GetReturnValue().Set(arr);
      return;
    }

    // In line-info mode, leaf nodes (hitCount > 0) have no children.
    if (fields.as_info()->hit_count > 0) {
      info.GetReturnValue().Set(v8::Array::New(isolate, 0));
      return;
    }

    AppendLineChildren(fields.state, fields.as_info()->node, children);
    auto arr = v8::Array::New(isolate, children.size());
    for (size_t i = 0; i < children.size(); i++) {
      arr->Set(ctx, i, WrapNode(children[i])).Check();
    }
    info.GetReturnValue().Set(arr);
  } else {
    int32_t count = fields.as_node()->GetChildrenCount();
    auto arr = v8::Array::New(isolate, count);
    for (int32_t i = 0; i < count; i++) {
      arr->Set(ctx, i, WrapNode(fields.as_node()->GetChild(i), fields.state))
          .Check();
    }
    info.GetReturnValue().Set(arr);
  }
}

class TimeProfileTranslator : ProfileTranslator {
 private:
  ContextsByNode* contextsByNode;
  v8::Local<v8::Array> emptyArray = NewArray(0);
  v8::Local<v8::Integer> zero = NewInteger(0);

#define FIELDS                                                                 \
  X(name)                                                                      \
  X(scriptName)                                                                \
  X(scriptId)                                                                  \
  X(lineNumber)                                                                \
  X(columnNumber)                                                              \
  X(hitCount)                                                                  \
  X(children)                                                                  \
  X(contexts)

#define X(name) v8::Local<v8::String> str_##name = NewString(#name);
  FIELDS
#undef X

  v8::Local<v8::Array> getContextsForNode(const v8::CpuProfileNode* node,
                                          uint32_t& hitcount) {
    hitcount = node->GetHitCount();
    if (!contextsByNode) {
      // custom contexts are not enabled, keep the node hitcount and return
      // empty array
      return emptyArray;
    }

    auto it = contextsByNode->find(node);
    auto contexts = emptyArray;
    if (it != contextsByNode->end()) {
      hitcount = it->second.hitcount;
      contexts = it->second.contexts;
    } else {
      // no context found for node, discard it since every sample taken from
      // signal handler should have a matching context if it does not, it means
      // sample was captured by a deopt event
      hitcount = 0;
    }
    return contexts;
  }

  v8::Local<v8::Object> CreateTimeNode(v8::Local<v8::String> name,
                                       v8::Local<v8::String> scriptName,
                                       v8::Local<v8::Integer> scriptId,
                                       v8::Local<v8::Integer> lineNumber,
                                       v8::Local<v8::Integer> columnNumber,
                                       v8::Local<v8::Integer> hitCount,
                                       v8::Local<v8::Array> children,
                                       v8::Local<v8::Array> contexts) {
    v8::Local<v8::Object> js_node = NewObject();
#define X(name) Set(js_node, str_##name, name);
    FIELDS
#undef X
#undef FIELDS
    return js_node;
  }

  v8::Local<v8::Array> GetLineNumberTimeProfileChildren(
      const v8::CpuProfileNode* node) {
    unsigned int index = 0;
    v8::Local<v8::Array> children;
    int32_t count = node->GetChildrenCount();

    unsigned int hitLineCount = node->GetHitLineCount();
    unsigned int hitCount = node->GetHitCount();
    auto scriptId = NewInteger(node->GetScriptId());
    if (hitLineCount > 0) {
      std::vector<v8::CpuProfileNode::LineTick> entries(hitLineCount);
      node->GetLineTicks(&entries[0], hitLineCount);
      children = NewArray(count + hitLineCount);
      for (const v8::CpuProfileNode::LineTick entry : entries) {
        Set(children,
            index++,
            CreateTimeNode(node->GetFunctionName(),
                           node->GetScriptResourceName(),
                           scriptId,
                           NewInteger(entry.line),
// V8 14+ (Node.js 25+) added column field to LineTick struct
#if V8_MAJOR_VERSION >= 14
                           NewInteger(entry.column),
#else
                           zero,
#endif
                           NewInteger(entry.hit_count),
                           emptyArray,
                           emptyArray));
      }
    } else if (hitCount > 0) {
      // Handle nodes for pseudo-functions like "process" and "garbage
      // collection" which do not have hit line counts.
      children = NewArray(count + 1);
      Set(children,
          index++,
          CreateTimeNode(node->GetFunctionName(),
                         node->GetScriptResourceName(),
                         scriptId,
                         NewInteger(node->GetLineNumber()),
                         NewInteger(node->GetColumnNumber()),
                         NewInteger(hitCount),
                         emptyArray,
                         emptyArray));
    } else {
      children = NewArray(count);
    }

    for (int32_t i = 0; i < count; i++) {
      Set(children,
          index++,
          TranslateLineNumbersTimeProfileNode(node, node->GetChild(i)));
    };

    return children;
  }

  v8::Local<v8::Object> TranslateLineNumbersTimeProfileNode(
      const v8::CpuProfileNode* parent, const v8::CpuProfileNode* node) {
    return CreateTimeNode(parent->GetFunctionName(),
                          parent->GetScriptResourceName(),
                          NewInteger(parent->GetScriptId()),
                          NewInteger(node->GetLineNumber()),
                          NewInteger(node->GetColumnNumber()),
                          zero,
                          GetLineNumberTimeProfileChildren(node),
                          emptyArray);
  }

  // In profiles with line level accurate line numbers, a node's line number
  // and column number refer to the line/column from which the function was
  // called.
  v8::Local<v8::Value> TranslateLineNumbersTimeProfileRoot(
      const v8::CpuProfileNode* node) {
    int32_t count = node->GetChildrenCount();
    std::vector<v8::Local<v8::Array>> childrenArrs(count);
    int32_t childCount = 0;
    for (int32_t i = 0; i < count; i++) {
      v8::Local<v8::Array> c =
          GetLineNumberTimeProfileChildren(node->GetChild(i));
      childCount = childCount + c->Length();
      childrenArrs[i] = c;
    }

    v8::Local<v8::Array> children = NewArray(childCount);
    int32_t idx = 0;
    for (int32_t i = 0; i < count; i++) {
      v8::Local<v8::Array> arr = childrenArrs[i];
      for (uint32_t j = 0; j < arr->Length(); j++) {
        Set(children, idx, Get(arr, j).ToLocalChecked());
        idx++;
      }
    }

    return CreateTimeNode(node->GetFunctionName(),
                          node->GetScriptResourceName(),
                          NewInteger(node->GetScriptId()),
                          NewInteger(node->GetLineNumber()),
                          NewInteger(node->GetColumnNumber()),
                          zero,
                          children,
                          emptyArray);
  }

  v8::Local<v8::Value> TranslateTimeProfileNode(
      const v8::CpuProfileNode* node) {
    int32_t count = node->GetChildrenCount();
    v8::Local<v8::Array> children = NewArray(count);
    for (int32_t i = 0; i < count; i++) {
      Set(children, i, TranslateTimeProfileNode(node->GetChild(i)));
    }

    uint32_t hitcount = 0;
    auto contexts = getContextsForNode(node, hitcount);

    return CreateTimeNode(node->GetFunctionName(),
                          node->GetScriptResourceName(),
                          NewInteger(node->GetScriptId()),
                          NewInteger(node->GetLineNumber()),
                          NewInteger(node->GetColumnNumber()),
                          NewInteger(hitcount),
                          children,
                          contexts);
  }

 public:
  explicit TimeProfileTranslator(ContextsByNode* nls = nullptr)
      : contextsByNode(nls) {}

  v8::Local<v8::Value> TranslateTimeProfile(const v8::CpuProfile* profile,
                                            bool includeLineInfo,
                                            bool hasCpuTime,
                                            int64_t nonJSThreadsCpuTime) {
    v8::Local<v8::Object> js_profile = NewObject();

    if (includeLineInfo) {
      Set(js_profile,
          NewString("topDownRoot"),
          TranslateLineNumbersTimeProfileRoot(profile->GetTopDownRoot()));
    } else {
      Set(js_profile,
          NewString("topDownRoot"),
          TranslateTimeProfileNode(profile->GetTopDownRoot()));
    }
    Set(js_profile, NewString("startTime"), NewNumber(profile->GetStartTime()));
    Set(js_profile, NewString("endTime"), NewNumber(profile->GetEndTime()));
    Set(js_profile, NewString("hasCpuTime"), NewBoolean(hasCpuTime));

    Set(js_profile,
        NewString("nonJSThreadsCpuTime"),
        NewNumber(nonJSThreadsCpuTime));
    return js_profile;
  }
};
}  // namespace

NAN_MODULE_INIT(TimeProfileNodeView::Init) {
  v8::Local<v8::FunctionTemplate> tpl = Nan::New<v8::FunctionTemplate>();
  tpl->SetClassName(Nan::New("TimeProfileNode").ToLocalChecked());
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
  Nan::SetAccessor(inst, Nan::New("hitCount").ToLocalChecked(), GetHitCount);
  Nan::SetAccessor(inst, Nan::New("children").ToLocalChecked(), GetChildren);
  Nan::SetAccessor(inst, Nan::New("contexts").ToLocalChecked(), GetContexts);

  PerIsolateData::For(v8::Isolate::GetCurrent())
      ->TimeProfileNodeConstructor()
      .Reset(Nan::GetFunction(tpl).ToLocalChecked());
}

// Builds a lazy JS profile view.
// For non-line-info, wrappers store raw CpuProfileNode pointers.
// For line-info, owned TimeProfileNodeInfo structs are used for synthetic
// nodes.
v8::Local<v8::Value> BuildTimeProfileView(const v8::CpuProfile* profile,
                                          bool has_cpu_time,
                                          int64_t non_js_threads_cpu_time,
                                          TimeProfileViewState& state) {
  auto* isolate = v8::Isolate::GetCurrent();
  v8::Local<v8::Object> js_profile = v8::Object::New(isolate);

  auto* root_node = profile->GetTopDownRoot();

  if (state.include_line_info) {
    auto* root_info = AllocNode(&state,
                                root_node,
                                root_node,
                                root_node->GetLineNumber(),
                                root_node->GetColumnNumber(),
                                0,
                                true);
    auto root = WrapNode(root_info);

    Nan::Set(js_profile, Nan::New("topDownRoot").ToLocalChecked(), root);
  } else {
    Nan::Set(js_profile,
             Nan::New("topDownRoot").ToLocalChecked(),
             WrapNode(root_node, &state));
  }
  Nan::Set(js_profile,
           Nan::New("startTime").ToLocalChecked(),
           Nan::New<v8::Number>(profile->GetStartTime()));
  Nan::Set(js_profile,
           Nan::New("endTime").ToLocalChecked(),
           Nan::New<v8::Number>(profile->GetEndTime()));
  Nan::Set(js_profile,
           Nan::New("hasCpuTime").ToLocalChecked(),
           Nan::New(has_cpu_time));
  Nan::Set(js_profile,
           Nan::New("nonJSThreadsCpuTime").ToLocalChecked(),
           Nan::New<v8::Number>(non_js_threads_cpu_time));
  Nan::Set(js_profile,
           Nan::New("totalHitCount").ToLocalChecked(),
           Nan::New<v8::Integer>(
               ComputeTotalHitCount(root_node, state.contexts_by_node)));
  return js_profile;
}

v8::Local<v8::Value> TranslateTimeProfile(const v8::CpuProfile* profile,
                                          bool includeLineInfo,
                                          ContextsByNode* contextsByNode,
                                          bool hasCpuTime,
                                          int64_t nonJSThreadsCpuTime) {
  return TimeProfileTranslator(contextsByNode)
      .TranslateTimeProfile(
          profile, includeLineInfo, hasCpuTime, nonJSThreadsCpuTime);
}

}  // namespace dd
