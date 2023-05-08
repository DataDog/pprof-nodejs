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

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include <nan.h>
#include <node.h>
#include <v8-profiler.h>

#include "../per-isolate-data.hh"
#include "wall.hh"

using namespace v8;

namespace dd {

using ProfilerMap = std::unordered_map<Isolate*, WallProfiler*>;

static std::atomic<ProfilerMap*> profilers(new ProfilerMap());

static std::unordered_map<const CpuProfileNode*, Local<Array>> labelSetsByNode;

static void (*old_handler)(int, siginfo_t*, void*) = nullptr;

static void sighandler(int sig, siginfo_t* info, void* context) {
  auto prof_map = profilers.load();
  auto prof_it = prof_map->find(Isolate::GetCurrent());
  if (prof_it != prof_map->end()) {
    prof_it->second->PushContext();
  }

  if (old_handler) {
    old_handler(sig, info, context);
  }
}

Local<Array> getLabelSetsForNode(const CpuProfileNode* node) {
  auto it = labelSetsByNode.find(node);
  if (it != labelSetsByNode.end()) {
    auto retval = it->second;
    labelSetsByNode.erase(it);
    return retval;
  } else {
    return Nan::New<Array>(0);
  }
}

Local<Object> CreateTimeNode(Local<String> name,
                             Local<String> scriptName,
                             Local<Integer> scriptId,
                             Local<Integer> lineNumber,
                             Local<Integer> columnNumber,
                             Local<Integer> hitCount,
                             Local<Array> children,
                             Local<Array> labelSets) {
  Local<Object> js_node = Nan::New<Object>();
  Nan::Set(js_node, Nan::New<String>("name").ToLocalChecked(), name);
  Nan::Set(
      js_node, Nan::New<String>("scriptName").ToLocalChecked(), scriptName);
  Nan::Set(js_node, Nan::New<String>("scriptId").ToLocalChecked(), scriptId);
  Nan::Set(
      js_node, Nan::New<String>("lineNumber").ToLocalChecked(), lineNumber);
  Nan::Set(
      js_node, Nan::New<String>("columnNumber").ToLocalChecked(), columnNumber);
  Nan::Set(js_node, Nan::New<String>("hitCount").ToLocalChecked(), hitCount);
  Nan::Set(js_node, Nan::New<String>("children").ToLocalChecked(), children);
  Nan::Set(js_node, Nan::New<String>("labelSets").ToLocalChecked(), labelSets);

  return js_node;
}

Local<Object> TranslateLineNumbersTimeProfileNode(const CpuProfileNode* parent,
                                                  const CpuProfileNode* node);

Local<Array> GetLineNumberTimeProfileChildren(const CpuProfileNode* node) {
  unsigned int index = 0;
  Local<Array> children;
  int32_t count = node->GetChildrenCount();

  unsigned int hitLineCount = node->GetHitLineCount();
  unsigned int hitCount = node->GetHitCount();
  auto labelSets = getLabelSetsForNode(node);
  if (hitLineCount > 0) {
    std::vector<CpuProfileNode::LineTick> entries(hitLineCount);
    node->GetLineTicks(&entries[0], hitLineCount);
    children = Nan::New<Array>(count + hitLineCount);
    for (const CpuProfileNode::LineTick entry : entries) {
      Nan::Set(children,
               index++,
               CreateTimeNode(node->GetFunctionName(),
                              node->GetScriptResourceName(),
                              Nan::New<Integer>(node->GetScriptId()),
                              Nan::New<Integer>(entry.line),
                              Nan::New<Integer>(0),
                              Nan::New<Integer>(entry.hit_count),
                              Nan::New<Array>(0),
                              labelSets));
    }
  } else if (hitCount > 0) {
    // Handle nodes for pseudo-functions like "process" and "garbage collection"
    // which do not have hit line counts.
    children = Nan::New<Array>(count + 1);
    Nan::Set(children,
             index++,
             CreateTimeNode(node->GetFunctionName(),
                            node->GetScriptResourceName(),
                            Nan::New<Integer>(node->GetScriptId()),
                            Nan::New<Integer>(node->GetLineNumber()),
                            Nan::New<Integer>(node->GetColumnNumber()),
                            Nan::New<Integer>(hitCount),
                            Nan::New<Array>(0),
                            labelSets));
  } else {
    children = Nan::New<Array>(count);
  }

  for (int32_t i = 0; i < count; i++) {
    Nan::Set(children,
             index++,
             TranslateLineNumbersTimeProfileNode(node, node->GetChild(i)));
  };

  return children;
}

Local<Object> TranslateLineNumbersTimeProfileNode(const CpuProfileNode* parent,
                                                  const CpuProfileNode* node) {
  return CreateTimeNode(parent->GetFunctionName(),
                        parent->GetScriptResourceName(),
                        Nan::New<Integer>(parent->GetScriptId()),
                        Nan::New<Integer>(node->GetLineNumber()),
                        Nan::New<Integer>(node->GetColumnNumber()),
                        Nan::New<Integer>(0),
                        GetLineNumberTimeProfileChildren(node),
                        getLabelSetsForNode(node));
}

// In profiles with line level accurate line numbers, a node's line number
// and column number refer to the line/column from which the function was
// called.
Local<Value> TranslateLineNumbersTimeProfileRoot(const CpuProfileNode* node) {
  int32_t count = node->GetChildrenCount();
  std::vector<Local<Array>> childrenArrs(count);
  int32_t childCount = 0;
  for (int32_t i = 0; i < count; i++) {
    Local<Array> c = GetLineNumberTimeProfileChildren(node->GetChild(i));
    childCount = childCount + c->Length();
    childrenArrs[i] = c;
  }

  Local<Array> children = Nan::New<Array>(childCount);
  int32_t idx = 0;
  for (int32_t i = 0; i < count; i++) {
    Local<Array> arr = childrenArrs[i];
    for (uint32_t j = 0; j < arr->Length(); j++) {
      Nan::Set(children, idx, Nan::Get(arr, j).ToLocalChecked());
      idx++;
    }
  }

  return CreateTimeNode(node->GetFunctionName(),
                        node->GetScriptResourceName(),
                        Nan::New<Integer>(node->GetScriptId()),
                        Nan::New<Integer>(node->GetLineNumber()),
                        Nan::New<Integer>(node->GetColumnNumber()),
                        Nan::New<Integer>(0),
                        children,
                        getLabelSetsForNode(node));
}

Local<Value> TranslateTimeProfileNode(const CpuProfileNode* node) {
  int32_t count = node->GetChildrenCount();
  Local<Array> children = Nan::New<Array>(count);
  for (int32_t i = 0; i < count; i++) {
    Nan::Set(children, i, TranslateTimeProfileNode(node->GetChild(i)));
  }

  return CreateTimeNode(node->GetFunctionName(),
                        node->GetScriptResourceName(),
                        Nan::New<Integer>(node->GetScriptId()),
                        Nan::New<Integer>(node->GetLineNumber()),
                        Nan::New<Integer>(node->GetColumnNumber()),
                        Nan::New<Integer>(node->GetHitCount()),
                        children,
                        getLabelSetsForNode(node));
}

Local<Value> TranslateTimeProfile(const CpuProfile* profile,
                                  bool includeLineInfo) {
  Local<Object> js_profile = Nan::New<Object>();
  Nan::Set(js_profile,
           Nan::New<String>("title").ToLocalChecked(),
           profile->GetTitle());

#if NODE_MODULE_VERSION > NODE_11_0_MODULE_VERSION
  if (includeLineInfo) {
    Nan::Set(js_profile,
             Nan::New<String>("topDownRoot").ToLocalChecked(),
             TranslateLineNumbersTimeProfileRoot(profile->GetTopDownRoot()));
  } else {
    Nan::Set(js_profile,
             Nan::New<String>("topDownRoot").ToLocalChecked(),
             TranslateTimeProfileNode(profile->GetTopDownRoot()));
  }
#else
  Nan::Set(js_profile,
           Nan::New<String>("topDownRoot").ToLocalChecked(),
           TranslateTimeProfileNode(profile->GetTopDownRoot()));
#endif
  Nan::Set(js_profile,
           Nan::New<String>("startTime").ToLocalChecked(),
           Nan::New<Number>(profile->GetStartTime()));
  Nan::Set(js_profile,
           Nan::New<String>("endTime").ToLocalChecked(),
           Nan::New<Number>(profile->GetEndTime()));

  return js_profile;
}

bool isIdleSample(const CpuProfileNode* sample) {
  return
#if NODE_MODULE_VERSION > NODE_12_0_MODULE_VERSION
      sample->GetParent() == nullptr &&
#endif
      sample->GetChildrenCount() == 0 &&
      strncmp("(idle)", sample->GetFunctionNameStr(), 7) == 0;
}

void WallProfiler::AddLabelSetsByNode(CpuProfile* profile) {
  auto halfInterval = samplingInterval * 1000 / 2;

  if (contexts.empty()) {
    return;
  }
  SampleContext sampleContext = contexts.pop_front();

  uint64_t time_diff = last_start - profile->GetStartTime() * 1000;

  for (int i = 0; i < profile->GetSamplesCount(); i++) {
    auto sample = profile->GetSample(i);
    if (isIdleSample(sample)) {
      continue;
    }

    // Translate from profiler micros to uv_hrtime nanos
    auto sampleTimestamp = profile->GetSampleTimestamp(i) * 1000 + time_diff;

    // This loop will drop all contexts that are too old to be associated with
    // the current sample; associate those (normally one) that are close enough
    // to it in time, and stop as soon as it sees a context that's too recent
    // for this sample.
    for (;;) {
      auto contextTimestamp = sampleContext.timestamp;
      if (contextTimestamp < sampleTimestamp - halfInterval) {
        // Current sample context is too old, discard it and fetch the next one
        if (contexts.empty()) {
          return;
        }
        sampleContext = contexts.pop_front();
      } else if (contextTimestamp >= sampleTimestamp + halfInterval) {
        // Current sample context is too recent, we'll try to match it to the
        // next sample
        break;
      } else {
        // This context timestamp is in the goldilocks zone around the sample
        // timestamp, so associate its labels with the sample.
        auto it = labelSetsByNode.find(sample);
        Local<Array> array;
        if (it == labelSetsByNode.end()) {
          array = Nan::New<Array>();
          labelSetsByNode.emplace(sample, array);
        } else {
          array = it->second;
        }
        Nan::Set(array, array->Length(), sampleContext.labels.get()->handle());
        // Sample context was consumed, fetch the next one
        if (contexts.empty()) {
          return;
        }
        sampleContext = contexts.pop_front();
      }
    }
  }
  // Push the last popped sample context back into the ring to be used by the
  // next profile
  contexts.push_front(std::move(sampleContext));
}

static Nan::Persistent<v8::Function> constructor;

WallProfiler::WallProfiler(int intervalMicros, int durationMicros)
    : samplingInterval(intervalMicros),
      contexts(durationMicros * 2 / intervalMicros) {}

WallProfiler::~WallProfiler() {
  Dispose(nullptr);
}

template <typename F>
void updateProfilers(F updateFn) {
  auto currProfilers = profilers.load();
  for (;;) {
    auto newProfilers = new ProfilerMap(*currProfilers);
    updateFn(newProfilers);
    if (profilers.compare_exchange_weak(currProfilers, newProfilers)) {
      delete currProfilers;
      break;
    } else {
      delete newProfilers;
    }
  }
}

void WallProfiler::Dispose(Isolate* isolate) {
  if (cpuProfiler != nullptr) {
    cpuProfiler->Dispose();
    cpuProfiler = nullptr;

    updateProfilers([isolate, this](auto map) {
      if (isolate != nullptr) {
        auto it = map->find(isolate);
        if (it != map->end() && it->second == this) {
          map->erase(it);
        }
      } else {
        // TODO: use map->erase_if once we can use C++20.
        for (auto it = map->begin(), last = map->end(); it != last;) {
          if (it->second == this) {
            it = map->erase(it);
          } else {
            ++it;
          }
        }
      }
    });
  }
}

NAN_METHOD(WallProfiler::Dispose) {
  WallProfiler* wallProfiler =
      Nan::ObjectWrap::Unwrap<WallProfiler>(info.Holder());

  wallProfiler->Dispose(info.GetIsolate());
}

NAN_METHOD(WallProfiler::New) {
  if (info.Length() != 2) {
    return Nan::ThrowTypeError("WallProfiler must have two arguments.");
  }
  if (!info[0]->IsNumber()) {
    return Nan::ThrowTypeError("Sample rate must be a number.");
  }

  if (!info[0]->IsNumber()) {
    return Nan::ThrowTypeError("Duration must be a number.");
  }

  if (info.IsConstructCall()) {
    int interval = Nan::MaybeLocal<Integer>(info[0].As<Integer>())
                       .ToLocalChecked()
                       ->Value();
    int duration = Nan::MaybeLocal<Integer>(info[1].As<Integer>())
                       .ToLocalChecked()
                       ->Value();

    if (interval <= 0) {
      return Nan::ThrowTypeError("Sample rate must be positive.");
    }
    if (duration <= 0) {
      return Nan::ThrowTypeError("Duration must be positive.");
    }
    if (duration < interval) {
      return Nan::ThrowTypeError("Duration must not be less than sample rate.");
    }

    WallProfiler* obj = new WallProfiler(interval, duration);
    obj->Wrap(info.This());
    info.GetReturnValue().Set(info.This());
  } else {
    const int argc = 1;
    v8::Local<v8::Value> argv[argc] = {info[0]};
    v8::Local<v8::Function> cons = Nan::New(
        PerIsolateData::For(info.GetIsolate())->WallProfilerConstructor());
    info.GetReturnValue().Set(
        Nan::NewInstance(cons, argc, argv).ToLocalChecked());
  }
}

NAN_METHOD(WallProfiler::Start) {
  WallProfiler* wallProfiler =
      Nan::ObjectWrap::Unwrap<WallProfiler>(info.Holder());

  if (info.Length() != 2) {
    return Nan::ThrowTypeError("Start must have two arguments.");
  }
  if (!info[0]->IsString()) {
    return Nan::ThrowTypeError("Profile name must be a string.");
  }
  if (!info[1]->IsBoolean()) {
    return Nan::ThrowTypeError("Include lines must be a boolean.");
  }

  Local<String> name =
      Nan::MaybeLocal<String>(info[0].As<String>()).ToLocalChecked();

  bool includeLines =
      Nan::MaybeLocal<Boolean>(info[1].As<Boolean>()).ToLocalChecked()->Value();

  wallProfiler->StartImpl(name, includeLines);

  struct sigaction sa, old_sa;
  sa.sa_flags = SA_SIGINFO | SA_RESTART;
  sa.sa_sigaction = &sighandler;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGPROF, &sa, &old_sa);

  // At the end of a cycle start is called before stop,
  // at this point old_sa.sa_sigaction is sighandler !
  if (!old_handler) {
    old_handler = old_sa.sa_sigaction;
  }
}

void WallProfiler::StartImpl(Local<String> name, bool includeLines) {
  if (includeLines) {
    GetProfiler()->StartProfiling(
        name, CpuProfilingMode::kCallerLineNumbers, true);
  } else {
    GetProfiler()->StartProfiling(name, true);
  }

  last_start = current_start;
  current_start = uv_hrtime();
}

NAN_METHOD(WallProfiler::Stop) {
  if (info.Length() != 2) {
    return Nan::ThrowTypeError("Start must have two arguments.");
  }
  if (!info[0]->IsString()) {
    return Nan::ThrowTypeError("Profile name must be a string.");
  }
  if (!info[1]->IsBoolean()) {
    return Nan::ThrowTypeError("Include lines must be a boolean.");
  }

  Local<String> name =
      Nan::MaybeLocal<String>(info[0].As<String>()).ToLocalChecked();

  bool includeLines =
      Nan::MaybeLocal<Boolean>(info[1].As<Boolean>()).ToLocalChecked()->Value();

  WallProfiler* wallProfiler =
      Nan::ObjectWrap::Unwrap<WallProfiler>(info.Holder());

  auto profiler = wallProfiler->GetProfiler();
  auto v8_profile = profiler->StopProfiling(name);
  wallProfiler->AddLabelSetsByNode(v8_profile);
  Local<Value> profile = TranslateTimeProfile(v8_profile, includeLines);
  v8_profile->Delete();

  info.GetReturnValue().Set(profile);
}

NAN_MODULE_INIT(WallProfiler::Init) {
  Local<FunctionTemplate> tpl = Nan::New<FunctionTemplate>(New);
  Local<String> className = Nan::New("TimeProfiler").ToLocalChecked();
  tpl->SetClassName(className);
  tpl->InstanceTemplate()->SetInternalFieldCount(1);

  Nan::SetAccessor(tpl->InstanceTemplate(),
                   Nan::New("labels").ToLocalChecked(),
                   GetLabels,
                   SetLabels);

  Nan::SetPrototypeMethod(tpl, "start", Start);
  Nan::SetPrototypeMethod(tpl, "dispose", Dispose);
  Nan::SetPrototypeMethod(tpl, "stop", Stop);
  Nan::SetPrototypeMethod(tpl, "unsetLabels", UnsetLabels);

  PerIsolateData::For(Isolate::GetCurrent())
      ->WallProfilerConstructor()
      .Reset(Nan::GetFunction(tpl).ToLocalChecked());
  Nan::Set(target, className, Nan::GetFunction(tpl).ToLocalChecked());
}

// A new CPU profiler object will be created each time profiling is started
// to work around https://bugs.chromium.org/p/v8/issues/detail?id=11051.
// TODO: Fixed in v16. Delete this hack when deprecating v14.
v8::CpuProfiler* WallProfiler::GetProfiler() {
  if (cpuProfiler == nullptr) {
    v8::Isolate* isolate = v8::Isolate::GetCurrent();

    updateProfilers([isolate, this](auto map) { map->emplace(isolate, this); });

    cpuProfiler = v8::CpuProfiler::New(isolate);
    cpuProfiler->SetSamplingInterval(samplingInterval);
  }
  return cpuProfiler;
}

v8::Local<v8::Value> WallProfiler::GetLabels() {
  if (!labels_) return Nan::Undefined();
  return labels_->handle();
}

void WallProfiler::SetLabels(v8::Local<v8::Value> value) {
  labels_ = std::make_shared<LabelWrap>(value);
}

void WallProfiler::UnsetLabels() {
  labels_.reset();
}

NAN_GETTER(WallProfiler::GetLabels) {
  auto profiler = Nan::ObjectWrap::Unwrap<WallProfiler>(info.Holder());
  info.GetReturnValue().Set(profiler->GetLabels());
}

NAN_SETTER(WallProfiler::SetLabels) {
  auto profiler = Nan::ObjectWrap::Unwrap<WallProfiler>(info.Holder());
  profiler->SetLabels(value);
}

NAN_METHOD(WallProfiler::UnsetLabels) {
  auto profiler = Nan::ObjectWrap::Unwrap<WallProfiler>(info.Holder());
  profiler->UnsetLabels();
}

void WallProfiler::PushContext() {
  // Be careful this is called in a signal handler context therefore all
  // operations must be async signal safe (in particular no allocations). Our
  // ring buffer avoids allocations.
  if (labels_) {
    contexts.push_back(SampleContext(labels_, uv_hrtime()));
  }
}

}  // namespace dd
