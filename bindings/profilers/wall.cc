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
#include <mutex>
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

// static std::string safe_string(v8::Isolate* isolate,
//                                v8::Local<v8::String> maybe_string) {
//   auto len = maybe_string->Utf8Length(isolate);
//   std::string buffer(len + 1, 0);
//   maybe_string->WriteUtf8(isolate, &buffer[0], len + 1);
//   return std::string(buffer.c_str());
// }

// Declare v8::base::TimeTicks::Now. It is exported from the node executable so
// our addon will be able to dynamically link to the symbol when loaded.
namespace v8 {
namespace base {
struct TimeTicks {
  static int64_t Now();
};
}  // namespace base
}  // namespace v8

using namespace v8;

namespace dd {

using ProfilerMap = std::unordered_map<Isolate*, WallProfiler*>;

static std::atomic<ProfilerMap*> profilers(new ProfilerMap());
static std::mutex update_profilers;

#ifdef DD_WALL_USE_SIGPROF
static void (*old_handler)(int, siginfo_t*, void*) = nullptr;

static void sighandler(int sig, siginfo_t* info, void* context) {
  if (!old_handler) {
    return;
  }

  auto isolate = Isolate::GetCurrent();
  WallProfiler* prof = nullptr;
  auto prof_map = profilers.exchange(nullptr, std::memory_order_acq_rel);
  if (prof_map) {
    auto prof_it = prof_map->find(isolate);
    if (prof_it != prof_map->end()) {
      prof = prof_it->second;
    }
    profilers.store(prof_map, std::memory_order_release);
  }
  if (prof && !prof->collectSampleAllowed()) {
    // puts("Dropping sample");
    return;
  }
  auto now = v8::base::TimeTicks::Now();
  old_handler(sig, info, context);
  if (prof) {
    prof->PushContext(now);
  }
}
#endif

class ProfileTranslator {
 public:
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

  ProfileTranslator(LabelSetsByNode&& nls) : labelSetsByNode(std::move(nls)) {}

  Local<Array> getLabelSetsForNode(const CpuProfileNode* node) {
    auto it = labelSetsByNode.find(node);
    if (it != labelSetsByNode.end()) {
      auto retval = it->second;
      labelSetsByNode.erase(it);
      return retval.labelSets;
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

  Local<Integer> NewInteger(int32_t x) { return Integer::New(isolate, x); }

  Local<Array> NewArray(int length) { return Array::New(isolate, length); }

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

    auto it = labelSetsByNode.find(node);
    auto hitcount = node->GetHitCount();
    auto labelSets = emptyArray;
    if (labelSetsByNode.size()) {
      if (it != labelSetsByNode.end()) {
        hitcount = it->second.hitcount;
        labelSets = it->second.labelSets;
      } else {
        hitcount = 0;
      }
    }
    return CreateTimeNode(node->GetFunctionName(),
                          node->GetScriptResourceName(),
                          NewInteger(node->GetScriptId()),
                          NewInteger(node->GetLineNumber()),
                          NewInteger(node->GetColumnNumber()),
                          NewInteger(hitcount),
                          children,
                          labelSets);
  }

  Local<Value> TranslateTimeProfile(const CpuProfile* profile,
                                    bool includeLineInfo) {
    Local<Object> js_profile = Nan::New<Object>();

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

std::pair<int, int> countHits(const CpuProfileNode* node) {
  int hitcount = node->GetHitCount();
  int nodecount = 1;
  for (int i = 0; i < node->GetChildrenCount(); ++i) {
    auto r = countHits(node->GetChild(i));
    nodecount += r.first;
    hitcount += r.second;
  }
  return {nodecount, hitcount};
}

LabelSetsByNode WallProfiler::GetLabelSetsByNode(CpuProfile* profile,
                                                 ContextBuffer& contexts) {
  LabelSetsByNode labelSetsByNode;

  auto sampleCount = profile->GetSamplesCount();
  if (contexts.empty() || sampleCount == 0) {
    return labelSetsByNode;
  }
  auto isolate = Isolate::GetCurrent();
  // auto labelKey = Nan::New<String>("label").ToLocalChecked();

  int deltaIdx = 0;

  // skip first sample because it's the one taken on profiler start, outside of
  // signal handler
  for (int i = 1; i < sampleCount; i++) {
    // Handle out-of-order samples, hypothesis is that at most 2 consecutive
    // samples can be out-of-order
    if (deltaIdx == 1) {
      // previous iteration was processing next sample, so this one should
      // process previous sample
      deltaIdx = -1;
    } else if (deltaIdx == -1) {
      // previous iteration was processing previous sample, returns to normal
      // index
      deltaIdx = 0;
    } else if (i < sampleCount - 1 && profile->GetSampleTimestamp(i + 1) <
                                          profile->GetSampleTimestamp(i)) {
      // detected  out-of-order sample, process next sample
      deltaIdx = 1;
    }

    auto sampleIdx = i + deltaIdx;
    auto sample = profile->GetSample(sampleIdx);

    // if (deltaIdx != 0) {
    //   printf("Processing sample#%d instead of #%d\n", sampleIdx, i);
    // }

    if (isIdleSample(sample)) {
      continue;
    }

    auto sampleTimestamp = profile->GetSampleTimestamp(sampleIdx);

    // This loop will drop all contexts that are too old to be associated with
    // the current sample; associate those (normally one) that are close enough
    // to it in time, and stop as soon as it sees a context that's too recent
    // for this sample.
    for (;;) {
      if (contexts.empty()) {
        // printf("Missing context1@%ld for %s (i=%d), state=%d\n",
        //        sampleTimestamp,
        //        sample->GetFunctionNameStr(),
        //        sampleIdx,
        //        profile->GetSampleState(sampleIdx));
        break;
      }

      auto& sampleContext = contexts.front();
      // std::string labels;
      // if (sampleContext.labels) {
      //   auto val = sampleContext.labels.get()->Get(isolate);
      //   auto obj = Nan::MaybeLocal<Object>(val.As<Object>()).ToLocalChecked();
      //   auto label = Nan::Get(obj, labelKey)
      //                    .ToLocalChecked()
      //                    ->ToString(isolate->GetCurrentContext())
      //                    .ToLocalChecked();
      //   labels = safe_string(isolate, label);
      // }

      if (sampleContext.time_to < sampleTimestamp) {
        // Current sample context is too old, discard it and fetch the next one.
        contexts.pop_front();
      } else if (sampleContext.time_from > sampleTimestamp) {
        // printf("Missing context2@%ld for %s (i=%d), state=%d\n",
        //        sampleTimestamp,
        //        sample->GetFunctionNameStr(),
        //        sampleIdx,
        //        profile->GetSampleState(sampleIdx));

        // Current sample context is too recent, we'll try to match it to the
        // next sample.
        break;
      } else {
        // This sample context is the closest to this sample.
        auto it = labelSetsByNode.find(sample);
        Local<Array> array;
        if (it == labelSetsByNode.end()) {
          array = Nan::New<Array>();
          assert(labelSetsByNode.find(sample) == labelSetsByNode.end());
          labelSetsByNode[sample] = {array, 1};
        } else {
          array = it->second.labelSets;
          ++it->second.hitcount;
        }
        if (sampleContext.labels) {
          Nan::Set(
              array, array->Length(), sampleContext.labels.get()->Get(isolate));
        }
        // printf("Matching context, time_from=%ld, time_to=%lu, labels=%s\n",
        //        sampleContext.time_from,
        //        sampleContext.time_to,
        //        labels.c_str());

        // Sample context was consumed, fetch the next one
        contexts.pop_front();
        break;  // don't match more than one context to one sample
      }
    }
  }

  while (!contexts.empty()) {
    contexts.pop_front();
  }
  return labelSetsByNode;
}

WallProfiler::WallProfiler(int intervalMicros, int durationMicros)
    : samplingInterval(intervalMicros),
      contexts_(durationMicros * 2 / intervalMicros) {}

WallProfiler::~WallProfiler() {
  Dispose(nullptr);
}

template <typename F>
void updateProfilers(F updateFn) {
  std::lock_guard<std::mutex> lock(update_profilers);
  auto currProfilers = profilers.load(std::memory_order_acquire);
  // Wait until sighandler is done using the map
  while (!currProfilers) {
    currProfilers = profilers.load(std::memory_order_relaxed);
  }
  auto newProfilers = new ProfilerMap(*currProfilers);
  updateFn(newProfilers);
  // Wait until sighandler is done using the map before installing a new map.
  // The value in profilers is either nullptr or currProfilers.
  for (;;) {
    ProfilerMap* currProfilers2 = currProfilers;
    if (profilers.compare_exchange_weak(
            currProfilers2, newProfilers, std::memory_order_acq_rel)) {
      break;
    }
  }
  delete currProfilers;
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

  if (info.Length() != 2) {
    return Nan::ThrowTypeError("Start must have two arguments.");
  }
  if (!info[0]->IsBoolean()) {
    return Nan::ThrowTypeError("Include lines flag must be a boolean.");
  }
  if (!info[1]->IsBoolean()) {
    return Nan::ThrowTypeError("With labels flag must be a boolean.");
  }

  bool includeLines =
      Nan::MaybeLocal<Boolean>(info[0].As<Boolean>()).ToLocalChecked()->Value();

  bool withLabels =
      Nan::MaybeLocal<Boolean>(info[1].As<Boolean>()).ToLocalChecked()->Value();

  wallProfiler->StartImpl(includeLines, withLabels);
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
}

void WallProfiler::StartImpl(bool includeLines, bool withLabels) {
  if (started_) {
    return;
  }
  includeLines_ = includeLines;
  withLabels_ = withLabels;

  profilerId_ = StartInternal();
  started_ = true;
}

v8::ProfilerId WallProfiler::StartInternal() {
  return GetProfiler()
      ->Start(v8::CpuProfilingOptions{
          includeLines_ ? CpuProfilingMode::kCallerLineNumbers
                        : CpuProfilingMode::kLeafNodeLineNumbers,
          withLabels_ ? v8::CpuProfilingOptions::kNoSampleLimit : 0})
      .id;
}

NAN_METHOD(WallProfiler::Stop) {
  if (info.Length() != 1) {
    return Nan::ThrowTypeError("Stop must have one argument.");
  }
  if (!info[0]->IsBoolean()) {
    return Nan::ThrowTypeError("Include lines must be a boolean.");
  }

  bool restart =
      Nan::MaybeLocal<Boolean>(info[0].As<Boolean>()).ToLocalChecked()->Value();

  WallProfiler* wallProfiler =
      Nan::ObjectWrap::Unwrap<WallProfiler>(info.Holder());

#if NODE_MODULE_VERSION < NODE_16_0_MODULE_VERSION
  auto profile = wallProfiler->StopImplOld(restart);
#else
  auto profile = wallProfiler->StopImpl(restart);
#endif

  info.GetReturnValue().Set(profile);
}

Local<Value> WallProfiler::StopImpl(bool restart) {
  auto oldProfilerId = profilerId_;
  collectSamples_.store(false, std::memory_order_relaxed);
  std::atomic_signal_fence(std::memory_order_release);

  // make sure timestamp changes to avoid having samples from previous profile
  auto now = v8::base::TimeTicks::Now();
  while (v8::base::TimeTicks::Now() == now) {}

  if (restart) {
    profilerId_ = StartInternal();
  }
  auto v8_profile = GetProfiler()->Stop(oldProfilerId);

  ContextBuffer contexts{contexts_.capacity()};
  std::swap(contexts, contexts_);

  // make sure timestamp changes to avoid mixing start sample and a sample from signal handler
  now = v8::base::TimeTicks::Now();
  while (v8::base::TimeTicks::Now() == now) {}

  collectSamples_.store(true, std::memory_order_relaxed);
  std::atomic_signal_fence(std::memory_order_release);

  Local<Value> profile =
      ProfileTranslator(GetLabelSetsByNode(v8_profile, contexts))
          .TranslateTimeProfile(v8_profile, includeLines_);
  v8_profile->Delete();

  if (!restart) {
    Dispose(v8::Isolate::GetCurrent());
  }
  started_ = restart;

  return profile;
}

Local<Value> WallProfiler::StopImplOld(bool restart) {
  auto v8_profile = GetProfiler()->Stop(profilerId_);
  Local<Value> profile =
      ProfileTranslator(GetLabelSetsByNode(v8_profile, contexts_))
          .TranslateTimeProfile(v8_profile, includeLines_);
  v8_profile->Delete();
  Dispose(v8::Isolate::GetCurrent());
  if (restart) {
    profilerId_ = StartInternal();
  } else {
    started_ = false;
  }

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
  auto labels = *curLabels.load(std::memory_order_relaxed);
  if (!labels) return v8::Undefined(isolate);
  return labels->Get(isolate);
}

void WallProfiler::SetLabels(Isolate* isolate, Local<Value> value) {
  auto newCurLabels = curLabels.load(std::memory_order_relaxed) == &labels1
                          ? &labels2
                          : &labels1;
  if (value->BooleanValue(isolate)) {
    *newCurLabels = std::make_shared<Global<Value>>(isolate, value);
  } else {
    newCurLabels->reset();
  }
  std::atomic_signal_fence(std::memory_order_release);
  curLabels.store(newCurLabels, std::memory_order_relaxed);
}

NAN_GETTER(WallProfiler::GetLabels) {
  auto profiler = Nan::ObjectWrap::Unwrap<WallProfiler>(info.Holder());
  info.GetReturnValue().Set(profiler->GetLabels(info.GetIsolate()));
}

NAN_SETTER(WallProfiler::SetLabels) {
  auto profiler = Nan::ObjectWrap::Unwrap<WallProfiler>(info.Holder());
  profiler->SetLabels(info.GetIsolate(), value);
}

void WallProfiler::PushContext(int64_t time_from) {
  // Be careful this is called in a signal handler context therefore all
  // operations must be async signal safe (in particular no allocations). Our
  // ring buffer avoids allocations.
  auto labels = curLabels.load(std::memory_order_relaxed);
  std::atomic_signal_fence(std::memory_order_acquire);
  contexts_.push_back(
      SampleContext(*labels, time_from, v8::base::TimeTicks::Now()));
}

}  // namespace dd
