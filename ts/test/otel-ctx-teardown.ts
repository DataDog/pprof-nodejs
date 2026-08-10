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

'use strict';

// Runs in a forked process: the failure mode under test is a SIGABRT, which
// would take the whole mocha run down.
//
// When CtxWrap derived from node::ObjectWrap, a CtxWrap collected during
// isolate teardown ran ~ObjectWrap -> RemoveEnvironmentCleanupHook, which
// CHECKs that an Environment is current. It is not, during teardown, so the
// process aborted:
//
//   Assertion failed: (env) != nullptr
//    3: otel_thread_ctx_nodejs::CtxWrap::~CtxWrap()
//
// It needs enough instances that V8 still has some left to collect at
// teardown — nothing below ~1000 reproduced it — hence the count here.

import {otelThreadCtx} from '../src/index';

const N = 3000;

function id(n: number, len: number): Uint8Array {
  const b = new Uint8Array(len);
  b[0] = (n >> 24) & 0xff;
  b[1] = (n >> 16) & 0xff;
  b[2] = (n >> 8) & 0xff;
  b[3] = n & 0xff;
  return b;
}

const retained: unknown[] = [];

for (let i = 0; i < N; i++) {
  const ctx = new otelThreadCtx.ThreadContext(id(i, 16), id(i, 8), [
    'k',
    String(i),
  ]);
  if (i % 4 === 0) {
    // Still strongly reachable at exit.
    retained.push(ctx);
  } else {
    // Reachable only through the async context frame; collectable whenever
    // V8 decides, including during teardown.
    ctx.enter();
  }
}

(globalThis as unknown as {__retained: unknown}).__retained = retained;

// Exit through the normal path, so the Environment is torn down and the
// isolate disposed. That is where the weak callbacks in question fire.
console.log(`created ${N}, retained ${retained.length}`);
