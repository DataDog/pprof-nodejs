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
#include "defer.hh"
#include "per-isolate-data.hh"
#include "translate-heap-profile.hh"

#include <chrono>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_set>
#include <vector>

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

static size_t NearHeapLimit(void* data,
                            size_t current_heap_limit,
                            size_t initial_heap_limit);
static void InterruptCallback(v8::Isolate* isolate, void* data);
static void AsyncCallback(uv_async_t* handle);

enum CallbackMode {
  kNoCallback = 0,
  kAsyncCallback = 1,
  kInterruptCallback = 2,
};

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

static void dumpAllocationProfile(FILE* file,
                                  Node* node,
                                  std::string& cur_stack) {
  auto initial_len = cur_stack.size();
  char buf[256];

  snprintf(buf,
           sizeof(buf),
           "%s%s:%s:%d",
           cur_stack.empty() ? "" : ";",
           node->script_name.empty() ? "_" : node->script_name.c_str(),
           node->name.empty() ? "(anonymous)" : node->name.c_str(),
           node->line_number);
  cur_stack += buf;
  for (auto& allocation : node->allocations) {
    fprintf(file,
            "%s %u %zu\n",
            cur_stack.c_str(),
            allocation.count,
            allocation.count * allocation.size);
  }
  for (auto& child : node->children) {
    dumpAllocationProfile(file, child.get(), cur_stack);
  }
  cur_stack.resize(initial_len);
}

static void dumpAllocationProfile(FILE* file, Node* node) {
  std::string stack;
  dumpAllocationProfile(file, node, stack);
}

static void dumpAllocationProfileAsJSON(FILE* file, Node* node) {
  fprintf(
      file,
      R"({"name":"%s","scriptName":"%s","scriptId":%d,"lineNumber":%d,"columnNumber":%d,"children":[)",
      node->name.c_str(),
      node->script_name.c_str(),
      node->script_id,
      node->line_number,
      node->column_number);

  bool first = true;
  for (auto& child : node->children) {
    if (!first) {
      fputs(",", file);
    } else {
      first = false;
    }
    dumpAllocationProfileAsJSON(file, child.get());
  }
  fprintf(file, R"(],"allocations":[)");
  first = true;
  for (auto& allocation : node->allocations) {
    fprintf(file,
            R"(%s{"sizeBytes":%zu,"count":%d})",
            first ? "" : ",",
            allocation.size,
            allocation.count);
    first = false;
  }
  fputs("]}", file);
}

static void OnExit(uv_process_t* req, int64_t, int) {
  if (req->data) {
    uv_timer_stop(reinterpret_cast<uv_timer_t*>(req->data));
  }
  uv_close((uv_handle_t*)req, nullptr);
}

static void CloseLoop(uv_loop_t& loop) {
  uv_run(&loop, UV_RUN_DEFAULT);
  uv_walk(
      &loop,
      [](uv_handle_t* handle, void* arg) {
        if (!uv_is_closing(handle)) {
          uv_close(handle, nullptr);
        }
      },
      nullptr);
  int r;
  do {
    r = uv_run(&loop, UV_RUN_ONCE);
  } while (r != 0);

  if (uv_loop_close(&loop)) {
    fprintf(stderr, "Failed to close event loop\n");
  }
}

static int CreateTempFile(uv_loop_t& loop, std::string& filepath) {
  char buf[PATH_MAX];
  size_t sz = sizeof(buf);
  int r;
  if ((r = uv_os_tmpdir(buf, &sz)) != 0) {
    fprintf(stderr, "Failed to retrieve temp directory: %s\n", uv_strerror(r));
    return -1;
  }

#if defined(__linux__) || defined(__APPLE__)
  filepath = std::string{buf, sz} + "/heap_profile_XXXXXX";
  int fd = mkstemp(&filepath[0]);
  if (fd < 0) {
    fprintf(stderr,
            "Failed to create temp file %s : %s\n",
            filepath.c_str(),
            strerror(errno));
    return -1;
  }
  return fd;
#else
  // Use custom implementation of mkstemp() for Windows
  // uv_fs_mkstemp() is not used because it fails unexpectedly on Windows
  // (fail fast exception is raised when trying to write to the returned file
  // descriptor)
  const int max_tries = 3;
  for (int i = 0; i < max_tries; ++i) {
    filepath = std::string{buf, sz} + "/heap_profile_" +
               std::to_string(
                   std::chrono::system_clock::now().time_since_epoch().count());
    uv_fs_t fs_req{};
    int fd = uv_fs_open(&loop,
                        &fs_req,
                        filepath.c_str(),
                        UV_FS_O_CREAT | UV_FS_O_EXCL | UV_FS_O_WRONLY,
                        0600,
                        nullptr);
    uv_fs_req_cleanup(&fs_req);
    if (fd >= 0) {
      return r;
    }
    if (fd != UV_EEXIST) {
      fprintf(stderr, "Failed to create temp file: %s\n", uv_strerror(fd));
      return -1;
    }
  }
  return -1;
#endif
}

static void ExportProfile(HeapProfilerState& state) {
  const int64_t timeoutMs = 15000;
  uv_loop_t loop;
  int r;

  if ((r = uv_loop_init(&loop)) != 0) {
    fprintf(stderr, "Failed to init new event loop: %s\n", uv_strerror(r));
    return;
  }

  defer {
    CloseLoop(loop);
  };

  std::string filepath;
  int fd;
  if ((fd = CreateTempFile(loop, filepath)) < 0) {
    return;
  }
  FILE* file = fdopen(fd, "w");
  dumpAllocationProfileAsJSON(file, state.profile.get());
  fclose(file);
  std::vector<char*> args;
  for (auto& arg : state.export_command) {
    args.push_back(const_cast<char*>(arg.data()));
  }
  args.push_back(&filepath[0]);
  args.push_back(nullptr);
  uv_process_options_t options = {};
  options.flags = UV_PROCESS_DETACHED;
  options.file = args[0];
  options.args = args.data();
  options.exit_cb = &OnExit;
  uv_stdio_container_t child_stdio[3];
  child_stdio[0].flags = UV_IGNORE;
  child_stdio[1].flags = UV_INHERIT_FD;
  child_stdio[1].data.fd = 2;
  child_stdio[2].flags = UV_INHERIT_FD;
  child_stdio[2].data.fd = 2;
  options.stdio = child_stdio;
  options.stdio_count = 3;
  uv_process_t child_req;
  uv_timer_t timer;
  timer.data = &child_req;
  child_req.data = &timer;

  fprintf(stderr, "Spawning export process:");
  for (auto arg : args) {
    fprintf(stderr, " %s", arg ? arg : "\n");
  }
  if ((r = uv_spawn(&loop, &child_req, &options))) {
    fprintf(stderr, "Failed to spawn export process: %s\n", uv_strerror(r));
    return;
  }
  if ((r = uv_timer_init(&loop, &timer)) != 0) {
    fprintf(stderr, "Failed to init timer: %s\n", uv_strerror(r));
    return;
  }
  if ((r = uv_timer_start(
           &timer,
           [](uv_timer_t* handle) {
             uv_process_kill(reinterpret_cast<uv_process_t*>(handle->data),
                             SIGKILL);
           },
           timeoutMs,
           0))) {
    fprintf(stderr, "Failed to start timer: %s\n", uv_strerror(r));
    return;
  }
  uv_run(&loop, UV_RUN_DEFAULT);

  // Delete temp file
  uv_fs_t fs_req{};
  uv_fs_unlink(&loop, &fs_req, filepath.c_str(), nullptr);
  uv_fs_req_cleanup(&fs_req);
}

size_t NearHeapLimit(void* data,
                     size_t current_heap_limit,
                     size_t initial_heap_limit) {
  auto isolate = v8::Isolate::GetCurrent();
  auto state = PerIsolateData::For(isolate)->GetHeapProfilerState();

  constexpr size_t kFallbackExtension = 10 * 1024 * 1024;
  size_t extension = state->heap_extension_size;
  if (extension == 0) {
    if (!state->automatic_heap_extension_size.has_value()) {
      // GetAllocationProfile() can allocate and re-enter this callback. Give
      // the capture enough room for at most one young generation, as Node.js
      // does for its near-OOM heap snapshot callback. In current V8,
      // heap_size_limit() is the current old-generation limit plus the maximum
      // young-generation size. Cache the delta so nested callbacks do not
      // repeatedly collect heap statistics.
      v8::HeapStatistics heap_statistics;
      isolate->GetHeapStatistics(&heap_statistics);
      const size_t total_heap_limit = heap_statistics.heap_size_limit();
      state->automatic_heap_extension_size =
          total_heap_limit > current_heap_limit
              ? total_heap_limit - current_heap_limit
              : kFallbackExtension;
    }
    extension = *state->automatic_heap_extension_size;
  }

  // V8 requires a value greater than current_heap_limit to continue. Saturate
  // instead of overflowing in the defensive extreme case.
  const size_t new_heap_limit =
      extension > std::numeric_limits<size_t>::max() - current_heap_limit
          ? std::numeric_limits<size_t>::max()
          : current_heap_limit + extension;

  if (state->insideCallback) {
    return new_heap_limit;
  }
  state->insideCallback = true;
  defer {
    state->insideCallback = false;
  };

  ++state->current_heap_extension_count;
  fprintf(stderr,
          "NearHeapLimit(count=%d): current_heap_limit=%zu, "
          "initial_heap_limit=%zu\n",
          state->current_heap_extension_count,
          current_heap_limit,
          initial_heap_limit);

  auto n = isolate->NumberOfTrackedHeapObjectTypes();
  v8::HeapObjectStatistics stats;

  for (size_t i = 0; i < n; ++i) {
    if (isolate->GetHeapObjectStatisticsAtLastGC(&stats, i) &&
        stats.object_count() > 0) {
      fprintf(stderr,
              "HeapObjectStats: type=%s, subtype=%s, size=%zu, count=%zu\n",
              stats.object_type(),
              stats.object_sub_type(),
              stats.object_size(),
              stats.object_count());
    }
  }
  // GetAllocationProfile returns null when V8's sampling heap profiler isn't
  // running, and that can happen while this callback is still installed:
  // HeapProfilerCleanupHook stops V8's sampler without touching our state, so
  // between that hook and the isolate actually going away we stay registered
  // with nothing to sample. The heap-limit bookkeeping below still has to run,
  // so skip only the profile-dependent work.
  std::unique_ptr<v8::AllocationProfile> profile{
      isolate->GetHeapProfiler()->GetAllocationProfile()};
  if (profile) {
    state->profile = TranslateAllocationProfileToCpp(profile->GetRootNode());
    if (state->dumpProfileOnStderr) {
      dumpAllocationProfile(stderr, state->profile.get());
    }

    if (!state->export_command.empty()) {
      ExportProfile(*state);
    }

    if (!state->callback.IsEmpty()) {
      if (state->callbackMode & kInterruptCallback) {
        isolate->RequestInterrupt(InterruptCallback, nullptr);
      }
      if (state->callbackMode & kAsyncCallback) {
        uv_async_send(state->async);
      }
    } else {
      state->profile.reset();
    }
  } else {
    // Drop any profile retained from an earlier invocation: it is stale, and
    // nothing below is going to consume or replace it.
    state->profile.reset();
    fprintf(stderr,
            "NearHeapLimit: heap profiler is not enabled, no allocation "
            "profile to report\n");
  }

  if (!state->isMainThread) {
    // In worker thread, OOM is not fatal to the whole process and will only
    // terminate the worker.
    // This is done by a callback registered by node, that's why we remove our
    // callback and then call LowMemoryNotification() here to trigger another
    // garbage collection, which will eventually call the callback registered by
    // node.
    state->UninstallNearHeapLimitCallback();
    isolate->LowMemoryNotification();
    // use the same value as node plus 1
    constexpr size_t kExtraHeapAllowance = 16 * 1024 * 1024;
    return current_heap_limit + kExtraHeapAllowance + 1;
  }

  if (state->current_heap_extension_count >= state->max_heap_extension_count) {
    // On Node 14, NearLimitCallback is sometimes called many times, without the
    // process aborting, even when returned limit is not increased. Disable
    // callback until next call to GetAllocationProfile()
    state->UninstallNearHeapLimitCallback();
  }
  return state->current_heap_extension_count <= state->max_heap_extension_count
             ? new_heap_limit
             : current_heap_limit;
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

NAN_METHOD(HeapProfiler::MonitorOutOfMemory) {
  if (info.Length() != 7) {
    return Nan::ThrowTypeError("MonitorOOMCondition must have 7 arguments.");
  }
  if (!info[0]->IsUint32()) {
    return Nan::ThrowTypeError("Heap limit extension size must be a uint32.");
  }
  if (!info[1]->IsUint32()) {
    return Nan::ThrowTypeError(
        "Max heap limit extension count must be a uint32.");
  }
  if (!info[2]->IsBoolean()) {
    return Nan::ThrowTypeError("DumpHeapProfileOnStdErr must be a boolean.");
  }
  if (!info[3]->IsArray()) {
    return Nan::ThrowTypeError("Export command must be a string array.");
  }
  if (!info[4]->IsNullOrUndefined() && !info[4]->IsFunction()) {
    return Nan::ThrowTypeError("Callback name must be a function.");
  }
  if (!info[5]->IsUint32()) {
    return Nan::ThrowTypeError("CallbackMode must be a uint32.");
  }
  if (!info[6]->IsBoolean()) {
    return Nan::ThrowTypeError("IsMainThread must be a boolean.");
  }

  auto isolate = v8::Isolate::GetCurrent();

  // Reuse existing state if present so sample_interval/allocations set by
  // StartSamplingHeapProfiler survive. Only OOM-owned fields are reset below.
  // callbackInstalled is intentionally left alone —
  // InstallNearHeapLimitCallback below is idempotent.
  auto& state = PerIsolateData::For(isolate)->GetHeapProfilerState();
  if (!state) {
    state = std::make_shared<HeapProfilerState>(isolate);
  }

  state->current_heap_extension_count = 0;
  state->profile.reset();
  state->export_command.clear();
  state->callback.Reset();

  state->heap_extension_size = info[0].As<v8::Integer>()->Value();
  state->max_heap_extension_count = info[1].As<v8::Integer>()->Value();
  state->dumpProfileOnStderr = info[2].As<v8::Boolean>()->Value();
  state->callbackMode = info[5].As<v8::Integer>()->Value();
  state->isMainThread = info[6].As<v8::Boolean>()->Value();
  state->InstallNearHeapLimitCallback();
  if (!info[4]->IsNullOrUndefined() && state->callbackMode != kNoCallback) {
    state->callback.Reset(Nan::To<v8::Function>(info[4]).ToLocalChecked());
  }

  auto commands = info[3].As<v8::Array>();
  for (uint32_t i = 0; i < commands->Length(); ++i) {
    auto value = Nan::Get(commands, i).ToLocalChecked();
    if (value->IsString()) {
      Nan::Utf8String arg{value};
      state->export_command.emplace_back(*arg, arg.length());
    }
  }

  if (!state->callback.IsEmpty() && (state->callbackMode & kAsyncCallback)) {
    state->RegisterAsyncCallback();
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

void InterruptCallback(v8::Isolate* isolate, void* data) {
  v8::HandleScope scope(isolate);
  auto state = PerIsolateData::For(isolate)->GetHeapProfilerState();
  // The interrupt is requested from NearHeapLimit but runs later, so
  // StopSamplingHeapProfiler() may have dropped the state in between.
  if (!state || !state->profile) {
    return;
  }
  v8::Local<v8::Value> argv[1] = {
      dd::TranslateAllocationProfile(state->profile.get())};
  Nan::AsyncResource resource("NearHeapLimit");
  state->callback.Call(1, argv, &resource);
  // Release the retained native profile once the callback has been invoked.
  state->profile.reset();
}

void AsyncCallback(uv_async_t* handle) {
  InterruptCallback(v8::Isolate::GetCurrent(), nullptr);
}

}  // namespace dd
