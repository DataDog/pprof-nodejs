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

#include "../per-isolate-data.hh"
#include "defer.hh"

#include <chrono>
#include <memory>
#include <vector>

#include <node.h>
#include <v8-profiler.h>
// #include <sys/prctl.h>


namespace dd {

struct Node {
  using Allocation = v8::AllocationProfile::Allocation;
  std::string name;
  std::string script_name;
  int line_number;
  int column_number;
  int script_id;
  std::vector<std::shared_ptr<Node>> children;
  std::vector<Allocation> allocations;
};

enum CallbackMode {
  kNoCallback = 0,
  kAsyncCallback = 1,
  kInterruptCallback = 2,
};

struct HeapProfilerState {
  uint32_t       heap_extension_size;
  uint32_t       max_heap_extension_count;
  uint32_t       current_heap_extension_count;
  uv_async_t     async;
  std::shared_ptr<Node> profile;
  std::vector<std::string> export_command;
  bool           dumpProfileOnStderr;
  Nan::Callback  callback;
  uint32_t       callbackMode;
};

std::shared_ptr<Node> TranslateAllocationProfileToCpp(v8::AllocationProfile::Node* node) {
  auto new_node = std::make_shared<Node>();
  new_node->line_number = node->line_number;
  new_node->column_number = node->column_number;
  new_node->script_id = node->script_id;
  Nan::Utf8String name(node->name);
  new_node->name.assign(*name, name.length());
  Nan::Utf8String script_name(node->script_name);
  new_node->script_name.assign(*script_name, script_name.length());

  new_node->children.reserve(node->children.size());
  for(auto& child: node->children) {
    new_node->children.push_back(TranslateAllocationProfileToCpp(child));
  }

  new_node->allocations.reserve(node->allocations.size());
  for(auto& allocation: node->allocations) {
    new_node->allocations.push_back(allocation);
  }
  return new_node;
}

v8::Local<v8::Value> TranslateAllocationProfile(Node* node) {
  v8::Local<v8::Object> js_node = Nan::New<v8::Object>();

  Nan::Set(js_node, Nan::New<v8::String>("name").ToLocalChecked(), Nan::New(node->name).ToLocalChecked());
  Nan::Set(js_node, Nan::New<v8::String>("scriptName").ToLocalChecked(),
           Nan::New(node->script_name).ToLocalChecked());
  Nan::Set(js_node, Nan::New<v8::String>("scriptId").ToLocalChecked(),
           Nan::New<v8::Integer>(node->script_id));
  Nan::Set(js_node, Nan::New<v8::String>("lineNumber").ToLocalChecked(),
           Nan::New<v8::Integer>(node->line_number));
  Nan::Set(js_node, Nan::New<v8::String>("columnNumber").ToLocalChecked(),
           Nan::New<v8::Integer>(node->column_number));

  v8::Local<v8::Array> children = Nan::New<v8::Array>(node->children.size());
  for (size_t i = 0; i < node->children.size(); i++) {
    Nan::Set(children, i, TranslateAllocationProfile(node->children[i].get()));
  }
  Nan::Set(js_node, Nan::New<v8::String>("children").ToLocalChecked(), children);
  v8::Local<v8::Array> allocations = Nan::New<v8::Array>(node->allocations.size());
  for (size_t i = 0; i < node->allocations.size(); i++) {
    v8::AllocationProfile::Allocation alloc = node->allocations[i];
    v8::Local<v8::Object> js_alloc = Nan::New<v8::Object>();
    Nan::Set(js_alloc, Nan::New<v8::String>("sizeBytes").ToLocalChecked(),
             Nan::New<v8::Number>(alloc.size));
    Nan::Set(js_alloc, Nan::New<v8::String>("count").ToLocalChecked(),
             Nan::New<v8::Number>(alloc.count));
    Nan::Set(allocations, i, js_alloc);
  }
  Nan::Set(js_node, Nan::New<v8::String>("allocations").ToLocalChecked(),
           allocations);
  return js_node;
}

static void dumpAllocationProfile(FILE* file, Node* node, std::string & cur_stack) {
  auto initial_len = cur_stack.size();
  char buf[256];

  snprintf(buf, sizeof(buf), "%s%s:%s:%d", cur_stack.empty() ? "" : ";",
           node->script_name.empty() ? "_" : node->script_name.c_str(),
           node->name.empty() ?  "(anonymous)" : node->name.c_str(),
           node->line_number);
  cur_stack += buf;
  for(auto& allocation: node->allocations) {
    fprintf(file, "%s %u %zu\n", cur_stack.c_str(), allocation.count, allocation.count * allocation.size);
  }
  for(auto& child: node->children) {
    dumpAllocationProfile(file, child.get(), cur_stack);
  }
  cur_stack.resize(initial_len);
}

static void dumpAllocationProfile(FILE* file, Node* node) {
  std::string stack;
  dumpAllocationProfile(file, node, stack);
}

static void dumpAllocationProfileAsJSON(FILE* file, Node* node) {
  fprintf(file, R"({"name":"%s","scriptName":"%s","scriptId":%d,"lineNumber":%d,"columnNumber":%d,"children":[)",
          node->name.c_str(), node->script_name.c_str(), node->script_id, node->line_number, node->column_number);

  bool first = true;
  for(auto& child: node->children) {
    if (!first) {
      fputs(",", file);
    } else {
      first = false;
    }
    dumpAllocationProfileAsJSON(file, child.get());
  }
  fprintf(file, R"(],"allocations":[)");
  first = true;
  for(auto& allocation: node->allocations) {
    fprintf(file, R"(%s{"sizeBytes":%zu,"count":%d})",
            first ? "" : ",", allocation.size, allocation.count);
    first = false;
  }
  fputs("]}", file);
}

static void InterruptCallback(v8::Isolate* isolate, void* data);
static void AsyncCallback(uv_async_t* handle);

static void OnExit(uv_process_t *req, int64_t, int) {
  uv_timeval64_t tv;
  uv_gettimeofday(&tv);
  uint64_t ms = tv.tv_sec*1000 + tv.tv_usec/1000;

  fprintf(stderr, "[%lu]OnExit triggered\n", ms);
  if (req->data) {
    uv_timer_stop(reinterpret_cast<uv_timer_t*>(req->data));
  }
  uv_close((uv_handle_t*) req, nullptr);
}

static void TimerCallback(uv_timer_t *handle) {
  uv_timeval64_t tv;
  uv_gettimeofday(&tv);
  uint64_t ms = tv.tv_sec*1000 + tv.tv_usec/1000;

  fprintf(stderr, "[%lu]Timeout triggered\n", ms);
  uv_process_t *proc = reinterpret_cast<uv_process_t*>(handle->data);
  uv_process_kill(proc, SIGKILL);
}

static void CloseLoop(uv_loop_t & loop) {
  uv_run(&loop, UV_RUN_DEFAULT);
  uv_walk(
      &loop, [](uv_handle_t *handle, void *arg)
      {
        if (!uv_is_closing(handle)) {
          uv_close(handle, nullptr);
        }
      },
      nullptr);
  int r;
  do {
    r = uv_run(&loop, UV_RUN_ONCE);
  } while(r != 0);

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
  std::string tmpl = std::string{buf, sz} + "/heap_profile_XXXXXX";

// uv_fs_mkstemp is available in libuv >= 1.34.0
#if UV_VERSION_HEX >= 0x012200
  uv_fs_t req;
  if ((r = uv_fs_mkstemp(&loop, &req, tmpl.c_str(), nullptr)) < 0) {
    fprintf(stderr, "Failed to create temp file error: %s\n", uv_strerror(r));
    return -1;
  }
  filepath = req.path;
  uv_fs_req_cleanup(&req);
  return req.result;
#elif defined(__linux__) ||  defined(__APPLE__)
  filepath = tmpl;
  return mkstemp(&filepath[0]);
#else
  return -1;
#endif
}

static void ExportProfile(HeapProfilerState& state) {
  const int64_t timeoutMs = 5000;
  uv_loop_t loop;
  int r;

  if ((r = uv_loop_init(&loop)) != 0) {
     fprintf(stderr, "Failed to init new event loop: %s\n", uv_strerror(r));
    return;
  }

  defer { CloseLoop(loop); };

  std::string filepath;
  int fd;
  if ((fd = CreateTempFile(loop, filepath)) < 0) {
    return;
  }
  FILE* file = fdopen(fd, "w");
  dumpAllocationProfileAsJSON(file, state.profile.get());
  fclose(file);
  std::vector<char*> args;
  for(auto& arg: state.export_command) {
    args.push_back(const_cast<char*>(arg.data()));
  }
  args.push_back(&filepath[0]);
  args.push_back(nullptr);
  uv_process_options_t options = {};
  options.flags = UV_PROCESS_DETACHED;
  options.file = args[0];
  options.args = args.data();
  options.exit_cb = &OnExit;
  uv_process_t child_req;
  uv_timer_t timer;
  timer.data = &child_req;

    uv_timeval64_t tv;
  uv_gettimeofday(&tv);
  uint64_t ms = tv.tv_sec*1000 + tv.tv_usec/1000;
  child_req.data = &timer;

  options.stdio_count = 3;
    uv_stdio_container_t child_stdio[3];
    child_stdio[0].flags = UV_IGNORE;
    child_stdio[1].flags = UV_INHERIT_FD;
    child_stdio[2].flags = UV_INHERIT_FD;
    child_stdio[2].data.fd = 2;
    child_stdio[1].data.fd = 1;
    options.stdio = child_stdio;
    
  fprintf(stderr, "[%lu]Spawning export process:", ms);
  for(auto arg: args) {
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
  if ((r = uv_timer_start(&timer, &TimerCallback, timeoutMs, 0))) {
    fprintf(stderr, "Failed to start timer: %s\n", uv_strerror(r));
    return;
  }
  uv_run(&loop, UV_RUN_DEFAULT);

  // Delete temp file
  uv_fs_t fs_req;
  uv_fs_unlink(&loop, &fs_req, filepath.c_str(), nullptr);
  uv_gettimeofday(&tv);
  ms = tv.tv_sec*1000 + tv.tv_usec/1000;
  fprintf(stderr, "[%zu]Export done\n", ms);
}

static size_t NearHeapLimit(void* data, size_t current_heap_limit,
                            size_t initial_heap_limit) {
  auto isolate = v8::Isolate::GetCurrent();
  auto state = PerIsolateData::For(isolate)->GetHeapProfilerState();
  ++state->current_heap_extension_count;
  uv_timeval64_t tv;
  uv_gettimeofday(&tv);
  uint64_t ms = tv.tv_sec*1000 + tv.tv_usec/1000;
  fprintf(stderr, "[%lu]NearHeapLimit(count=%d): current_heap_limit=%zu, initial_heap_limit=%zu\n",
         ms, state->current_heap_extension_count, current_heap_limit, initial_heap_limit);

  auto n = isolate->NumberOfTrackedHeapObjectTypes();
  v8::HeapObjectStatistics stats;

  for(size_t i=0; i<n; ++i) {
    if (isolate->GetHeapObjectStatisticsAtLastGC(&stats, i) && stats.object_count() > 0) {
      fprintf(stderr, "HeapObjectStats: type=%s, subtype=%s, size=%zu, count=%zu\n",
        stats.object_type(), stats.object_sub_type(), stats.object_size(), stats.object_count());
    }
  }
  std::unique_ptr<v8::AllocationProfile> profile{isolate->GetHeapProfiler()->GetAllocationProfile()};
  state->profile = TranslateAllocationProfileToCpp(profile->GetRootNode());
  if (state->dumpProfileOnStderr) {
    dumpAllocationProfile(stderr, state->profile.get());
  }

  if (!state->callback.IsEmpty()) {
    if (state->callbackMode & kInterruptCallback) {
      isolate->RequestInterrupt(InterruptCallback, nullptr);
    }
    if (state->callbackMode & kAsyncCallback) {
      uv_async_send(&state->async);
    }
  }

  if (!state->export_command.empty()) {
    ExportProfile(*state);
  }

  uv_gettimeofday(&tv);
  ms = tv.tv_sec*1000 + tv.tv_usec/1000;
  fprintf(stderr, "[%zu]Returning from NearHeapLimit\n", ms);
  // fprintf(stderr, "[%zu]Aborting\n", ms);
  // // prctl(PR_SET_DUMPABLE, 0);
  // abort();
  return current_heap_limit + ((state->current_heap_extension_count <= state->max_heap_extension_count) ? state->heap_extension_size : 0);
}

v8::Local<v8::Value> TranslateAllocationProfile(v8::AllocationProfile::Node* node) {
  v8::Local<v8::Object> js_node = Nan::New<v8::Object>();

  Nan::Set(js_node, Nan::New<v8::String>("name").ToLocalChecked(), node->name);
  Nan::Set(js_node, Nan::New<v8::String>("scriptName").ToLocalChecked(),
           node->script_name);
  Nan::Set(js_node, Nan::New<v8::String>("scriptId").ToLocalChecked(),
           Nan::New<v8::Integer>(node->script_id));
  Nan::Set(js_node, Nan::New<v8::String>("lineNumber").ToLocalChecked(),
           Nan::New<v8::Integer>(node->line_number));
  Nan::Set(js_node, Nan::New<v8::String>("columnNumber").ToLocalChecked(),
           Nan::New<v8::Integer>(node->column_number));

  v8::Local<v8::Array> children = Nan::New<v8::Array>(node->children.size());
  for (size_t i = 0; i < node->children.size(); i++) {
    Nan::Set(children, i, TranslateAllocationProfile(node->children[i]));
  }
  Nan::Set(js_node, Nan::New<v8::String>("children").ToLocalChecked(), children);
  v8::Local<v8::Array> allocations = Nan::New<v8::Array>(node->allocations.size());
  for (size_t i = 0; i < node->allocations.size(); i++) {
    v8::AllocationProfile::Allocation alloc = node->allocations[i];
    v8::Local<v8::Object> js_alloc = Nan::New<v8::Object>();
    Nan::Set(js_alloc, Nan::New<v8::String>("sizeBytes").ToLocalChecked(),
             Nan::New<v8::Number>(alloc.size));
    Nan::Set(js_alloc, Nan::New<v8::String>("count").ToLocalChecked(),
             Nan::New<v8::Number>(alloc.count));
    Nan::Set(allocations, i, js_alloc);
  }
  Nan::Set(js_node, Nan::New<v8::String>("allocations").ToLocalChecked(),
           allocations);
  return js_node;
}

NAN_METHOD(HeapProfiler::StartSamplingHeapProfiler) {
  if (info.Length() == 2) {
    if (!info[0]->IsUint32()) {
      return Nan::ThrowTypeError("First argument type must be uint32.");
    }
    if (!info[1]->IsNumber()) {
      return Nan::ThrowTypeError("First argument type must be Integer.");
    }

#if NODE_MODULE_VERSION > NODE_8_0_MODULE_VERSION
    uint64_t sample_interval = info[0].As<v8::Integer>()->Value();
    int stack_depth = info[1].As<v8::Integer>()->Value();
#else
    uint64_t sample_interval = info[0].As<Integer>()->Uint32Value();
    int stack_depth = info[1].As<Integer>()->IntegerValue();
#endif

    info.GetIsolate()->GetHeapProfiler()->StartSamplingHeapProfiler(
        sample_interval, stack_depth);
  } else {
    info.GetIsolate()->GetHeapProfiler()->StartSamplingHeapProfiler();
  }
}

// Signature:
// stopSamplingHeapProfiler()
NAN_METHOD(HeapProfiler::StopSamplingHeapProfiler) {
  auto isolate = info.GetIsolate();
  isolate->GetHeapProfiler()->StopSamplingHeapProfiler();
  auto state = PerIsolateData::For(isolate)->GetHeapProfilerState();
  if (state) {
    isolate->RemoveNearHeapLimitCallback(&NearHeapLimit, 0);
    state.reset();
  }
}

// Signature:
// getAllocationProfile(): AllocationProfileNode
NAN_METHOD(HeapProfiler::GetAllocationProfile) {
  std::unique_ptr<v8::AllocationProfile> profile(
      info.GetIsolate()->GetHeapProfiler()->GetAllocationProfile());
  v8::AllocationProfile::Node* root = profile->GetRootNode();
  info.GetReturnValue().Set(TranslateAllocationProfile(root));
}

NAN_METHOD(HeapProfiler::MonitorOutOfMemory) {
   if (info.Length() != 6) {
    return Nan::ThrowTypeError("MonitorOOMCondition must have six arguments.");
  }
  if (!info[0]->IsUint32()) {
    return Nan::ThrowTypeError("Heap limit extension size must be a uint32.");
  }
  if (!info[1]->IsUint32()) {
    return Nan::ThrowTypeError("Max heap limit extension count must be a uint32.");
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

  auto isolate = v8::Isolate::GetCurrent();
  isolate->AddNearHeapLimitCallback(&NearHeapLimit, nullptr);

  auto & state = PerIsolateData::For(isolate)->GetHeapProfilerState();
  state = std::make_shared<HeapProfilerState>();
  state->heap_extension_size = info[0].As<v8::Integer>()->Value();
  state->max_heap_extension_count = info[1].As<v8::Integer>()->Value();
  state->dumpProfileOnStderr = info[2].As<v8::Boolean>()->Value();
  state->callbackMode = info[5].As<v8::Integer>()->Value();

  if (!info[4]->IsNullOrUndefined() && state->callbackMode != kNoCallback) {
    state->callback.Reset(Nan::To<v8::Function>(info[4]).ToLocalChecked());

  }

  auto commands = info[3].As<v8::Array>();
  for(uint32_t i=0; i<commands->Length(); ++i) {
    auto value = Nan::Get(commands, i).ToLocalChecked();
    if (value->IsString()) {
      Nan::Utf8String arg{value};
      state->export_command.emplace_back(*arg, arg.length());
    }
  }

  if (!state->callback.IsEmpty() && (state->callbackMode & kAsyncCallback)) {
    uv_async_init(Nan::GetCurrentEventLoop(), &state->async, AsyncCallback);
    uv_unref(reinterpret_cast<uv_handle_t*>(&state->async));
  }
}

NAN_MODULE_INIT(HeapProfiler::Init) {
  v8::Local<v8::Object> heapProfiler = Nan::New<v8::Object>();
  Nan::SetMethod(heapProfiler, "startSamplingHeapProfiler",
                 StartSamplingHeapProfiler);
  Nan::SetMethod(heapProfiler, "stopSamplingHeapProfiler",
                 StopSamplingHeapProfiler);
  Nan::SetMethod(heapProfiler, "getAllocationProfile",
                 GetAllocationProfile);
  Nan::SetMethod(heapProfiler,
                 "monitorOutOfMemory",
                 MonitorOutOfMemory);
  Nan::Set(target, Nan::New<v8::String>("heapProfiler").ToLocalChecked(),
           heapProfiler);
}

void InterruptCallback(v8::Isolate* isolate, void* data) {
  v8::HandleScope scope(isolate);
  auto state = PerIsolateData::For(isolate)->GetHeapProfilerState();
  v8::Local<v8::Value> argv[1] = { dd::TranslateAllocationProfile(state->profile.get()) };
  Nan::AsyncResource resource("NearHeapLimit");
  state->callback.Call(1, argv, &resource);
}

void AsyncCallback(uv_async_t* handle) {
  InterruptCallback(v8::Isolate::GetCurrent(), nullptr);
}

} // namespace dd
