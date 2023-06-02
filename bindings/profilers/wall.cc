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

#ifndef _WIN32
#define DD_WALL_USE_SIGPROF
#else
#undef DD_WALL_USE_SIGPROF
#endif

using namespace v8;

namespace dd {

using ProfilerMap = std::unordered_map<Isolate*, WallProfiler*>;

static std::atomic<ProfilerMap*> profilers(new ProfilerMap());

#ifdef DD_WALL_USE_SIGPROF
static void (*old_handler)(int, siginfo_t*, void*) = nullptr;

static void sighandler(int sig, siginfo_t* info, void* context) {
  auto prof_map = profilers.load();
  auto prof_it = prof_map->find(Isolate::GetCurrent());
  if (old_handler) {
    old_handler(sig, info, context);
  }
  if (prof_it != prof_map->end()) {
    prof_it->second->PushContext();
  }
}
#endif

class ProfileTranslator {
  friend class WallProfiler;

  LabelSetsByNode labelSetsByNode;
  Isolate* isolate = Isolate::GetCurrent();
  Local<Array> emptyArray = NewArray(0);
  Local<Integer> zero = NewInteger(0);

#define FIELDS                                                                 \
  X(name)                                                                      \
  X(scriptName)                                                                \
  X(scriptId)                                                                  \
  X(lineNumber)                                                                \
  X(columnNumber)                                                              \
  X(hitCount)                                                                  \
  X(children)                                                                  \
  X(labelSets)

#define X(name) Local<String> str_##name = NewString(#name);
  FIELDS
#undef X

  ProfileTranslator(
      std::unordered_map<const CpuProfileNode*, Local<Array>>&& nls)
      : labelSetsByNode(std::move(nls)) {}

  Local<Array> getLabelSetsForNode(const CpuProfileNode* node) {
    auto it = labelSetsByNode.find(node);
    if (it != labelSetsByNode.end()) {
      auto retval = it->second;
      labelSetsByNode.erase(it);
      return retval;
    } else {
      return emptyArray;
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
#define X(name) Nan::Set(js_node, str_##name, name);
    FIELDS
#undef X
    return js_node;
  }

  Local<Integer> NewInteger(int32_t x) {
    return Integer::New(isolate, x);
  }

  Local<Array> NewArray(int length) {
    return Array::New(isolate, length);
  }

  Local<String> NewString(const char* str) {
#if NODE_MODULE_VERSION > NODE_12_0_MODULE_VERSION
    return String::NewFromUtf8(isolate, str).ToLocalChecked();
#else
    return Nan::New<String>(str).ToLocalChecked();
#endif
  }

  Local<Array> GetLineNumberTimeProfileChildren(const CpuProfileNode* node) {
    unsigned int index = 0;
    Local<Array> children;
    int32_t count = node->GetChildrenCount();

    unsigned int hitLineCount = node->GetHitLineCount();
    unsigned int hitCount = node->GetHitCount();
    auto labelSets = getLabelSetsForNode(node);
    auto scriptId = NewInteger(node->GetScriptId());
    if (hitLineCount > 0) {
      std::vector<CpuProfileNode::LineTick> entries(hitLineCount);
      node->GetLineTicks(&entries[0], hitLineCount);
      children = NewArray(count + hitLineCount);
      for (const CpuProfileNode::LineTick entry : entries) {
        Nan::Set(children,
                 index++,
                 CreateTimeNode(node->GetFunctionName(),
                                node->GetScriptResourceName(),
                                scriptId,
                                NewInteger(entry.line),
                                zero,
                                NewInteger(entry.hit_count),
                                emptyArray,
                                labelSets));
      }
    } else if (hitCount > 0) {
      // Handle nodes for pseudo-functions like "process" and "garbage
      // collection" which do not have hit line counts.
      children = NewArray(count + 1);
      Nan::Set(children,
               index++,
               CreateTimeNode(node->GetFunctionName(),
                              node->GetScriptResourceName(),
                              scriptId,
                              NewInteger(node->GetLineNumber()),
                              NewInteger(node->GetColumnNumber()),
                              NewInteger(hitCount),
                              emptyArray,
                              labelSets));
    } else {
      children = NewArray(count);
    }

    for (int32_t i = 0; i < count; i++) {
      Nan::Set(children,
               index++,
               TranslateLineNumbersTimeProfileNode(node, node->GetChild(i)));
    };

    return children;
  }

  Local<Object> TranslateLineNumbersTimeProfileNode(
      const CpuProfileNode* parent, const CpuProfileNode* node) {
    return CreateTimeNode(parent->GetFunctionName(),
                          parent->GetScriptResourceName(),
                          NewInteger(parent->GetScriptId()),
                          NewInteger(node->GetLineNumber()),
                          NewInteger(node->GetColumnNumber()),
                          zero,
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

    Local<Array> children = NewArray(childCount);
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
                          NewInteger(node->GetScriptId()),
                          NewInteger(node->GetLineNumber()),
                          NewInteger(node->GetColumnNumber()),
                          zero,
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
                          NewInteger(node->GetScriptId()),
                          NewInteger(node->GetLineNumber()),
                          NewInteger(node->GetColumnNumber()),
                          NewInteger(node->GetHitCount()),
                          children,
                          getLabelSetsForNode(node));
  }

  Local<Value> TranslateTimeProfile(const CpuProfile* profile,
                                    bool includeLineInfo) {
    Local<Object> js_profile = Nan::New<Object>();
    Nan::Set(js_profile, NewString("title"), profile->GetTitle());

#if NODE_MODULE_VERSION > NODE_11_0_MODULE_VERSION
    if (includeLineInfo) {
      Nan::Set(js_profile,
               NewString("topDownRoot"),
               TranslateLineNumbersTimeProfileRoot(profile->GetTopDownRoot()));
    } else {
      Nan::Set(js_profile,
               NewString("topDownRoot"),
               TranslateTimeProfileNode(profile->GetTopDownRoot()));
    }
#else
    Nan::Set(js_profile,
             NewString("topDownRoot"),
             TranslateTimeProfileNode(profile->GetTopDownRoot()));
#endif
    Nan::Set(js_profile,
             NewString("startTime"),
             Nan::New<Number>(profile->GetStartTime()));
    Nan::Set(js_profile,
             NewString("endTime"),
             Nan::New<Number>(profile->GetEndTime()));

    return js_profile;
  }
#undef FIELDS
};

bool isIdleSample(const CpuProfileNode* sample) {
  return
#if NODE_MODULE_VERSION > NODE_12_0_MODULE_VERSION
      sample->GetParent() == nullptr &&
#endif
      sample->GetChildrenCount() == 0 &&
      strncmp("(idle)", sample->GetFunctionNameStr(), 7) == 0;
}

LabelSetsByNode WallProfiler::GetLabelSetsByNode(CpuProfile* profile,
                                                 uint64_t startTime) {
  LabelSetsByNode labelSetsByNode;

  if (contexts.empty() || profile->GetSamplesCount() == 0) {
    return labelSetsByNode;
  }
  auto isolate = Isolate::GetCurrent();
  SampleContext sampleContext = contexts.pop_front();

  auto sampleCount = profile->GetSamplesCount();
  uint64_t halfInterval =
      (sampleCount > 1 ? (profile->GetSampleTimestamp(sampleCount - 1) -
                          profile->GetSampleTimestamp(0)) /
                             (sampleCount - 1)
                       : samplingInterval) /
      2;

  // Assumption is that startTime and profile->GetSampleTimestamp(0) were both
  // taken very closely to each other in real time so they can be used as a
  // common point of time origin, since startTime is taken in
  // WallProfiler::Start just after calling WallProfiler::StartImpl.
  // Sample times are in micros, context times are
  // in nanos. We are using these values - adjusted for half interval in the
  // past so we don't suffer any overflows anywhere in the arithmetic below.
  int64_t zeroSampleTime = profile->GetSampleTimestamp(0) - halfInterval;
  uint64_t zeroContextTime = startTime - halfInterval * 1000;

  for (int i = 0; i < sampleCount; i++) {
    auto sample = profile->GetSample(i);
    if (isIdleSample(sample)) {
      continue;
    }
    int64_t sampleTimestamp = profile->GetSampleTimestamp(i) - zeroSampleTime;
    // Compute earliest (inclusive) and latest (exclusive) context timestamps
    // that can belong to this sample. Use half the distance to neighboring
    // samples, or barring that half the sampling interval
    uint64_t earliest =
        1000 * (i > 0 ? (sampleTimestamp + (profile->GetSampleTimestamp(i - 1) -
                                            zeroSampleTime)) /
                            2
                      : (sampleTimestamp - halfInterval));
    uint64_t latest =
        1000 * (i < sampleCount - 1
                    ? (sampleTimestamp +
                       (profile->GetSampleTimestamp(i + 1) - zeroSampleTime)) /
                          2
                    : (sampleTimestamp + halfInterval));

    // This loop will drop all contexts that are too old to be associated with
    // the current sample; associate those (normally one) that are close enough
    // to it in time, and stop as soon as it sees a context that's too recent
    // for this sample.
    for (;;) {
      auto contextTimestamp = sampleContext.timestamp - zeroContextTime;
      if (contextTimestamp < earliest) {
        // Current sample context is too old (closer to the previous sample than
        // to this one), discard it and fetch the next one.
        if (contexts.empty()) {
          return labelSetsByNode;
        }
        sampleContext = contexts.pop_front();
      } else if (contextTimestamp >= latest) {
        // Current sample context is too recent (closer to the next sample than
        // to this one), we'll try to match it to the next sample.
        break;
      } else {
        // This sample context is the closest to this sample.
        if (sampleContext.labels) {
          auto it = labelSetsByNode.find(sample);
          Local<Array> array;
          if (it == labelSetsByNode.end()) {
            array = Nan::New<Array>();
            labelSetsByNode.emplace(sample, array);
          } else {
            array = it->second;
          }
          Nan::Set(
              array, array->Length(), sampleContext.labels.get()->Get(isolate));
        }
        // Sample context was consumed, fetch the next one
        if (contexts.empty()) {
          return labelSetsByNode;
        }
        sampleContext = contexts.pop_front();
        break;  // don't match more than one context to one sample
      }
    }
  }
  // Push the last popped sample context back into the ring to be used by the
  // next profile
  contexts.push_front(std::move(sampleContext));
  return labelSetsByNode;
}

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

  if (!info[1]->IsNumber()) {
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
    const int argc = 2;
    v8::Local<v8::Value> argv[argc] = {info[0], info[1]};
    v8::Local<v8::Function> cons = Nan::New(
        PerIsolateData::For(info.GetIsolate())->WallProfilerConstructor());
    info.GetReturnValue().Set(
        Nan::NewInstance(cons, argc, argv).ToLocalChecked());
  }
}

NAN_METHOD(WallProfiler::Start) {
  WallProfiler* wallProfiler =
      Nan::ObjectWrap::Unwrap<WallProfiler>(info.Holder());

  if (info.Length() != 3) {
    return Nan::ThrowTypeError("Start must have three arguments.");
  }
  if (!info[0]->IsString()) {
    return Nan::ThrowTypeError("Profile name must be a string.");
  }
  if (!info[1]->IsBoolean()) {
    return Nan::ThrowTypeError("Include lines flag must be a boolean.");
  }
  if (!info[2]->IsBoolean()) {
    return Nan::ThrowTypeError("With labels flag must be a boolean.");
  }

  Local<String> name =
      Nan::MaybeLocal<String>(info[0].As<String>()).ToLocalChecked();

  bool includeLines =
      Nan::MaybeLocal<Boolean>(info[1].As<Boolean>()).ToLocalChecked()->Value();

  bool withLabels =
      Nan::MaybeLocal<Boolean>(info[2].As<Boolean>()).ToLocalChecked()->Value();

  wallProfiler->StartImpl(name, includeLines, withLabels);
  auto now = withLabels ? wallProfiler->PushContext() : uv_hrtime();

#ifdef DD_WALL_USE_SIGPROF
  if (withLabels) {
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
#endif
  auto bi_now = BigInt::NewFromUnsigned(info.GetIsolate(), now);
  info.GetReturnValue().Set(bi_now);
}

void WallProfiler::StartImpl(Local<String> name,
                             bool includeLines,
                             bool withLabels) {
  if (includeLines) {
    GetProfiler()->StartProfiling(
        name, CpuProfilingMode::kCallerLineNumbers, withLabels);
  } else {
    GetProfiler()->StartProfiling(name, withLabels);
  }
}

NAN_METHOD(WallProfiler::Stop) {
  if (info.Length() != 3) {
    return Nan::ThrowTypeError("Stop must have three arguments.");
  }
  if (!info[0]->IsString()) {
    return Nan::ThrowTypeError("Profile name must be a string.");
  }
  if (!info[1]->IsBoolean()) {
    return Nan::ThrowTypeError("Include lines must be a boolean.");
  }

  if (!info[2]->IsBigInt()) {
    return Nan::ThrowTypeError("Start time must be a bigint.");
  }

  Local<String> name =
      Nan::MaybeLocal<String>(info[0].As<String>()).ToLocalChecked();

  bool includeLines =
      Nan::MaybeLocal<Boolean>(info[1].As<Boolean>()).ToLocalChecked()->Value();

  uint64_t startTime = Nan::MaybeLocal<BigInt>(info[2].As<BigInt>())
                           .ToLocalChecked()
                           ->Uint64Value();

  WallProfiler* wallProfiler =
      Nan::ObjectWrap::Unwrap<WallProfiler>(info.Holder());

  auto profile = wallProfiler->StopImpl(name, includeLines, startTime);

  info.GetReturnValue().Set(profile);
}

Local<Value> WallProfiler::StopImpl(Local<String> name,
                                    bool includeLines,
                                    uint64_t startTime) {
  auto profiler = GetProfiler();
  auto v8_profile = profiler->StopProfiling(name);
  Local<Value> profile =
      ProfileTranslator(GetLabelSetsByNode(v8_profile, startTime))
          .TranslateTimeProfile(v8_profile, includeLines);
  v8_profile->Delete();
  return profile;
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

  Nan::SetAccessor(tpl->InstanceTemplate(),
                   Nan::New("labelsCaptured").ToLocalChecked(),
                   GetLabelsCaptured);

  Nan::SetPrototypeMethod(tpl, "start", Start);
  Nan::SetPrototypeMethod(tpl, "dispose", Dispose);
  Nan::SetPrototypeMethod(tpl, "stop", Stop);

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

v8::Local<v8::Value> WallProfiler::GetLabels(Isolate* isolate) {
  if (!labels_) return v8::Undefined(isolate);
  return labels_->Get(isolate);
}

void WallProfiler::SetLabels(Isolate* isolate, Local<Value> value) {
  if (value->BooleanValue(isolate)) {
    labels_ = std::make_shared<Global<Value>>(isolate, value);
  } else {
    labels_.reset();
  }
}

NAN_GETTER(WallProfiler::GetLabels) {
  auto profiler = Nan::ObjectWrap::Unwrap<WallProfiler>(info.Holder());
  info.GetReturnValue().Set(profiler->GetLabels(info.GetIsolate()));
}

NAN_SETTER(WallProfiler::SetLabels) {
  auto profiler = Nan::ObjectWrap::Unwrap<WallProfiler>(info.Holder());
  profiler->SetLabels(info.GetIsolate(), value);
}

NAN_GETTER(WallProfiler::GetLabelsCaptured) {
  auto profiler = Nan::ObjectWrap::Unwrap<WallProfiler>(info.Holder());
  info.GetReturnValue().Set(profiler->GetLabelsCaptured());
}

uint64_t WallProfiler::PushContext() {
  // Be careful this is called in a signal handler context therefore all
  // operations must be async signal safe (in particular no allocations). Our
  // ring buffer avoids allocations.
  auto now = uv_hrtime();
  contexts.push_back(SampleContext(labels_, now));
  if (labels_) {
    labelsCaptured = true;
  }
  return now;
}

}  // namespace dd
