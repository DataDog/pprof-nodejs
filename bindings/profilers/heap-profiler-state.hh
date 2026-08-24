/*
 * Copyright 2026 Datadog, Inc
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

#include "heap-near-oom.hh"
#include "translate-heap-profile.hh"

#include <nan.h>
#include <uv.h>
#include <v8-profiler.h>
#include <v8.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace dd {

struct HeapProfilerState {
  explicit HeapProfilerState(v8::Isolate* isolate) : isolate(isolate) {}

  ~HeapProfilerState() {
    // Uninstall first. By the time we run, the shared_ptr in PerIsolateData is
    // already empty (that is what destroyed us), so NearHeapLimit would find no
    // state to work with; anything below that can trigger a GC must not be able
    // to reach it.
    UninstallNearHeapLimitCallback();

    auto profiler = isolate->GetHeapProfiler();
    if (profiler) {
      profiler->StopSamplingHeapProfiler();
    }

    if (async) {
      // defer deletion of async when uv_close callback is invoked
      uv_close(reinterpret_cast<uv_handle_t*>(async), [](uv_handle_t* handle) {
        delete reinterpret_cast<uv_async_t*>(handle);
      });
      async = nullptr;
    }
  }

  void UninstallNearHeapLimitCallback() {
    if (isolate && callbackInstalled) {
      isolate->RemoveNearHeapLimitCallback(&NearHeapLimit, 0);
      callbackInstalled = false;
    }
  }

  void InstallNearHeapLimitCallback() {
    if (callbackInstalled) {
      return;
    }
    if (isolate) {
      isolate->AddNearHeapLimitCallback(&NearHeapLimit, nullptr);
      // Restore the original heap limit once live old-generation usage falls
      // below 90% of the original limit. The threshold controls when V8
      // restores the limit, not the restored limit size.
      constexpr double kHeapLimitRestoreThreshold = 0.90;
      isolate->AutomaticallyRestoreInitialHeapLimit(kHeapLimitRestoreThreshold);
      callbackInstalled = true;
    }
  }

  void RegisterAsyncCallback() {
    if (async) {
      return;
    }
    // async is dynamically allocated so that its lifetime can be different
    // from the one of HeapProfilerState since uv_close is asynchronous
    async = new uv_async_t();
    uv_async_init(Nan::GetCurrentEventLoop(), async, AsyncCallback);
    uv_unref(reinterpret_cast<uv_handle_t*>(async));
  }

  void OnNewProfile() {
    profile.reset();
    // Only (re)install the NearHeapLimit callback when OOM monitoring is
    // configured. Otherwise a plain start()+profile() flow would silently
    // register a callback that the user never asked for.
    if (max_heap_extension_count > 0) {
      InstallNearHeapLimitCallback();
    }
  }

  v8::Isolate* isolate = nullptr;
  uint32_t heap_extension_size = 0;
  // When true, heap_extension_size is ignored in favour of one maximum-sized
  // young generation, sampled once into automatic_heap_extension_size.
  bool automatic_heap_extension = false;
  std::optional<size_t> automatic_heap_extension_size;
  uint32_t max_heap_extension_count = 0;
  uint32_t current_heap_extension_count = 0;
  uv_async_t* async = nullptr;
  std::shared_ptr<Node> profile;
  std::vector<std::string> export_command;
  bool allocations = false;
  bool dumpProfileOnStderr = false;
  Nan::Callback callback;
  uint32_t callbackMode = 0;
  bool isMainThread = true;
  bool callbackInstalled = false;
  bool insideCallback = false;
};

}  // namespace dd
