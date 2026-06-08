/*
 * Copyright 2023 Datadog, Inc
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

#include <nan.h>
#include <node.h>
#include <v8-internal.h>
#include <v8-profiler.h>
#include <cinttypes>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <type_traits>
#include <unordered_set>
#include <vector>

#include "map-get.hh"
#include "per-isolate-data.hh"
#include "translate-time-profile.hh"
#include "wall.hh"

#ifndef _WIN32
#define DD_WALL_USE_SIGPROF true

// Declare v8::base::TimeTicks::Now. It is exported from the node executable so
// our addon will be able to dynamically link to the symbol when loaded.
namespace v8 {
namespace base {
struct TimeTicks {
  static int64_t Now();
};
}  // namespace base
#if NODE_MAJOR_VERSION >= 22

// Available from 22.7.0
#define DD_WALL_USE_CPED true

namespace internal {
#if NODE_MAJOR_VERSION < 25
struct HandleScopeData {
  v8::internal::Address* next;
  v8::internal::Address* limit;
};
#endif  // NODE_MAJOR_VERSION < 25
#if NODE_MAJOR_VERSION >= 24
constexpr int kHandleBlockSize = v8::internal::KB - 2;
#endif  // NODE_MAJOR_VERSION >= 24
}  // namespace internal
#else  // NODE_MAJOR_VERSION >= 22
#define DD_WALL_USE_CPED false
#endif  //
}  // namespace v8

static int64_t Now() {
  return v8::base::TimeTicks::Now();
};

#else
#define DD_WALL_USE_SIGPROF false
#define DD_WALL_USE_CPED false

static int64_t Now() {
  return 0;
};

#endif

using namespace v8;

namespace dd {

class SignalGuard {
  std::atomic<bool>& guard_;

  inline void store(bool value) {
    std::atomic_signal_fence(std::memory_order_release);
    guard_.store(value, std::memory_order_relaxed);
  }

 public:
  inline SignalGuard(std::atomic<bool>& guard) : guard_(guard) { store(true); }

  inline ~SignalGuard() { store(false); }
};

void SetContextPtr(ContextPtr& contextPtr,
                   Isolate* isolate,
                   Local<Value> value) {
  if (!value->IsNullOrUndefined()) {
    contextPtr = std::make_shared<Global<Value>>(isolate, value);
  } else {
    contextPtr.reset();
  }
}

class PersistentContextPtr : public node::ObjectWrap {
  ContextPtr context;
  // Back-pointer to the WallProfiler that created this PCP. Guaranteed to be
  // a valid pointer whenever pprev_ != nullptr — ~WallProfiler nulls pprev_
  // on every live PCP before any of them can outlive the profiler.
  WallProfiler* const profiler_;
  // Intrusive doubly-linked list, threaded through the WallProfiler's
  // liveContextPtrHead_. pprev_ is the address of the pointer that currently
  // references this PCP — &profiler_->liveContextPtrHead_ if we're the head,
  // &predecessor->next_ otherwise — so by construction *pprev_ == this.
  // Storing the address-of (rather than the predecessor itself) lets ~PCP
  // unlink without a head/non-head branch: *pprev_ = next_ followed by
  // next_->pprev_ = pprev_ does both ends in one shape. pprev_ == nullptr
  // is the "detached" sentinel — set by ~WallProfiler before deleting us,
  // so our unlink becomes a no-op and we don't poke at the dying profiler's
  // memory.
  PersistentContextPtr** pprev_ = nullptr;
  PersistentContextPtr* next_ = nullptr;

  friend class WallProfiler;

 public:
  PersistentContextPtr(WallProfiler* profiler, Local<Object> wrap);

  ~PersistentContextPtr();

  void Set(Isolate* isolate, const Local<Value>& value) {
    SetContextPtr(context, isolate, value);
  }

  ContextPtr Get() const { return context; }

  static PersistentContextPtr* Unwrap(Local<Object> wrap) {
    return node::ObjectWrap::Unwrap<PersistentContextPtr>(wrap);
  }
};

PersistentContextPtr::PersistentContextPtr(WallProfiler* profiler,
                                           Local<Object> wrap)
    : profiler_(profiler) {
  Wrap(wrap);
  // Splice ourselves at the head of profiler's live list.
  auto** headSlot = profiler->liveContextPtrHeadSlot();
  next_ = *headSlot;
  pprev_ = headSlot;
  if (next_ != nullptr) next_->pprev_ = &next_;
  *headSlot = this;
  profiler->recordContextCreate();
}

PersistentContextPtr::~PersistentContextPtr() {
  // pprev_ != nullptr means we're still on profiler_'s live list, which means
  // ~WallProfiler hasn't run yet and the back-pointer is still valid. Unlink
  // and release the slot in one step. If pprev_ is null, ~WallProfiler is
  // taking care of us so don't do anything (don't unlink us, and leave its
  // counter alone as it's about to go away).
  if (pprev_ != nullptr) {
    *pprev_ = next_;
    if (next_ != nullptr) next_->pprev_ = pprev_;
    profiler_->recordContextRelease();
  }
}

inline void* GetAlignedPointerFromInternalField(Object* object, int index) {
#if NODE_MAJOR_VERSION >= 26
  return object->GetAlignedPointerFromInternalField(
      index, kEmbedderDataTypeTagDefault);
#else
  return object->GetAlignedPointerFromInternalField(index);
#endif
}

// Maximum number of rounds in the GetV8ToEpochOffset
static constexpr int MAX_EPOCH_OFFSET_ATTEMPTS = 20;

int getTotalHitCount(const v8::CpuProfileNode* node, bool* noHitLeaf) {
  int count = node->GetHitCount();
  auto child_count = node->GetChildrenCount();

  for (int i = 0; i < child_count; ++i) {
    count += getTotalHitCount(node->GetChild(i), noHitLeaf);
  }
  if (child_count == 0 && count == 0) {
    *noHitLeaf = true;
  }
  return count;
}

/** Returns 0 if no bug detected, 1 if possible bug (it could be a false
 * positive), 2 if bug detected for certain. */
int detectV8Bug(const v8::CpuProfile* profile) {
  /* When the profiler operates correctly, there'll be at least one node with
   * a non-zero hit count and the number of samples will be strictly greater
   * than the number of hits because they'll contain at least the starting
   * sample and potentially some deoptimization samples. If these conditions
   * don't hold, it implies that v8::SamplingEventsProcessor::ProcessOneSample
   * loop is stuck for ticks_buffer_ or vm_ticks_buffer_. */

  bool noHitLeaf = false;
  auto totalHitCount = getTotalHitCount(profile->GetTopDownRoot(), &noHitLeaf);
  if (totalHitCount == 0) {
    return 2;
  }

  if (profile->GetSamplesCount() == totalHitCount && !noHitLeaf) {
    /*  Checking number of samples against number of hits potentially leads to
     * false positive because some ticks samples can be discarded if their
     * timestamp is older than profile start time because of queueing.
     * Additionally check for leaf nodes with zero hit count, if there is one,
     * this implies that one non-tick sample was processed.
     */
    return 1;
  }
  return 0;
}

// Per-thread active profiler, used by the SIGPROF handler. In Node's threading
// model each V8 isolate is pinned to a single thread, so a thread-local pointer
// is equivalent to "the profiler for the isolate that received this signal".
//
// This addon is dlopen'd, which complicates async-signal-safety of TLS:
// - On glibc, the default general-dynamic model compiles each access into a
//   __tls_get_addr call that can take loader locks. We force initial-exec so
//   each access is a single segment-relative load; glibc reserves a surplus
//   DTV slot pool for dlopen'd libraries to make this work.
// - On musl, all TLS for dlopen'd DSOs is pre-allocated at load time across
//   every live thread, so general-dynamic access is already lock-free and
//   signal-safe — and musl refuses initial-exec for dlopen'd objects, so we
//   must NOT request it there.
// - macOS and Windows use unrelated TLS mechanisms; neither needs the
//   attribute (and on Windows SIGPROF isn't even compiled in).
#if defined(__GLIBC__)
__attribute__((tls_model("initial-exec")))
#endif
thread_local WallProfiler* t_active_profiler = nullptr;

// Registry of all live profilers across threads. Used by the JS worker threads
// to publish CPU consumed by them, and by the main thread to gather that data
// while building a profile.
class ActiveProfilers {
 public:
  void Add(WallProfiler* profiler) {
    std::lock_guard<std::mutex> lock(mutex_);
    profilers_.insert(profiler);
  }

  bool Remove(WallProfiler* profiler) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = profilers_.find(profiler);
    if (it == profilers_.end()) return false;
    terminatedWorkersCpu_ += profiler->GetAndResetThreadCpu();
    profilers_.erase(it);
    return true;
  }

  ThreadCpuClock::duration GatherTotalWorkerCpuAndReset() {
    std::lock_guard<std::mutex> lock(mutex_);
    ThreadCpuClock::duration totalWorkerCpu = terminatedWorkersCpu_;
    terminatedWorkersCpu_ = ThreadCpuClock::duration::zero();
    for (auto* profiler : profilers_) {
      totalWorkerCpu += profiler->GetAndResetThreadCpu();
    }
    return totalWorkerCpu;
  }

 private:
  std::mutex mutex_;
  std::unordered_set<WallProfiler*> profilers_;
  ThreadCpuClock::duration terminatedWorkersCpu_{};
};

static ActiveProfilers g_profilers;

namespace {

#if DD_WALL_USE_SIGPROF
class SignalHandler {
 public:
  // Returns true on success; false if Install() failed, either because
  // sigaction itself failed or because the previous SIGPROF disposition was
  // already &HandleProfilerSignal (in which case we cannot chain to v8's
  // sampler and the caller should surface the failure rather than silently
  // produce empty profiles). On failure use_count_ is left unchanged, so
  // the caller must NOT balance with DecreaseUseCount().
  static bool IncreaseUseCount() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!Install()) {
      return false;
    }
    ++use_count_;
    return true;
  }

  static void DecreaseUseCount() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (--use_count_ == 0) {
      Restore();
    }
  }

  static bool Installed() {
    std::lock_guard<std::mutex> lock(mutex_);
    return installed_;
  }

 private:
  static bool Install() {
    struct sigaction sa;
    sa.sa_sigaction = &HandleProfilerSignal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_SIGINFO | SA_ONSTACK;
    if (installed_) {
      sigaction(SIGPROF, &sa, nullptr);
      return true;
    }
    struct sigaction prev;
    if (sigaction(SIGPROF, &sa, &prev) != 0) {
      return false;
    }
    if (prev.sa_sigaction == &HandleProfilerSignal) {
      // SIGPROF was already pointing at HandleProfilerSignal when we ran
      // the install (e.g. another SIGPROF user restored its own saved
      // disposition — which it had captured while we were the active
      // handler — after we Restore()'d). Chaining to prev would loop
      // forever on the next signal, so refuse. Sampling won't work in
      // this state because we can't reach v8's SIGPROF sampler.
      return false;
    }
    old_handler_ = prev;
    old_handler_func_.store(prev.sa_sigaction, std::memory_order_relaxed);
    installed_ = true;
    return true;
  }

  static void Restore() {
    if (installed_) {
      sigaction(SIGPROF, &old_handler_, nullptr);
      installed_ = false;
      old_handler_func_.store(nullptr, std::memory_order_relaxed);
    }
  }

  static void HandleProfilerSignal(int signal, siginfo_t* info, void* context);

  // Protects the process wide state below.
  static std::mutex mutex_;
  static int use_count_;
  static bool installed_;
  static struct sigaction old_handler_;
  using HandlerFunc = void (*)(int, siginfo_t*, void*);
  static std::atomic<HandlerFunc> old_handler_func_;
};

std::mutex SignalHandler::mutex_;
int SignalHandler::use_count_ = 0;
struct sigaction SignalHandler::old_handler_;
bool SignalHandler::installed_ = false;
std::atomic<SignalHandler::HandlerFunc> SignalHandler::old_handler_func_;

void SignalHandler::HandleProfilerSignal(int sig,
                                         siginfo_t* info,
                                         void* context) {
  auto old_handler = old_handler_func_.load(std::memory_order_relaxed);

  if (!old_handler) {
    return;
  }
  auto isolate = Isolate::GetCurrent();
  if (!isolate) {
    return;
  }
  WallProfiler* prof = t_active_profiler;

  if (!prof) {
    // no profiler active on this thread, just pass the signal to old handler
    old_handler(sig, info, context);
    return;
  }

  auto mode = prof->collectionMode();
  if (mode == WallProfiler::CollectionMode::kNoCollect) {
    return;
  } else if (mode == WallProfiler::CollectionMode::kPassThrough) {
    old_handler(sig, info, context);
    return;
  }

  int64_t cpu_time = 0;
  if (prof->collectCpuTime()) {
    cpu_time = CurrentThreadCpuClock::now().time_since_epoch().count();
  }
  auto time_from = Now();
  old_handler(sig, info, context);
  auto time_to = Now();
  prof->PushContext(time_from, time_to, cpu_time, isolate);
}
#else
class SignalHandler {
 public:
  static bool IncreaseUseCount() { return true; }
  static void DecreaseUseCount() {}
};
#endif
}  // namespace

static_assert((-1L >> 1) == -1L, "Right shift is not arithmetic");

static int64_t midpoint(int64_t x, int64_t y) {
  // TODO: remove when we're on C++20 as it has a built-in midpoint
  return ((x ^ y) >> 1) + (x & y);
}

static int64_t GetV8ToEpochOffset() {
  using namespace std::chrono;
  // Make a best effort to capture the difference between UNIX epoch and the V8
  // profiling timer as precisely as possible. Will make at most 20 attempts to
  // capture the epoch time within the same V8 microsecond and use the one with
  // the smallest error. We repeat this every time we gather a profile (so,
  // every minute) instead of once statically, as the difference doesn't
  // necessarily remain constant depending on the characteristics of the clocks
  // being used.
  int64_t V8toEpochOffset = 0;
  int64_t smallestDiff = std::numeric_limits<int64_t>::max();
  for (int i = 0; i < MAX_EPOCH_OFFSET_ATTEMPTS; ++i) {
    auto v8Now = Now();
    auto epochNow =
        duration_cast<microseconds>(system_clock::now().time_since_epoch())
            .count();
    auto v8Now2 = Now();
    auto diff = v8Now2 - v8Now;
    if (diff < smallestDiff) {
      V8toEpochOffset = epochNow - midpoint(v8Now, v8Now2);
      if (diff == 0) {
        break;
      }
      smallestDiff = diff;
    }
  }
  return V8toEpochOffset;
}

void WallProfiler::CleanupHook(void* data) {
  // Environment cleanup hooks run on the isolate's own thread, so the
  // thread-local pointer for this thread (if any) refers to the profiler that
  // belongs to this isolate.
  auto prof = t_active_profiler;
  if (prof) {
    auto isolate = static_cast<Isolate*>(data);
    prof->Cleanup(isolate);
    delete prof;
  }
}

// This is only called when isolate is terminated without `beforeExit`
// notification.
void WallProfiler::Cleanup(Isolate* isolate) {
  if (started_) {
    cpuProfiler_->Stop(profileId_);
    if (interceptSignal()) {
      SignalHandler::DecreaseUseCount();
    }
    Dispose(isolate);
  }
}

ContextsByNode WallProfiler::GetContextsByNode(CpuProfile* profile,
                                               ContextBuffer& contexts,
                                               int64_t startCpuTime) {
  ContextsByNode contextsByNode;

  auto sampleCount = profile->GetSamplesCount();
  if (contexts.empty() || sampleCount == 0) {
    return contextsByNode;
  }

  auto isolate = Isolate::GetCurrent();
  auto v8Context = isolate->GetCurrentContext();
  auto contextIt = contexts.begin();

  // deltaIdx is the offset of the sample to process compared to current
  // iteration index
  int deltaIdx = 0;

  auto contextKey = String::NewFromUtf8Literal(isolate, "context");
  auto timestampKey = String::NewFromUtf8Literal(isolate, "timestamp");
  auto cpuTimeKey = String::NewFromUtf8Literal(isolate, "cpuTime");
  auto asyncIdKey = String::NewFromUtf8Literal(isolate, "asyncId");
  auto V8toEpochOffset = GetV8ToEpochOffset();
  auto lastCpuTime = startCpuTime;

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
        auto it = contextsByNode.find(sample);
        Local<Array> array;
        if (it == contextsByNode.end()) {
          array = Array::New(isolate);
          contextsByNode[sample] = {array, 1};
        } else {
          array = it->second.contexts;
          ++it->second.hitcount;
        }
        // Conforms to TimeProfileNodeContext defined in v8-types.ts
        Local<Object> timedContext = Object::New(isolate);
        timedContext
            ->Set(v8Context,
                  timestampKey,
                  BigInt::New(isolate, sampleTimestamp + V8toEpochOffset))
            .Check();
        auto* function_name = sample->GetFunctionNameStr();
        // If current sample is program, reports its cpu time to the next sample
        if (strcmp(function_name, "(program)") != 0) {
          if (collectCpuTime_) {
            timedContext
                ->Set(
                    v8Context,
                    cpuTimeKey,
                    Number::New(isolate, sampleContext.cpu_time - lastCpuTime))
                .Check();
            lastCpuTime = sampleContext.cpu_time;
          }
          // If current sample is neither program nor idle, associate a sampling
          // context and async ID
          if (strcmp(function_name, "(idle)") != 0) {
            if (sampleContext.context) {
              timedContext
                  ->Set(v8Context,
                        contextKey,
                        sampleContext.context.get()->Get(isolate))
                  .Check();
            }
            if (collectAsyncId_) {
              timedContext
                  ->Set(v8Context,
                        asyncIdKey,
                        Number::New(isolate, sampleContext.async_id))
                  .Check();
            }
          }
        }
        array->Set(v8Context, array->Length(), timedContext).Check();

        // Sample context was consumed, fetch the next one
        ++contextIt;
        break;  // don't match more than one context to one sample
      }
    }
  }

  return contextsByNode;
}

void GCPrologueCallback(Isolate* isolate,
                        GCType type,
                        GCCallbackFlags flags,
                        void* data) {
  static_cast<WallProfiler*>(data)->OnGCStart(isolate);
}

void GCEpilogueCallback(Isolate* isolate,
                        GCType type,
                        GCCallbackFlags flags,
                        void* data) {
  static_cast<WallProfiler*>(data)->OnGCEnd();
}

WallProfiler::WallProfiler(std::chrono::microseconds samplingPeriod,
                           std::chrono::microseconds duration,
                           bool includeLines,
                           bool withContexts,
                           bool workaroundV8Bug,
                           bool collectCpuTime,
                           bool collectAsyncId,
                           bool isMainThread,
                           Local<Value> cpedKey)
    : samplingPeriod_(samplingPeriod),
      includeLines_(includeLines),
      withContexts_(withContexts),
      isMainThread_(isMainThread) {
  // Try to workaround V8 bug where profiler event processor loop becomes stuck.
  // When starting a new profile, wait for one signal before and one signal
  // after to reduce the likelihood that race condition occurs and one code
  // event just after triggers the issue.
  workaroundV8Bug_ = workaroundV8Bug && DD_WALL_USE_SIGPROF && detectV8Bug_;
  collectCpuTime_ = collectCpuTime && withContexts;
  collectAsyncId_ = collectAsyncId && withContexts;
#if DD_WALL_USE_CPED
  bool useCPED = withContexts && cpedKey->IsObject();
#else
  constexpr bool useCPED = false;
#endif

  if (withContexts_) {
    contexts_.reserve(duration * 2 / samplingPeriod);
  }

  collectionMode_.store(CollectionMode::kNoCollect, std::memory_order_relaxed);
  gcCount.store(0, std::memory_order_relaxed);

  // TODO: bind to this isolate? Would fix the Dispose(nullptr) issue.
  auto isolate = v8::Isolate::GetCurrent();
  v8::Local<v8::ArrayBuffer> buffer =
      v8::ArrayBuffer::New(isolate, sizeof(uint32_t) * kFieldCount);

  v8::Local<v8::Uint32Array> jsArray =
      v8::Uint32Array::New(buffer, 0, kFieldCount);
  fields_ = static_cast<uint32_t*>(buffer->GetBackingStore()->Data());
  jsArray_ = v8::Global<v8::Uint32Array>(isolate, jsArray);
  std::fill(fields_, fields_ + kFieldCount, 0);

  if (useCPED) {
    // Used to create Map value objects that will have one internal field to
    // store the sample context pointer.
    auto wrapObjectTemplate = ObjectTemplate::New(isolate);
    wrapObjectTemplate->SetInternalFieldCount(1);
    wrapObjectTemplate_.Reset(isolate, wrapObjectTemplate);
    auto cpedKeyObj = cpedKey.As<Object>();
    cpedKey_.Reset(isolate, cpedKeyObj);
    cpedKeyHash_ = cpedKeyObj->GetIdentityHash();
  }
}

WallProfiler::~WallProfiler() {
  // Defensive: under normal use Dispose() has already cleared both of these,
  // but if a profiler is destroyed without going through Dispose we still must
  // not leave dangling pointers.
  if (t_active_profiler == this) {
    t_active_profiler = nullptr;
  }
  g_profilers.Remove(this);

  // Delete every PCP still live in the CPED map. ~PCP would normally unlink
  // itself via pprev_/next_, but we're tearing down the list we point into —
  // so null pprev_ first to signal "already detached" and let ~PCP skip the
  // unlink. (~ObjectWrap will still clear V8's weak callback during delete,
  // so the dangling internal-field pointer in the wrap object stays inert
  // even if V8 later GCs the wrap.)
  auto* p = liveContextPtrHead_;
  while (p != nullptr) {
    auto* next = p->next_;
    p->pprev_ = nullptr;
    p->next_ = nullptr;
    delete p;
    p = next;
  }
  liveContextPtrHead_ = nullptr;
}

void WallProfiler::Dispose(Isolate* isolate) {
  if (cpuProfiler_ != nullptr) {
    cpuProfiler_->Dispose();
    cpuProfiler_ = nullptr;

    g_profilers.Remove(this);
    if (t_active_profiler == this) {
      t_active_profiler = nullptr;
    }

    if (collectAsyncId_ || useCPED()) {
      isolate->RemoveGCPrologueCallback(&GCPrologueCallback, this);
      isolate->RemoveGCEpilogueCallback(&GCEpilogueCallback, this);
    }

    node::RemoveEnvironmentCleanupHook(
        isolate, &WallProfiler::CleanupHook, isolate);
  }
}

#define DD_WALL_PROFILER_GET_BOOLEAN_CONFIG(name)                              \
  auto name##Value = getArg(#name);                                            \
  if (name##Value.IsEmpty() || !name##Value.ToLocalChecked()->IsBoolean()) {   \
    return Nan::ThrowTypeError(#name " must be a boolean.");                   \
  }                                                                            \
  bool name = name##Value.ToLocalChecked().As<v8::Boolean>()->Value();

NAN_METHOD(WallProfiler::New) {
  if (info.Length() != 1 || !info[0]->IsObject()) {
    return Nan::ThrowTypeError("WallProfiler must have one object argument.");
  }

  if (info.IsConstructCall()) {
    auto arg = info[0].As<v8::Object>();
    auto isolate = info.GetIsolate();
    auto context = isolate->GetCurrentContext();
    auto getArg = [&](const char* name) {
      return arg->Get(context,
                      String::NewFromUtf8(isolate, name).ToLocalChecked());
    };

    auto intervalMicrosValue = getArg("intervalMicros");
    if (intervalMicrosValue.IsEmpty() ||
        !intervalMicrosValue.ToLocalChecked()->IsNumber()) {
      return Nan::ThrowTypeError("intervalMicros must be a number.");
    }

    std::chrono::microseconds interval{
        intervalMicrosValue.ToLocalChecked().As<v8::Integer>()->Value()};

    if (interval <= std::chrono::microseconds::zero()) {
      return Nan::ThrowTypeError("Sample rate must be positive.");
    }

    auto durationMillisValue = getArg("durationMillis");
    if (durationMillisValue.IsEmpty() ||
        !durationMillisValue.ToLocalChecked()->IsNumber()) {
      return Nan::ThrowTypeError("durationMillis must be a number.");
    }

    std::chrono::milliseconds duration{
        durationMillisValue.ToLocalChecked().As<v8::Integer>()->Value()};

    if (duration <= std::chrono::microseconds::zero()) {
      return Nan::ThrowTypeError("Duration must be positive.");
    }
    if (duration < interval) {
      return Nan::ThrowTypeError("Duration must not be less than sample rate.");
    }

    DD_WALL_PROFILER_GET_BOOLEAN_CONFIG(lineNumbers);
    DD_WALL_PROFILER_GET_BOOLEAN_CONFIG(withContexts);
    DD_WALL_PROFILER_GET_BOOLEAN_CONFIG(workaroundV8Bug);
    DD_WALL_PROFILER_GET_BOOLEAN_CONFIG(collectCpuTime);
    DD_WALL_PROFILER_GET_BOOLEAN_CONFIG(collectAsyncId);
    DD_WALL_PROFILER_GET_BOOLEAN_CONFIG(isMainThread);
    DD_WALL_PROFILER_GET_BOOLEAN_CONFIG(useCPED);

    auto cpedKey = getArg("CPEDKey").ToLocalChecked();
    if (cpedKey->IsObject() && !useCPED) {
      return Nan::ThrowTypeError("useCPED is false but CPEDKey is specified");
    }
    if (useCPED && cpedKey->IsUndefined()) {
      cpedKey = Object::New(isolate);
    }
#if !DD_WALL_USE_CPED
    if (useCPED) {
      return Nan::ThrowTypeError(
#ifndef _WIN32
          "useCPED is not supported on this Node.js version."
#else
          "useCPED is not supported on Windows."
#endif
      );
    }
#endif

    if (withContexts && !DD_WALL_USE_SIGPROF) {
      return Nan::ThrowTypeError("Contexts are not supported.");
    }

    if (collectCpuTime && !withContexts) {
      return Nan::ThrowTypeError("Cpu time collection requires contexts.");
    }

    if (collectAsyncId && !withContexts) {
      return Nan::ThrowTypeError("Async ID collection requires contexts.");
    }

    if (lineNumbers && withContexts) {
      // Currently custom contexts are not compatible with caller line
      // information, because it's not possible to associate context with line
      // ticks:
      // context is associated to sample which itself is associated with
      // a CpuProfileNode, but if node has several line ticks, then we cannot
      // determine context <-> line ticks association. Note that line number is
      // present in v8 internal sample struct and would allow mapping sample to
      // line tick, and thus context to line tick, but this information is not
      // available in v8 public API.
      // Moreover in caller line number mode, line number of a CpuProfileNode
      // is not the line of the current function, but the line number where this
      // function is called, therefore we don't have access either to the line
      // of the function (otherwise we could ignore line ticks and replace them
      // with single hit count for the function).
      return Nan::ThrowTypeError(
          "Include line option is not compatible with contexts.");
    }

    WallProfiler* obj = new WallProfiler(interval,
                                         duration,
                                         lineNumbers,
                                         withContexts,
                                         workaroundV8Bug,
                                         collectCpuTime,
                                         collectAsyncId,
                                         isMainThread,
                                         cpedKey);
    obj->Wrap(info.This());
    info.GetReturnValue().Set(info.This());
  } else {
    v8::Local<v8::Value> arg = info[0];
    v8::Local<v8::Function> cons = Nan::New(
        PerIsolateData::For(info.GetIsolate())->WallProfilerConstructor());
    info.GetReturnValue().Set(Nan::NewInstance(cons, 1, &arg).ToLocalChecked());
  }
}

#undef DD_WALL_PROFILER_GET_BOOLEAN_CONFIG

NAN_METHOD(WallProfiler::Start) {
  WallProfiler* wallProfiler =
      Nan::ObjectWrap::Unwrap<WallProfiler>(info.This());

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

  // Register GC callbacks for async ID and CPED context tracking before
  // starting profiling
  auto isolate = Isolate::GetCurrent();
  if (collectAsyncId_ || useCPED()) {
    isolate->AddGCPrologueCallback(&GCPrologueCallback, this);
    isolate->AddGCEpilogueCallback(&GCEpilogueCallback, this);
  }

  if (auto res = StartInternal(); !res.success) {
    // StartInternal may have left v8 cpu profiling running and (if
    // CreateV8CpuProfiler succeeded) we hold a cpuProfiler_, a g_profilers
    // entry, and possibly GC pro/epilogue callbacks. Tear them all down so
    // the caller doesn't observe a half-initialized profiler.
    Dispose(isolate);
    return res;
  }

  auto collectionMode = withContexts_
                            ? CollectionMode::kCollectContexts
                            : (workaroundV8Bug_ ? CollectionMode::kPassThrough
                                                : CollectionMode::kNoCollect);
  collectionMode_.store(collectionMode, std::memory_order_relaxed);
  started_ = true;
  node::AddEnvironmentCleanupHook(isolate, &WallProfiler::CleanupHook, isolate);
  return {};
}

Result WallProfiler::StartInternal() {
  // Reuse the same names for the profiles because strings used for profile
  // names are not released until v8::CpuProfiler object is destroyed.
  // https://github.com/nodejs/node/blob/b53c51995380b1f8d642297d848cab6010d2909c/deps/v8/src/profiler/profile-generator.h#L516
  char buf[128];
  snprintf(buf, sizeof(buf), "pprof-%" PRId64, (profileIdx_++) % 2);
  v8::Local<v8::String> title = Nan::New<String>(buf).ToLocalChecked();
  auto result = cpuProfiler_->Start(
      title,
      includeLines_ ? CpuProfilingMode::kCallerLineNumbers
                    : CpuProfilingMode::kLeafNodeLineNumbers,
      // Always record samples in order to be able to check if non tick samples
      // (ie. starting or deopt samples) have been processed, and therefore if
      // SamplingEventsProcessor::ProcessOneSample is stuck on vm_ticks_buffer_.
      withContexts_ || detectV8Bug_);
  profileId_ = result.id;

  // reinstall sighandler on each new upload period
  if (withContexts_ || workaroundV8Bug_) {
    if (!SignalHandler::IncreaseUseCount()) {
      // SIGPROF was already pointing at HandleProfilerSignal at install
      // time (with installed_ == false), so chaining would loop, or
      // sigaction itself failed. Without a working chain, v8's sampler
      // can't fire and we'd silently produce empty profiles — surface a
      // startup error so the caller can roll back. The just-started v8
      // cpu profile is left running; the caller is expected to Dispose
      // the cpuProfiler_, which stops all running profiles.
      return Result{
          "Cannot start wall profiler: failed to install SIGPROF handler. "
          "This typically means more than one copy of @datadog/pprof is "
          "loaded in the process."};
    }
    fields_[kSampleCount] = 0;
  }

  if (collectCpuTime_) {
    startThreadCpuTime_ =
        CurrentThreadCpuClock::now().time_since_epoch().count();
    startProcessCpuTime_ = ProcessCpuClock::now();
  }

  // Force collection of two other non-tick samples (ie. that will not add to
  // hit count).
  // This is to be able to detect when v8 profiler event processor loop is
  // stuck on ticks_from_vm_buffer_.
  // A non-tick sample is already taken upon profiling start, and should be
  // enough to determine if a non-tick sample has been processed at the end by
  // comparing number of samples with total hit count.
  // The first tick sample might be discarded though if its timestamp is older
  // than profile start time due to queueing and in that case it is still added
  // to hit count but not to the sample array, leading to incorrectly detect
  // that ticks_from_vm_buffer_ is stuck.
  // This is not needed when workaroundV8Bug_ is enabled because in that case,
  // we wait for one signal before starting a new profile which should leave
  // time to process in-flight tick samples.
  if (detectV8Bug_ && !workaroundV8Bug_) {
    cpuProfiler_->CollectSample(v8::Isolate::GetCurrent());
    cpuProfiler_->CollectSample(v8::Isolate::GetCurrent());
  }

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
      Nan::ObjectWrap::Unwrap<WallProfiler>(info.This());

  v8::Local<v8::Value> profile;
  auto err = wallProfiler->StopImpl(restart, profile);

  if (!err.success) {
    return Nan::ThrowTypeError(err.msg.c_str());
  }
  info.GetReturnValue().Set(profile);
}

// stopAndCollect(restart, callback): callback result
NAN_METHOD(WallProfiler::StopAndCollect) {
  if (info.Length() != 2) {
    return Nan::ThrowTypeError("stopAndCollect must have two arguments.");
  }
  if (!info[0]->IsBoolean()) {
    return Nan::ThrowTypeError("Restart must be a boolean.");
  }
  if (!info[1]->IsFunction()) {
    return Nan::ThrowTypeError("stopAndCollect requires a callback.");
  }

  bool restart = info[0].As<Boolean>()->Value();
  auto callback = info[1].As<Function>();

  WallProfiler* wallProfiler =
      Nan::ObjectWrap::Unwrap<WallProfiler>(info.This());

  v8::Local<v8::Value> result;
  auto err = wallProfiler->StopAndCollectImpl(restart, callback, result);
  if (!err.success) {
    return Nan::ThrowTypeError(err.msg.c_str());
  }
  if (!result.IsEmpty()) {
    info.GetReturnValue().Set(result);
  }
}

bool WallProfiler::waitForSignal(uint64_t targetCallCount) {
  auto currentCallCount = noCollectCallCount_.load(std::memory_order_relaxed);
  std::atomic_signal_fence(std::memory_order_acquire);
  if (targetCallCount != 0) {
    // check if target call count already reached
    if (currentCallCount >= targetCallCount) {
      return true;
    }
  } else {
    // no target call count in input, wait for the next signal
    targetCallCount = currentCallCount + 1;
  }
#if DD_WALL_USE_SIGPROF
  const int maxRetries = 2;
  // wait for a maximum of 2 sample periods
  // if a signal occurs it will interrupt sleep (we use nanosleep and not
  // uv_sleep because we want this behaviour)
  timespec ts = {
      0, std::chrono::nanoseconds(samplingPeriod_ * maxRetries).count()};
  nanosleep(&ts, nullptr);
#endif
  auto res = noCollectCallCount_.load(std::memory_order_relaxed);
  std::atomic_signal_fence(std::memory_order_acquire);
  return res >= targetCallCount;
}

template <typename ProfileBuilder>
Result WallProfiler::StopCore(bool restart, ProfileBuilder&& buildProfile) {
  if (!started_) {
    return Result{"Stop called on not started profiler."};
  }

  uint64_t callCount = 0;
  auto oldProfileId = profileId_;
  if (restart && workaroundV8Bug_) {
    std::atomic_signal_fence(std::memory_order_release);
    collectionMode_.store(CollectionMode::kNoCollect,
                          std::memory_order_relaxed);
    waitForSignal();
  } else if (withContexts_) {
    std::atomic_signal_fence(std::memory_order_release);
    collectionMode_.store(CollectionMode::kNoCollect,
                          std::memory_order_relaxed);

    // make sure timestamp changes to avoid having samples from previous profile
    auto now = Now();
    while (Now() == now) {
    }
  }

  auto startThreadCpuTime = startThreadCpuTime_;
  auto startProcessCpuTime = startProcessCpuTime_;

  if (restart) {
    // In restart mode the signal handler is already installed (use_count_
    // stayed positive across the cycle), so the install can never go through
    // the else-branch in Install() and IncreaseUseCount() can't fail.
    StartInternal();
    // record call count to wait for next signal at the end of function
    callCount = noCollectCallCount_.load(std::memory_order_relaxed);
    std::atomic_signal_fence(std::memory_order_acquire);
  }

  if (interceptSignal()) {
    SignalHandler::DecreaseUseCount();
  }

  auto v8_profile = cpuProfiler_->Stop(oldProfileId);

  ContextBuffer contexts;
  if (withContexts_) {
    contexts.reserve(contexts_.capacity());
    std::swap(contexts, contexts_);
  }

  if (detectV8Bug_) {
    v8ProfilerStuckEventLoopDetected_ = detectV8Bug(v8_profile);
  }

  if (restart && withContexts_ && !workaroundV8Bug_) {
    // make sure timestamp changes to avoid mixing sample taken upon start and a
    // sample from signal handler
    // If v8 bug workaround is enabled, reactivation of sample collection is
    // delayed until function end.
    auto now = Now();
    while (Now() == now) {
    }
    std::atomic_signal_fence(std::memory_order_release);
    collectionMode_.store(CollectionMode::kCollectContexts,
                          std::memory_order_relaxed);
  }

  ContextsByNode contextsByNode;
  ContextsByNode* contextsByNodePtr = nullptr;
  int64_t nonJSThreadsCpuTime = 0;
  bool hasCpuTime = false;

  if (withContexts_) {
    if (isMainThread_ && collectCpuTime_) {
      // account for non-JS threads CPU only in main thread
      // CPU time of non-JS threads is the difference between process CPU time
      // and sum of all worker JS thread during the profiling period of main
      // worker thread.
      auto totalWorkerCpu = g_profilers.GatherTotalWorkerCpuAndReset();
      auto processCpu = ProcessCpuClock::now() - startProcessCpuTime;
      nonJSThreadsCpuTime =
          std::max(processCpu - totalWorkerCpu, ProcessCpuClock::duration{})
              .count();
    }
    contextsByNode =
        GetContextsByNode(v8_profile, contexts, startThreadCpuTime);
    contextsByNodePtr = &contextsByNode;
    hasCpuTime = collectCpuTime_;
  }

  buildProfile(v8_profile, hasCpuTime, nonJSThreadsCpuTime, contextsByNodePtr);

  v8_profile->Delete();

  if (!restart) {
    Dispose(v8::Isolate::GetCurrent());
  } else if (workaroundV8Bug_) {
    waitForSignal(callCount + 1);
    std::atomic_signal_fence(std::memory_order_release);
    collectionMode_.store(withContexts_ ? CollectionMode::kCollectContexts
                                        : CollectionMode::kPassThrough,
                          std::memory_order_relaxed);
  }

  started_ = restart;
  return {};
}

Result WallProfiler::StopImpl(bool restart, v8::Local<v8::Value>& profile) {
  return StopCore(restart,
                  [&](const v8::CpuProfile* v8_profile,
                      bool hasCpuTime,
                      int64_t nonJSThreadsCpuTime,
                      ContextsByNode* contextsByNodePtr) {
                    profile = TranslateTimeProfile(v8_profile,
                                                   includeLines_,
                                                   contextsByNodePtr,
                                                   hasCpuTime,
                                                   nonJSThreadsCpuTime);
                  });
}

Result WallProfiler::StopAndCollectImpl(bool restart,
                                        v8::Local<v8::Function> callback,
                                        v8::Local<v8::Value>& result) {
  return StopCore(
      restart,
      [&](const v8::CpuProfile* v8_profile,
          bool hasCpuTime,
          int64_t nonJSThreadsCpuTime,
          ContextsByNode* contextsByNodePtr) {
        auto* isolate = Isolate::GetCurrent();
        TimeProfileViewState state{includeLines_, contextsByNodePtr, {}};
        auto profile_view = BuildTimeProfileView(
            v8_profile, hasCpuTime, nonJSThreadsCpuTime, state);
        v8::Local<v8::Value> argv[] = {profile_view};
        auto cb_result = Nan::Call(
            callback, isolate->GetCurrentContext()->Global(), 1, argv);
        if (!cb_result.IsEmpty()) {
          result = cb_result.ToLocalChecked();
        }
      });
}

NAN_MODULE_INIT(WallProfiler::Init) {
  Local<FunctionTemplate> tpl = Nan::New<FunctionTemplate>(New);
  Local<String> className = Nan::New("TimeProfiler").ToLocalChecked();
  tpl->SetClassName(className);
  tpl->InstanceTemplate()->SetInternalFieldCount(1);

  Nan::SetAccessor(tpl->InstanceTemplate(),
                   Nan::New("context").ToLocalChecked(),
                   GetContext,
                   SetContext);

  Nan::SetPrototypeMethod(tpl, "start", Start);
  Nan::SetPrototypeMethod(tpl, "stop", Stop);
  Nan::SetPrototypeMethod(tpl, "stopAndCollect", StopAndCollect);
  Nan::SetPrototypeMethod(tpl, "dispose", Dispose);
  Nan::SetPrototypeMethod(tpl,
                          "v8ProfilerStuckEventLoopDetected",
                          V8ProfilerStuckEventLoopDetected);
  Nan::SetPrototypeMethod(tpl, "createContextHolder", CreateContextHolder);

  Nan::SetAccessor(tpl->InstanceTemplate(),
                   Nan::New("state").ToLocalChecked(),
                   SharedArrayGetter);

  Nan::SetAccessor(tpl->InstanceTemplate(),
                   Nan::New("metrics").ToLocalChecked(),
                   GetMetrics);

  PerIsolateData::For(Isolate::GetCurrent())
      ->WallProfilerConstructor()
      .Reset(Nan::GetFunction(tpl).ToLocalChecked());
  Nan::Set(target, className, Nan::GetFunction(tpl).ToLocalChecked());

  auto isolate = v8::Isolate::GetCurrent();
  v8::PropertyAttribute ReadOnlyDontDelete =
      static_cast<v8::PropertyAttribute>(ReadOnly | DontDelete);

  v8::Local<Object> constants = v8::Object::New(isolate);
  Nan::DefineOwnProperty(constants,
                         Nan::New("kSampleCount").ToLocalChecked(),
                         Nan::New<Integer>(kSampleCount),
                         ReadOnlyDontDelete)
      .FromJust();
  Nan::DefineOwnProperty(target,
                         Nan::New("constants").ToLocalChecked(),
                         constants,
                         ReadOnlyDontDelete)
      .FromJust();
}

v8::CpuProfiler* WallProfiler::CreateV8CpuProfiler() {
  if (cpuProfiler_ == nullptr) {
    if (t_active_profiler != nullptr) {
      // refuse to create a new profiler if one is already active on this thread
      return nullptr;
    }
    v8::Isolate* isolate = v8::Isolate::GetCurrent();
    g_profilers.Add(this);
    t_active_profiler = this;
    cpuProfiler_ = v8::CpuProfiler::New(isolate);
    cpuProfiler_->SetSamplingInterval(
        std::chrono::microseconds(samplingPeriod_).count());
  }
  return cpuProfiler_;
}

Local<Value> WallProfiler::GetContext(Isolate* isolate) {
  auto context = GetContextPtr(isolate);
  if (context) {
    return context->Get(isolate);
  }
  return Undefined(isolate);
}

void WallProfiler::SetCurrentContextPtr(Isolate* isolate, Local<Value> value) {
  SignalGuard m(setInProgress_);
  SetContextPtr(curContext_, isolate, value);
}

void WallProfiler::SetContext(Isolate* isolate, Local<Value> value) {
#if DD_WALL_USE_CPED
  if (!useCPED()) {
    SetCurrentContextPtr(isolate, value);
    return;
  }

  auto cped = isolate->GetContinuationPreservedEmbedderData();
  // No Node AsyncContextFrame in this continuation yet
  if (!cped->IsMap()) return;

  auto v8Ctx = isolate->GetCurrentContext();
  // This should always be called from a V8 context, but check just in case.
  if (v8Ctx.IsEmpty()) return;

  auto cpedMap = cped.As<Map>();
  auto localKey = cpedKey_.Get(isolate);

  // Always replace the PersistentContextPtr in the map even if it is present,
  // we want the PersistentContextPtr in a parent map to not be mutated.
  if (value->IsUndefined()) {
    // The absence of a sample context will be interpreted as undefined in
    // GetContextPtr so if value is undefined, just delete the key.
    SignalGuard m(setInProgress_);
    cpedMap->Delete(v8Ctx, localKey).Check();
  } else {
    auto contextHolder = CreateContextHolder(isolate, v8Ctx, value);
    SignalGuard m(setInProgress_);
    cpedMap->Set(v8Ctx, localKey, contextHolder).ToLocalChecked();
  }
#else
  SetCurrentContextPtr(isolate, value);
#endif
}

Local<Object> WallProfiler::CreateContextHolder(Isolate* isolate,
                                                Local<Context> v8Ctx,
                                                Local<Value> value) {
  auto wrap =
      wrapObjectTemplate_.Get(isolate)->NewInstance(v8Ctx).ToLocalChecked();
  // for easy access from JS when cpedKey is an ALS, it can do
  // als.getStore()?.[0];
  wrap->Set(v8Ctx, 0, value).Check();
  auto contextPtr = new PersistentContextPtr(this, wrap);
  contextPtr->Set(isolate, value);
  return wrap;
}

ContextPtr WallProfiler::GetContextPtrSignalSafe(Isolate* isolate) {
  auto isSetInProgress = setInProgress_.load(std::memory_order_relaxed);
  std::atomic_signal_fence(std::memory_order_acquire);
  if (isSetInProgress) {
    // New sample context is being set. Safe behavior is to not try attempt
    // Object::Get on it and just return empty right now.
    return ContextPtr();
  }

  if (useCPED()) {
    auto curGcCount = gcCount.load(std::memory_order_relaxed);
    std::atomic_signal_fence(std::memory_order_acquire);
    if (curGcCount > 0) {
      return gcContext_;
    }
  }

  return GetContextPtr(isolate);
}

ContextPtr WallProfiler::GetContextPtr(Isolate* isolate) {
#if DD_WALL_USE_CPED
  if (!useCPED()) {
    return curContext_;
  }

  if (!isolate->IsInUse()) {
    return ContextPtr();
  }

  auto cpedAddrPtr = reinterpret_cast<internal::Address*>(
      reinterpret_cast<uint64_t>(isolate) +
      internal::Internals::kContinuationPreservedEmbedderDataOffset);
  auto cpedAddr = *cpedAddrPtr;
  if (internal::Internals::HasHeapObjectTag(cpedAddr)) {
    auto cpedValuePtr = reinterpret_cast<Value*>(cpedAddrPtr);
    if (cpedValuePtr->IsMap()) {
      Address keyAddr = **(reinterpret_cast<Address**>(&cpedKey_));

      Address wrapAddr = GetValueFromMap(cpedAddr, cpedKeyHash_, keyAddr);
      if (internal::Internals::HasHeapObjectTag(wrapAddr)) {
        auto wrapValue = reinterpret_cast<Value*>(&wrapAddr);
        if (wrapValue->IsObject()) {
          auto wrapObj = reinterpret_cast<Object*>(wrapValue);
          if (wrapObj->InternalFieldCount() > 0) {
            return static_cast<PersistentContextPtr*>(
                       GetAlignedPointerFromInternalField(wrapObj, 0))
                ->Get();
          }
        }
      }
    }
  }
  return ContextPtr();
#else
  return curContext_;
#endif
}

Local<Object> WallProfiler::GetMetrics(Isolate* isolate) {
  auto usedAsyncContextCount = liveContextPtrCount();
  auto context = isolate->GetCurrentContext();
  auto metrics = Object::New(isolate);
  metrics
      ->Set(context,
            String::NewFromUtf8Literal(isolate, "usedAsyncContextCount"),
            Number::New(isolate, usedAsyncContextCount))
      .ToChecked();
  metrics
      ->Set(context,
            String::NewFromUtf8Literal(isolate, "totalAsyncContextCount"),
            Number::New(isolate, usedAsyncContextCount))
      .ToChecked();
  return metrics;
}

NAN_GETTER(WallProfiler::GetContext) {
  auto profiler = Nan::ObjectWrap::Unwrap<WallProfiler>(info.This());
  info.GetReturnValue().Set(profiler->GetContext(info.GetIsolate()));
}

NAN_SETTER(WallProfiler::SetContext) {
  auto profiler = Nan::ObjectWrap::Unwrap<WallProfiler>(info.This());
  profiler->SetContext(info.GetIsolate(), value);
}

NAN_METHOD(WallProfiler::CreateContextHolder) {
  auto profiler = Nan::ObjectWrap::Unwrap<WallProfiler>(info.This());
  if (!profiler->useCPED()) {
    return Nan::ThrowTypeError(
        "CreateContextHolder can only be used with CPED");
  }
  auto isolate = info.GetIsolate();
  auto contextHolder = profiler->CreateContextHolder(
      isolate, isolate->GetCurrentContext(), info[0]);
  info.GetReturnValue().Set(contextHolder);
}

NAN_GETTER(WallProfiler::SharedArrayGetter) {
  auto profiler = Nan::ObjectWrap::Unwrap<WallProfiler>(info.This());
  info.GetReturnValue().Set(profiler->jsArray_.Get(v8::Isolate::GetCurrent()));
}

NAN_GETTER(WallProfiler::GetMetrics) {
  auto profiler = Nan::ObjectWrap::Unwrap<WallProfiler>(info.This());
  info.GetReturnValue().Set(profiler->GetMetrics(info.GetIsolate()));
}

NAN_METHOD(WallProfiler::V8ProfilerStuckEventLoopDetected) {
  auto profiler = Nan::ObjectWrap::Unwrap<WallProfiler>(info.This());
  info.GetReturnValue().Set(profiler->v8ProfilerStuckEventLoopDetected());
}

NAN_METHOD(WallProfiler::Dispose) {
  auto profiler = Nan::ObjectWrap::Unwrap<WallProfiler>(info.This());
  // Profiler must already be stopped when this is called.
  if (profiler->started_) {
    return Nan::ThrowTypeError("Profiler is still running, stop it first.");
  }
  delete profiler;
}

#if NODE_MAJOR_VERSION >= 24 && DD_WALL_USE_SIGPROF
// Returns the number of free Address slots for Locals that can be returned by
// the isolate without triggering memory allocation.
int GetFreeLocalSlotCount(Isolate* isolate) {
  v8::internal::HandleScopeData* data =
      reinterpret_cast<v8::internal::HandleScopeData*>(
          reinterpret_cast<uint64_t>(isolate) +
          v8::internal::Internals::kIsolateHandleScopeDataOffset);
  auto diff = data->limit - data->next;
  // sanity check: diff can be at most kHandleBlockSize. If it is larger,
  // something is suspicious. See
  // https://github.com/v8/v8/blob/6fcfeccda2d8bcb7397f89bf5bbacd0c2eb2fb7f/src/handles/handles.cc#L195
  return diff > v8::internal::kHandleBlockSize ? 0 : diff;
}
#endif

double GetAsyncIdNoGC(v8::Isolate* isolate) {
  if (!isolate->IsInUse()) {
    // Must not try to create a handle scope if isolate is not in use.
    return -1;
  }
#if NODE_MAJOR_VERSION >= 24 && DD_WALL_USE_SIGPROF
  if (GetFreeLocalSlotCount(isolate) < 1) {
    // Must not try to create a handle scope if we can't create one local handle
    // (return value of GetEnteredOrMicrotaskContext) without allocation.
    return -1;
  }
  HandleScope scope(isolate);
  auto context = isolate->GetEnteredOrMicrotaskContext();
  return context.IsEmpty() ? -1 : node::AsyncHooksGetExecutionAsyncId(context);
#else
  return node::AsyncHooksGetExecutionAsyncId(isolate);
#endif
}

double WallProfiler::GetAsyncId(v8::Isolate* isolate) {
  if (!collectAsyncId_) {
    return -1;
  }
  auto curGcCount = gcCount.load(std::memory_order_relaxed);
  std::atomic_signal_fence(std::memory_order_acquire);
  if (curGcCount > 0) {
    return gcAsyncId;
  }
  return GetAsyncIdNoGC(isolate);
}

void WallProfiler::OnGCStart(v8::Isolate* isolate) {
  auto curCount = gcCount.load(std::memory_order_relaxed);
  std::atomic_signal_fence(std::memory_order_acquire);
  if (curCount == 0) {
    if (collectAsyncId_) {
      gcAsyncId = GetAsyncIdNoGC(isolate);
    }
    if (useCPED()) {
      gcContext_ = GetContextPtrSignalSafe(isolate);
    }
  }
  std::atomic_signal_fence(std::memory_order_release);
  gcCount.store(curCount + 1, std::memory_order_relaxed);
}

void WallProfiler::OnGCEnd() {
  auto oldCount = gcCount.fetch_sub(1, std::memory_order_relaxed);
  if (oldCount != 1 || !useCPED()) {
    return;
  }
  // Not strictly necessary, as we'll reset it to something else on next GC,
  // but why retain it longer than needed?
  gcContext_.reset();
}

void WallProfiler::PushContext(int64_t time_from,
                               int64_t time_to,
                               int64_t cpu_time,
                               Isolate* isolate) {
  // Be careful this is called in a signal handler context therefore all
  // operations must be async signal safe (in particular no allocations).
  // Our ring buffer avoids allocations.
  if (contexts_.size() < contexts_.capacity()) {
    contexts_.push_back({GetContextPtrSignalSafe(isolate),
                         time_from,
                         time_to,
                         cpu_time,
                         GetAsyncId(isolate)});
    std::atomic_fetch_add_explicit(
        reinterpret_cast<std::atomic<uint32_t>*>(&fields_[kSampleCount]),
        1U,
        std::memory_order_relaxed);
  }
}

}  // namespace dd
