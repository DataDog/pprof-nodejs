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

#include "near-oom.hh"

#include "defer.hh"
#include "heap.hh"
#include "per-isolate-data.hh"
#include "translate-heap-profile.hh"

#include <chrono>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include <node.h>
#include <v8-profiler.h>

namespace dd {

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

// V8 only raises the limit when the returned value is strictly greater than
// current_heap_limit, and clamps it to its own allocator maximum, so
// saturating is enough to stay well-defined in the extreme case.
static size_t ExtendedHeapLimit(size_t current_heap_limit, size_t extension) {
  return extension > std::numeric_limits<size_t>::max() - current_heap_limit
             ? std::numeric_limits<size_t>::max()
             : current_heap_limit + extension;
}

size_t NearHeapLimit(void* data,
                     size_t current_heap_limit,
                     size_t initial_heap_limit) {
  auto isolate = v8::Isolate::GetCurrent();
  auto state = PerIsolateData::For(isolate)->GetHeapProfilerState();

  if (!state) {
    // StopSamplingHeapProfiler uninstalls us before dropping the state, so
    // normally this cannot happen. The gap is the other destruction path: a
    // shared_ptr copy taken by an in-flight NearHeapLimit or InterruptCallback
    // can outlive the per-isolate slot — the OOM JS callback calling
    // process.exit() erases PerIsolateData while InterruptCallback still holds
    // a reference, so ~HeapProfilerState never runs to uninstall us. Decline
    // and let V8 do its normal OOM handling.
    //
    // Deliberately no RemoveNearHeapLimitCallback here: the state that tracked
    // the installation is already unreachable, so callbackInstalled cannot be
    // cleared, and the only way to get here is a process on its way out.
    return current_heap_limit;
  }

  size_t extension = state->heap_extension_size;
  if (state->automatic_heap_extension) {
    if (!state->automatic_heap_extension_size.has_value()) {
      // Grant at most one young generation, as Node.js does for its near-OOM
      // heap snapshot callback. In current V8, heap_size_limit() is the
      // old-generation limit this callback was handed plus the maximum
      // young-generation size, so the delta is that young generation. It is
      // fixed for the isolate, so sample it once and reuse it.
      v8::HeapStatistics heap_statistics;
      isolate->GetHeapStatistics(&heap_statistics);
      const size_t total_heap_limit = heap_statistics.heap_size_limit();
      // Only cache a usable sample: a degenerate one must not disable
      // automatic sizing for the rest of the isolate's lifetime.
      if (total_heap_limit > current_heap_limit) {
        state->automatic_heap_extension_size =
            total_heap_limit - current_heap_limit;
      }
    }
    extension = state->automatic_heap_extension_size.value_or(0);
  }

  if (state->insideCallback) {
    // Reentrant call: GetAllocationProfile() allocated its way back into us.
    // The in-progress capture still needs room to finish, so rescue it even
    // when the caller asked for no top-level extension at all.
    constexpr size_t kReentrantRescueExtension = 10 * 1024 * 1024;
    return ExtendedHeapLimit(
        current_heap_limit,
        extension != 0 ? extension : kReentrantRescueExtension);
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
             ? ExtendedHeapLimit(current_heap_limit, extension)
             : current_heap_limit;
}

NAN_METHOD(HeapProfiler::MonitorOutOfMemory) {
  if (info.Length() != 8) {
    return Nan::ThrowTypeError("MonitorOOMCondition must have 8 arguments.");
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
  if (!info[7]->IsBoolean()) {
    return Nan::ThrowTypeError(
        "AutomaticHeapLimitExtension must be a boolean.");
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
  state->automatic_heap_extension_size.reset();
  state->profile.reset();
  state->export_command.clear();
  state->callback.Reset();

  state->heap_extension_size = info[0].As<v8::Integer>()->Value();
  state->max_heap_extension_count = info[1].As<v8::Integer>()->Value();
  state->dumpProfileOnStderr = info[2].As<v8::Boolean>()->Value();
  state->callbackMode = info[5].As<v8::Integer>()->Value();
  state->isMainThread = info[6].As<v8::Boolean>()->Value();
  state->automatic_heap_extension = info[7].As<v8::Boolean>()->Value();
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
