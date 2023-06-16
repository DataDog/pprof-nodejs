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

static std::atomic<ProfilerMap*> g_profilers(new ProfilerMap());
static std::mutex g_profilers_update_mtx;

#ifdef DD_WALL_USE_SIGPROF
static void (*g_old_handler)(int, siginfo_t*, void*) = nullptr;

static void sighandler(int sig, siginfo_t* info, void* context) {
  if (!g_old_handler) {
    return;
  }

  WallProfiler* prof = nullptr;
  // Prevent updates to profiler map by atomically setting g_profilers to null
  auto prof_map = g_profilers.exchange(nullptr, std::memory_order_acq_rel);
  if (prof_map) {
    auto isolate = Isolate::GetCurrent();
    auto prof_it = prof_map->find(isolate);
    if (prof_it != prof_map->end()) {
      prof = prof_it->second;
    }
    // Allow updates
    g_profilers.store(prof_map, std::memory_order_release);
  }
  if (prof && !prof->collectSampleAllowed()) {
    return;
  }
  auto time_from = v8::base::TimeTicks::Now();
  g_old_handler(sig, info, context);
  if (prof) {
    auto time_to = v8::base::TimeTicks::Now();
    prof->PushContext(time_from, time_to);
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
#undef FIELDS
    return js_node;
  }

  Local<Integer> NewInteger(int32_t x) { return Integer::New(isolate, x); }

  Local<Array> NewArray(int length) { return Array::New(isolate, length); }

  Local<String> NewString(const char* str) {
    return Nan::New<String>(str).ToLocalChecked();
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

    if (includeLineInfo) {
      Nan::Set(js_profile,
               NewString("topDownRoot"),
               TranslateLineNumbersTimeProfileRoot(profile->GetTopDownRoot()));
    } else {
      Nan::Set(js_profile,
               NewString("topDownRoot"),
               TranslateTimeProfileNode(profile->GetTopDownRoot()));
    }
    Nan::Set(js_profile,
             NewString("startTime"),
             Nan::New<Number>(profile->GetStartTime()));
    Nan::Set(js_profile,
             NewString("endTime"),
             Nan::New<Number>(profile->GetEndTime()));

    return js_profile;
  }
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
                                                 ContextBuffer& contexts) {
  LabelSetsByNode labelSetsByNode;

  auto sampleCount = profile->GetSamplesCount();
  if (contexts.empty() || sampleCount == 0) {
    return labelSetsByNode;
  }

  auto isolate = Isolate::GetCurrent();
  // auto labelKey = Nan::New<String>("label").ToLocalChecked();

  auto contextIt = contexts.begin();

  // deltaIdx is the offset of the sample to process compared to current
  // iteration index
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

    if (isIdleSample(sample)) {
      continue;
    }

    auto sampleTimestamp = profile->GetSampleTimestamp(sampleIdx);

    // This loop will drop all contexts that are too old to be associated with
    // the current sample; association is done by matching each sample with
    // context whose [time_from,time_to] interval encompasses sample timestamp.
    while (contextIt != contexts.end()) {
      auto& sampleContext = *contextIt;
      if (sampleContext.time_to < sampleTimestamp) {
        // Current sample context is too old, discard it and fetch the next one.
        ++contextIt;
      } else if (sampleContext.time_from > sampleTimestamp) {
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

        // Sample context was consumed, fetch the next one
        ++contextIt;
        break;  // don't match more than one context to one sample
      }
    }
  }

  return labelSetsByNode;
}

WallProfiler::WallProfiler(int samplingPeriodMicros,
                           int durationMicros,
                           bool includeLines,
                           bool withLabels)
    : samplingPeriodMicros_(samplingPeriodMicros),
      includeLines_(includeLines),
      withLabels_(withLabels) {
  contexts_.reserve(durationMicros * 2 / samplingPeriodMicros);
  curLabels_.store(&labels1_, std::memory_order_relaxed);
  collectSamples_.store(false, std::memory_order_relaxed);
}

WallProfiler::~WallProfiler() {
  Dispose(nullptr);
}

template <typename F>
bool updateProfilers(F updateFn) {
  std::lock_guard<std::mutex> lock(g_profilers_update_mtx);
  auto currProfilers = g_profilers.load(std::memory_order_acquire);
  // Wait until sighandler is done using the map
  while (!currProfilers) {
    currProfilers = g_profilers.load(std::memory_order_relaxed);
  }
  auto newProfilers = new ProfilerMap(*currProfilers);
  auto res = updateFn(newProfilers);
  // Wait until sighandler is done using the map before installing a new map.
  // The value in profilers is either nullptr or currProfilers.
  for (;;) {
    ProfilerMap* currProfilers2 = currProfilers;
    if (g_profilers.compare_exchange_weak(
            currProfilers2, newProfilers, std::memory_order_acq_rel)) {
      break;
    }
  }
  delete currProfilers;
  return res;
}

void WallProfiler::Dispose(Isolate* isolate) {
  if (cpuProfiler_ != nullptr) {
    cpuProfiler_->Dispose();
    cpuProfiler_ = nullptr;

    updateProfilers([isolate, this](auto map) {
      if (isolate != nullptr) {
        auto it = map->find(isolate);
        if (it != map->end() && it->second == this) {
          map->erase(it);
          return true;
        }
        return false;
      } else {
        // TODO: use map->erase_if once we can use C++20.
        for (auto it = map->begin(), last = map->end(); it != last;) {
          if (it->second == this) {
            it = map->erase(it);
            return true;
          } else {
            ++it;
          }
        }
        return false;
      }
    });
  }
}

NAN_METHOD(WallProfiler::New) {
  if (info.Length() != 4) {
    return Nan::ThrowTypeError("WallProfiler must have four arguments.");
  }

  if (!info[0]->IsNumber()) {
    return Nan::ThrowTypeError("Sample period must be a number.");
  }
  if (!info[1]->IsNumber()) {
    return Nan::ThrowTypeError("Duration must be a number.");
  }
  if (!info[2]->IsBoolean()) {
    return Nan::ThrowTypeError("includeLines must be a boolean.");
  }
  if (!info[3]->IsBoolean()) {
    return Nan::ThrowTypeError("withLabels must be a boolean.");
  }

  if (info.IsConstructCall()) {
    int interval = info[0].As<Integer>()->Value();
    int duration = info[1].As<Integer>()->Value();

    if (interval <= 0) {
      return Nan::ThrowTypeError("Sample rate must be positive.");
    }
    if (duration <= 0) {
      return Nan::ThrowTypeError("Duration must be positive.");
    }
    if (duration < interval) {
      return Nan::ThrowTypeError("Duration must not be less than sample rate.");
    }

    bool includeLines = info[2].As<Boolean>()->Value();
    bool withLabels = info[3].As<Boolean>()->Value();

    WallProfiler* obj =
        new WallProfiler(interval, duration, includeLines, withLabels);
    obj->Wrap(info.This());
    info.GetReturnValue().Set(info.This());
  } else {
    const int argc = 4;
    v8::Local<v8::Value> argv[argc] = {info[0], info[1], info[2], info[3]};
    v8::Local<v8::Function> cons = Nan::New(
        PerIsolateData::For(info.GetIsolate())->WallProfilerConstructor());
    info.GetReturnValue().Set(
        Nan::NewInstance(cons, argc, argv).ToLocalChecked());
  }
}

NAN_METHOD(WallProfiler::Start) {
  WallProfiler* wallProfiler =
      Nan::ObjectWrap::Unwrap<WallProfiler>(info.Holder());

  if (info.Length() != 0) {
    return Nan::ThrowTypeError("Start must not have any arguments.");
  }

  auto res = wallProfiler->StartImpl();
  if (!res.success) {
    return Nan::ThrowTypeError(res.msg.c_str());
  }
}

Result WallProfiler::StartImpl() {
  if (started_) {
    return Result{"Start called on already started profiler, stop it first."};
  }

  profileIdx_ = 0;

  if (!CreateV8CpuProfiler()) {
    return Result{"Cannot start profiler: another profiler is already active."};
  }

  auto res = StartInternal(profileId_);
  if (!res.success) {
    return res;
  }

#ifdef DD_WALL_USE_SIGPROF
  if (withLabels_) {
    struct sigaction sa, old_sa;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sa.sa_sigaction = &sighandler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGPROF, &sa, &old_sa);

    // At the end of a cycle start is called before stop,
    // at this point old_sa.sa_sigaction is sighandler !
    if (!g_old_handler) {
      g_old_handler = old_sa.sa_sigaction;
    }
  }
#endif

  collectSamples_.store(true, std::memory_order_relaxed);
  started_ = true;
  return {};
}

Result WallProfiler::StartInternal(std::string& profileId) {
  char buf[128];
  snprintf(buf, sizeof(buf), "pprof-%lld", profileIdx_++);
  v8::Local<v8::String> title = Nan::New<String>(buf).ToLocalChecked();
  auto status = cpuProfiler_->StartProfiling(
      title,
      v8::CpuProfilingOptions(includeLines_
                                  ? CpuProfilingMode::kCallerLineNumbers
                                  : CpuProfilingMode::kLeafNodeLineNumbers,
                              withLabels_ ? contexts_.capacity() : 0));

  switch (status) {
    case CpuProfilingStatus::kStarted:
      break;
    case CpuProfilingStatus::kAlreadyStarted:
      return Result("Failed to start V8 profiler: already started");
    case CpuProfilingStatus::kErrorTooManyProfilers:
      return Result("Failed to start V8 profiler: too many profilers");
    default:
      return Result("Failed to start V8 profiler: unknown error");
  }
  profileId = buf;
  return {};
}

NAN_METHOD(WallProfiler::Stop) {
  if (info.Length() != 1) {
    return Nan::ThrowTypeError("Stop must have one argument.");
  }
  if (!info[0]->IsBoolean()) {
    return Nan::ThrowTypeError("Restart must be a boolean.");
  }

  bool restart = info[0].As<Boolean>()->Value();

  WallProfiler* wallProfiler =
      Nan::ObjectWrap::Unwrap<WallProfiler>(info.Holder());

  v8::Local<v8::Value> profile;
#if NODE_MODULE_VERSION < NODE_16_0_MODULE_VERSION
  auto err = wallProfiler->StopImplOld(restart, profile);
#else
  auto err = wallProfiler->StopImpl(restart, profile);
#endif

  if (!err.success) {
    return Nan::ThrowTypeError(err.msg.c_str());
  }
  info.GetReturnValue().Set(profile);
}

Result WallProfiler::StopImpl(bool restart, v8::Local<v8::Value>& profile) {
  if (!started_) {
    return Result{"Stop called on not started profiler."};
  }

  auto oldProfileId = profileId_;
  collectSamples_.store(false, std::memory_order_relaxed);
  std::atomic_signal_fence(std::memory_order_release);

  // make sure timestamp changes to avoid having samples from previous profile
  auto now = v8::base::TimeTicks::Now();
  while (v8::base::TimeTicks::Now() == now) {
  }

  if (restart) {
    StartInternal(profileId_);
  }
  auto v8_profile = cpuProfiler_->StopProfiling(
      Nan::New<String>(oldProfileId).ToLocalChecked());

  ContextBuffer contexts;
  contexts.reserve(contexts_.capacity());
  std::swap(contexts, contexts_);

  if (restart) {
    // make sure timestamp changes to avoid mixing start sample and a sample
    // from signal handler
    now = v8::base::TimeTicks::Now();
    while (v8::base::TimeTicks::Now() == now) {
    }
    collectSamples_.store(true, std::memory_order_relaxed);
    std::atomic_signal_fence(std::memory_order_release);
  }

  profile = ProfileTranslator(GetLabelSetsByNode(v8_profile, contexts))
                .TranslateTimeProfile(v8_profile, includeLines_);
  v8_profile->Delete();

  if (!restart) {
    Dispose(v8::Isolate::GetCurrent());
  }
  started_ = restart;

  return {};
}

Result WallProfiler::StopImplOld(bool restart, v8::Local<v8::Value>& profile) {
  if (started_) {
    return Result{"Stop called on not started profiler."};
  }

  auto v8_profile = cpuProfiler_->StopProfiling(
      Nan::New<String>(profileId_).ToLocalChecked());

  profile = ProfileTranslator(GetLabelSetsByNode(v8_profile, contexts_))
                .TranslateTimeProfile(v8_profile, includeLines_);
  contexts_.clear();
  v8_profile->Delete();
  Dispose(v8::Isolate::GetCurrent());
  if (restart) {
    auto res = StartInternal(profileId_);
  } else {
    started_ = false;
  }

  return {};
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
  Nan::SetPrototypeMethod(tpl, "stop", Stop);

  PerIsolateData::For(Isolate::GetCurrent())
      ->WallProfilerConstructor()
      .Reset(Nan::GetFunction(tpl).ToLocalChecked());
  Nan::Set(target, className, Nan::GetFunction(tpl).ToLocalChecked());
}

// A new CPU profiler object will be created each time profiling is started
// to work around https://bugs.chromium.org/p/v8/issues/detail?id=11051.
// TODO: Fixed in v16. Delete this hack when deprecating v14.
v8::CpuProfiler* WallProfiler::CreateV8CpuProfiler() {
  if (cpuProfiler_ == nullptr) {
    v8::Isolate* isolate = v8::Isolate::GetCurrent();

    bool inserted = updateProfilers([isolate, this](auto map) {
      return map->emplace(isolate, this).second;
    });

    if (!inserted) {
      // refuse to create a new profiler if one is already active
      return nullptr;
    }
    cpuProfiler_ = v8::CpuProfiler::New(isolate);
    cpuProfiler_->SetSamplingInterval(samplingPeriodMicros_);
  }
  return cpuProfiler_;
}

v8::Local<v8::Value> WallProfiler::GetLabels(Isolate* isolate) {
  auto labels = *curLabels_.load(std::memory_order_relaxed);
  if (!labels) return v8::Undefined(isolate);
  return labels->Get(isolate);
}

void WallProfiler::SetLabels(Isolate* isolate, Local<Value> value) {
  // Need to be careful here, because we might be interrupted by a
  // signal handler that will make use of curLabels_.
  // Update of shared_ptr is not atomic, so instead we use a pointer
  // (curLabels_) that points on two shared_ptr (labels1_ and labels2_), update
  // the shared_ptr that is not currently in use and then atomically update
  // curLabels_.
  auto newCurLabels = curLabels_.load(std::memory_order_relaxed) == &labels1_
                          ? &labels2_
                          : &labels1_;
  if (value->BooleanValue(isolate)) {
    *newCurLabels = std::make_shared<Global<Value>>(isolate, value);
  } else {
    newCurLabels->reset();
  }
  std::atomic_signal_fence(std::memory_order_release);
  curLabels_.store(newCurLabels, std::memory_order_relaxed);
}

NAN_GETTER(WallProfiler::GetLabels) {
  auto profiler = Nan::ObjectWrap::Unwrap<WallProfiler>(info.Holder());
  info.GetReturnValue().Set(profiler->GetLabels(info.GetIsolate()));
}

NAN_SETTER(WallProfiler::SetLabels) {
  auto profiler = Nan::ObjectWrap::Unwrap<WallProfiler>(info.Holder());
  profiler->SetLabels(info.GetIsolate(), value);
}

void WallProfiler::PushContext(int64_t time_from, int64_t time_to) {
  // Be careful this is called in a signal handler context therefore all
  // operations must be async signal safe (in particular no allocations).
  // Our ring buffer avoids allocations.
  auto labels = curLabels_.load(std::memory_order_relaxed);
  std::atomic_signal_fence(std::memory_order_acquire);
  if (contexts_.size() < contexts_.capacity()) {
    contexts_.push_back({*labels, time_from, time_to});
  }
}

}  // namespace dd
