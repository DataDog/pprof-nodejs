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

#include <uv.h>
#include <v8.h>

#include <cstddef>

namespace dd {

enum CallbackMode {
  kNoCallback = 0,
  kAsyncCallback = 1,
  kInterruptCallback = 2,
};

size_t NearHeapLimit(void* data,
                     size_t current_heap_limit,
                     size_t initial_heap_limit);
void InterruptCallback(v8::Isolate* isolate, void* data);
void AsyncCallback(uv_async_t* handle);

}  // namespace dd
