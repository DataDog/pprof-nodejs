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

// Runs in a forked process because the failure mode under test is a SIGSEGV,
// which would take the whole mocha run down with it.
//
// V8's sampling heap profiler can be enabled by anything in the process - here
// the inspector's HeapProfiler.startSampling, but equally DevTools or another
// agent. That leaves getAllocationProfile()/mapAllocationProfile() with a live
// V8 profile but no per-isolate HeapProfilerState, since only
// startSamplingHeapProfiler() creates one. Both used to dereference that empty
// shared_ptr.

import * as inspector from 'inspector';

import * as v8HeapProfiler from '../src/heap-profiler-bindings';

function post(
  session: inspector.Session,
  method: string,
  params?: object,
): Promise<void> {
  return new Promise((resolve, reject) => {
    session.post(method, params, err => (err ? reject(err) : resolve()));
  });
}

async function main() {
  const session = new inspector.Session();
  session.connect();

  await post(session, 'HeapProfiler.enable');
  await post(session, 'HeapProfiler.startSampling', {samplingInterval: 16384});

  // Allocate so the sampler actually has samples to report. Kept modest and
  // scoped to this function: the profile only needs a non-empty sample set.
  const retained: Array<{i: number; s: string}> = [];
  for (let i = 0; i < 20000; i++) {
    retained.push({i, s: 'x'.repeat(16)});
  }

  // pprof never started the heap profiler, so there is no state for this
  // isolate. Both of these must return rather than crash.
  const profile = v8HeapProfiler.getAllocationProfile();
  if (!profile || typeof profile.name !== 'string') {
    throw new Error('getAllocationProfile returned an unusable profile');
  }

  const mapped = v8HeapProfiler.mapAllocationProfile(node => node.name);
  if (typeof mapped !== 'string') {
    throw new Error('mapAllocationProfile did not invoke the callback');
  }

  await post(session, 'HeapProfiler.stopSampling');
  session.disconnect();
}

main().then(
  () => process.exit(0),
  err => {
    console.error(err);
    process.exit(1);
  },
);
