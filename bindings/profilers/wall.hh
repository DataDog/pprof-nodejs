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

#pragma once

#include "contexts.hh"
#include "thread-cpu-clock.hh"

#include <nan.h>
#include <v8-profiler.h>
#include <atomic>
#include <memory>
#include <unordered_map>
#include <utility>

namespace dd {

struct Result {
  Result() = default;
  explicit Result(const char* msg) : success{false}, msg{msg} {};

  bool success = true;
  std::string msg;
};

using ContextPtr = std::shared_ptr<v8::Global<v8::Value>>;

/**
 * Class that allows atomic updates to a ContextPtr. Since update of shared_ptr
 * is not atomic, we use a pointer that alternates between pointing to one of
 * two shared_ptrs instead, and we use atomic operations to update the pointer.
 */
class AtomicContextPtr {
  ContextPtr ptr1;
  ContextPtr ptr2;
  std::atomic<ContextPtr*> currentPtr = &ptr1;

  void Set(v8::Isolate* isolate, v8::Local<v8::Value> value) {
    auto oldPtr = currentPtr.load(std::memory_order_relaxed);
    std::atomic_signal_fence(std::memory_order_acquire);
    auto newPtr = oldPtr == &ptr1 ? &ptr2 : &ptr1;
    if (!value->IsNullOrUndefined()) {
      *newPtr = std::make_shared<v8::Global<v8::Value>>(isolate, value);
    } else {
      newPtr->reset();
    }
    std::atomic_signal_fence(std::memory_order_release);
    currentPtr.store(newPtr, std::memory_order_relaxed);
    std::atomic_signal_fence(std::memory_order_release);
  }

  ContextPtr Get() {
    auto ptr = currentPtr.load(std::memory_order_relaxed);
    std::atomic_signal_fence(std::memory_order_acquire);
    return ptr ? *ptr : ContextPtr();
  }

  friend class WallProfiler;
};

class PersistentContextPtr;

class WallProfiler : public Nan::ObjectWrap {
 public:
  enum class CollectionMode { kNoCollect, kPassThrough, kCollectContexts };

 private:
  enum Fields { kSampleCount, kFieldCount };

  std::chrono::microseconds samplingPeriod_{0};
  v8::CpuProfiler* cpuProfiler_ = nullptr;

  bool useCPED_ = false;
  // If we aren't using the CPED, we use a single context ptr stored here.
  AtomicContextPtr curContext_;
  // Otherwise we'll use a private symbol to store the context in CPED objects.
  v8::Global<v8::Private> cpedSymbol_;
  // We track live context pointers in a set to avoid memory leaks. They will
  // be deleted when the profiler is disposed.
  std::unordered_set<PersistentContextPtr*> liveContextPtrs_;
  // Context pointers belonging to GC'd CPED objects register themselves here.
  // They will be deleted and removed from liveContextPtrs_ next time SetContext
  // is invoked.
  std::vector<PersistentContextPtr*> deadContextPtrs_;

  std::atomic<int> gcCount = 0;
  std::atomic<bool> setInProgress = false;
  double gcAsyncId;
  ContextPtr gcContext;

  std::atomic<CollectionMode> collectionMode_;
  std::atomic<uint64_t> noCollectCallCount_;
  v8::ProfilerId profileId_;
  uint64_t profileIdx_ = 0;
  bool includeLines_ = false;
  bool withContexts_ = false;
  bool started_ = false;
  bool workaroundV8Bug_;
  static inline constexpr bool detectV8Bug_ = true;
  bool collectCpuTime_;
  bool collectAsyncId_;
  bool isMainThread_;
  int v8ProfilerStuckEventLoopDetected_ = 0;
  ProcessCpuClock::time_point startProcessCpuTime_{};
  int64_t startThreadCpuTime_ = 0;
  /* threadCpuStopWatch_ is used to measure CPU consumed by JS thread owning the
   * WallProfiler object during profiling period of main worker thread. */
  ThreadCpuStopWatch threadCpuStopWatch_;
  uint32_t* fields_;
  v8::Global<v8::Uint32Array> jsArray_;

  struct SampleContext {
    ContextPtr context;
    int64_t time_from;
    int64_t time_to;
    int64_t cpu_time;
    double async_id;
  };

  using ContextBuffer = std::vector<SampleContext>;
  ContextBuffer contexts_;

  ~WallProfiler() = default;
  void Dispose(v8::Isolate* isolate, bool removeFromMap);

  // A new CPU profiler object will be created each time profiling is started
  // to work around https://bugs.chromium.org/p/v8/issues/detail?id=11051.
  v8::CpuProfiler* CreateV8CpuProfiler();

  ContextsByNode GetContextsByNode(v8::CpuProfile* profile,
                                   ContextBuffer& contexts,
                                   int64_t startCpuTime);

  bool waitForSignal(uint64_t targetCallCount = 0);
  static void CleanupHook(void* data);
  void Cleanup(v8::Isolate* isolate);

  ContextPtr GetContextPtr(v8::Isolate* isolate);
  ContextPtr GetContextPtrSignalSafe(v8::Isolate* isolate);

 public:
  /**
   * @param samplingPeriodMicros sampling interval, in microseconds
   * @param durationMicros the duration of sampling, in microseconds. This
   * parameter is informative; it is up to the caller to call the Stop method
   * every period. The parameter is used to preallocate data structures that
   * should not be reallocated in async signal safe code.
   * @param useCPED whether to use the V8 ContinuationPreservedEmbedderData to
   * store the current sampling context. It can be used if AsyncLocalStorage
   * uses the AsyncContextFrame implementation (experimental in Node 23, default
   * in Node 24.)
   */
  explicit WallProfiler(std::chrono::microseconds samplingPeriod,
                        std::chrono::microseconds duration,
                        bool includeLines,
                        bool withContexts,
                        bool workaroundV8bug,
                        bool collectCpuTime,
                        bool collectAsyncId,
                        bool isMainThread,
                        bool useCPED);

  v8::Local<v8::Value> GetContext(v8::Isolate*);
  void SetContext(v8::Isolate*, v8::Local<v8::Value>);
  void PushContext(int64_t time_from,
                   int64_t time_to,
                   int64_t cpu_time,
                   v8::Isolate* isolate);
  Result StartImpl();
  v8::ProfilerId StartInternal();
  Result StopImpl(bool restart, v8::Local<v8::Value>& profile);

  CollectionMode collectionMode() {
    auto res = collectionMode_.load(std::memory_order_relaxed);
    if (res == CollectionMode::kNoCollect) {
      noCollectCallCount_.fetch_add(1, std::memory_order_relaxed);
    }
    std::atomic_signal_fence(std::memory_order_acquire);
    return res;
  }

  bool collectCpuTime() const { return collectCpuTime_; }

  bool interceptSignal() const { return withContexts_ || workaroundV8Bug_; }

  int v8ProfilerStuckEventLoopDetected() const {
    return v8ProfilerStuckEventLoopDetected_;
  }

  ThreadCpuClock::duration GetAndResetThreadCpu() {
    return threadCpuStopWatch_.GetAndReset();
  }

  double GetAsyncId(v8::Isolate* isolate);
  void OnGCStart(v8::Isolate* isolate);
  void OnGCEnd();

  static NAN_METHOD(New);
  static NAN_METHOD(Start);
  static NAN_METHOD(Stop);
  static NAN_METHOD(V8ProfilerStuckEventLoopDetected);
  static NAN_METHOD(Dispose);
  static NAN_MODULE_INIT(Init);
  static NAN_GETTER(GetContext);
  static NAN_SETTER(SetContext);
  static NAN_GETTER(SharedArrayGetter);
};

}  // namespace dd
