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
#include <v8.h>

#include "allocation-profile-node.hh"
#include "otel-thread-ctx.hh"
#include "profilers/heap.hh"
#include "profilers/wall.hh"
#include "translate-time-profile.hh"

#ifdef __linux__
#include <sys/syscall.h>
#include <unistd.h>
#endif

// Whether the isolate's ContinuationPreservedEmbedderData is a JS Map that
// currently binds `key` to `value`.
//
// This exists for AsyncContextFrame feature detection. With ACF active, Node
// implements AsyncLocalStorage#run by installing an AsyncContextFrame — a JS
// Map keyed by the AsyncLocalStorage instance — as the CPED of the running
// continuation. Calling this from inside a run() with the storage and its
// store therefore observes the property this addon actually depends on,
// instead of inferring it from the Node version, process.execArgv, or whether
// run() happens to dispatch through the instance's enterWith.
static NAN_METHOD(CpedMapContains) {
#if NODE_MAJOR_VERSION >= 22
  // A malformed call must not accidentally answer true by comparing an absent
  // key's undefined against an undefined expected value.
  if (info.Length() >= 2) {
    auto isolate = info.GetIsolate();
    auto cped = isolate->GetContinuationPreservedEmbedderData();
    if (!cped.IsEmpty() && cped->IsMap()) {
      auto context = isolate->GetCurrentContext();
      if (!context.IsEmpty()) {
        v8::Local<v8::Value> found;
        if (cped.As<v8::Map>()->Get(context, info[0]).ToLocal(&found)) {
          info.GetReturnValue().Set(found->StrictEquals(info[1]));
          return;
        }
      }
    }
  }
#endif
  // Either code above didn't reach the innermost if statement, or
  // we're compiling for Node.js < 22.
  info.GetReturnValue().Set(false);
}

static NAN_METHOD(GetNativeThreadId) {
#ifdef __APPLE__
  uint64_t native_id;
  (void)pthread_threadid_np(NULL, &native_id);
#elif defined(__linux__)
  pid_t native_id = syscall(SYS_gettid);
#elif defined(_MSC_VER)
  DWORD native_id = GetCurrentThreadId();
#endif
  info.GetReturnValue().Set(v8::Integer::New(info.GetIsolate(), native_id));
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-function-type"
#endif
NODE_MODULE_INIT(/* exports, module, context */) {
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

  dd::AllocationProfileNodeView::Init(exports);
  dd::TimeProfileNodeView::Init(exports);
  dd::HeapProfiler::Init(exports);
  dd::WallProfiler::Init(exports);
  dd::OtelThreadCtx::Init(exports);
  Nan::SetMethod(exports, "getNativeThreadId", GetNativeThreadId);
  Nan::SetMethod(exports, "cpedMapContains", CpedMapContains);
}
