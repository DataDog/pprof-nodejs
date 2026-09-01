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

#include "heap.hh"

#include "allocation-profile-node.hh"
#include "allocation-profile.hh"
#include "near-oom.hh"
#include "per-isolate-data.hh"
#include "translate-heap-profile.hh"

#include <memory>
#include <mutex>
#include <unordered_set>

#include <node.h>
#include <v8-profiler.h>

namespace dd {

// Track which isolates have cleanup hooks registered for heap profiler
static std::unordered_set<v8::Isolate*> g_heap_profiler_isolates;
static std::mutex g_heap_profiler_mutex;

// Cleanup hook to stop heap profiler before isolate is destroyed
static void HeapProfilerCleanupHook(void* data) {
  auto isolate = static_cast<v8::Isolate*>(data);
  {
    const std::lock_guard<std::mutex> lock(g_heap_profiler_mutex);
    g_heap_profiler_isolates.erase(isolate);
  }
  // Stop the sampling heap profiler to prevent crash during V8 teardown
  auto heap_profiler = isolate->GetHeapProfiler();
  if (heap_profiler) {
    heap_profiler->StopSamplingHeapProfiler();
  }
}

NAN_METHOD(HeapProfiler::StartSamplingHeapProfiler) {
  auto isolate = info.GetIsolate();

  // Register cleanup hook if not already registered for this isolate
  {
    const std::lock_guard<std::mutex> lock(g_heap_profiler_mutex);
    if (g_heap_profiler_isolates.find(isolate) ==
        g_heap_profiler_isolates.end()) {
      node::AddEnvironmentCleanupHook(
          isolate, HeapProfilerCleanupHook, isolate);
      g_heap_profiler_isolates.insert(isolate);
    }
  }

  bool allocations = false;

  if (info.Length() == 2 || info.Length() == 3) {
    if (!info[0]->IsUint32()) {
      return Nan::ThrowTypeError("First argument type must be uint32.");
    }
    if (!info[1]->IsNumber()) {
      return Nan::ThrowTypeError("Second argument type must be Integer.");
    }
    if (info.Length() == 3 && !info[2]->IsBoolean()) {
      return Nan::ThrowTypeError("Third argument type must be boolean.");
    }

    uint64_t sample_interval = info[0].As<v8::Integer>()->Value();
    int stack_depth = info[1].As<v8::Integer>()->Value();
    allocations = info.Length() == 3 && info[2].As<v8::Boolean>()->Value();
#if NODE_MAJOR_VERSION < 26
    if (allocations) {
      return Nan::ThrowError(
          "Allocation profiling requires Node.js 26 or newer.");
    }
#endif
    auto flags = v8::HeapProfiler::kSamplingNoFlags;
#if NODE_MAJOR_VERSION >= 26
    if (allocations) {
      flags = static_cast<v8::HeapProfiler::SamplingFlags>(
          v8::HeapProfiler::kSamplingForceGC |
          v8::HeapProfiler::kSamplingIncludeObjectsCollectedByMajorGC |
          v8::HeapProfiler::kSamplingIncludeObjectsCollectedByMinorGC);
    }
#endif

    isolate->GetHeapProfiler()->StartSamplingHeapProfiler(
        sample_interval, stack_depth, flags);
  } else if (info.Length() == 0) {
    isolate->GetHeapProfiler()->StartSamplingHeapProfiler();
  } else {
    return Nan::ThrowTypeError(
        "StartSamplingHeapProfiler must have 0, 2, or 3 arguments.");
  }

  auto& state = PerIsolateData::For(isolate)->GetHeapProfilerState();
  if (!state) {
    state = std::make_shared<HeapProfilerState>(isolate);
  }
  state->allocations = allocations;
}

// Signature:
// stopSamplingHeapProfiler()
NAN_METHOD(HeapProfiler::StopSamplingHeapProfiler) {
  auto isolate = info.GetIsolate();
  isolate->GetHeapProfiler()->StopSamplingHeapProfiler();

  // Uninstall explicitly rather than leaving it to ~HeapProfilerState. reset()
  // only destroys the state if this is the last reference, and it need not be:
  // NearHeapLimit and InterruptCallback both take a shared_ptr copy for the
  // duration of the call, so a stop() reached from inside one of them (the
  // near-heap-limit JS callback calling heapProfiler.stop(), say) would leave
  // the state alive, the destructor unrun, and this callback still registered
  // with V8 while the per-isolate slot is already empty. The next
  // near-heap-limit GC would then enter NearHeapLimit with no state at all.
  // Idempotent: it clears callbackInstalled.
  auto& state = PerIsolateData::For(isolate)->GetHeapProfilerState();
  if (state) {
    state->UninstallNearHeapLimitCallback();
  }
  state.reset();

  // Remove cleanup hook since profiler is explicitly stopped
  {
    const std::lock_guard<std::mutex> lock(g_heap_profiler_mutex);
    if (g_heap_profiler_isolates.erase(isolate) == 1) {
      node::RemoveEnvironmentCleanupHook(
          isolate, HeapProfilerCleanupHook, isolate);
    }
  }
}

// Signature:
// getAllocationProfile(): AllocationProfileNode
NAN_METHOD(HeapProfiler::GetAllocationProfile) {
  auto isolate = info.GetIsolate();
  auto& state = PerIsolateData::For(isolate)->GetHeapProfilerState();

  std::unique_ptr<v8::AllocationProfile> profile(
      isolate->GetHeapProfiler()->GetAllocationProfile());
  if (!profile) {
    return Nan::ThrowError("Heap profiler is not enabled.");
  }
  // A non-null profile only proves V8's sampling heap profiler is running; it
  // does not imply we are the one who started it. Anything else in the process
  // (the inspector's HeapProfiler.startSampling, another agent) can enable it
  // without ever going through StartSamplingHeapProfiler, in which case there
  // is no per-isolate state. Serve the profile without allocation stats rather
  // than dereferencing an empty shared_ptr.
  const bool allocations = state && state->allocations;
  v8::AllocationProfile::Node* root = profile->GetRootNode();
  AllocationProfileNodeStatsMap allocation_stats;
  if (allocations) {
    allocation_stats = BuildAllocationStatsByNodeId(profile->GetSamples());
  }

  if (state) {
    state->OnNewProfile();
  }
  info.GetReturnValue().Set(TranslateAllocationProfile(
      root, allocations ? &allocation_stats : nullptr));
}

// mapAllocationProfile(callback): callback result
NAN_METHOD(HeapProfiler::MapAllocationProfile) {
  if (info.Length() < 1 || !info[0]->IsFunction()) {
    return Nan::ThrowTypeError("mapAllocationProfile requires a callback");
  }
  auto isolate = info.GetIsolate();
  auto callback = info[0].As<v8::Function>();
  auto& state = PerIsolateData::For(isolate)->GetHeapProfilerState();
  if (state && state->allocations) {
    return Nan::ThrowError(
        "mapAllocationProfile does not support allocation mode.");
  }

  std::unique_ptr<v8::AllocationProfile> profile(
      isolate->GetHeapProfiler()->GetAllocationProfile());

  if (!profile) {
    return Nan::ThrowError("Heap profiler is not enabled.");
  }

  // As in GetAllocationProfile: V8's profiler may be running without us having
  // started it, so there may be no per-isolate state to update.
  if (state) {
    state->OnNewProfile();
  }

  auto root = AllocationProfileNodeView::New(profile->GetRootNode());
  v8::Local<v8::Value> argv[] = {root};
  auto result =
      Nan::Call(callback, isolate->GetCurrentContext()->Global(), 1, argv);
  if (!result.IsEmpty()) {
    info.GetReturnValue().Set(result.ToLocalChecked());
  }
}

NAN_MODULE_INIT(HeapProfiler::Init) {
  v8::Local<v8::Object> heapProfiler = Nan::New<v8::Object>();
  Nan::SetMethod(
      heapProfiler, "startSamplingHeapProfiler", StartSamplingHeapProfiler);
  Nan::SetMethod(
      heapProfiler, "stopSamplingHeapProfiler", StopSamplingHeapProfiler);
  Nan::SetMethod(heapProfiler, "getAllocationProfile", GetAllocationProfile);
  Nan::SetMethod(heapProfiler, "mapAllocationProfile", MapAllocationProfile);
  Nan::SetMethod(heapProfiler, "monitorOutOfMemory", MonitorOutOfMemory);
  Nan::Set(target,
           Nan::New<v8::String>("heapProfiler").ToLocalChecked(),
           heapProfiler);
}

}  // namespace dd
