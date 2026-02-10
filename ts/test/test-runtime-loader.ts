/**
 * Copyright 2026 Datadog
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 */

import assert from 'assert';

const RUNTIME_ENV_KEY = 'DATADOG_PPROF_RUNTIME';

function clearModule(modulePath: string) {
  const resolved = require.resolve(modulePath);
  delete require.cache[resolved];
}

describe('Runtime Loader', () => {
  const originalRuntime = process.env[RUNTIME_ENV_KEY];

  function restoreRuntimeEnv() {
    if (typeof originalRuntime !== 'undefined') {
      process.env[RUNTIME_ENV_KEY] = originalRuntime;
      return;
    }

    delete process.env[RUNTIME_ENV_KEY];
  }

  function clearAllProfileModules() {
    clearModule('../src/runtime');
    clearModule('../src/native-backend-loader');
    clearModule('../src/time-profiler-bindings');
    clearModule('../src/heap-profiler-bindings');
    clearModule('../src/time-profiler');
    clearModule('../src/heap-profiler');
    clearModule('../src/index');
  }

  afterEach(() => {
    restoreRuntimeEnv();
    clearAllProfileModules();
  });

  it('loads a bun-compatible backend when runtime is forced to bun', async () => {
    process.env[RUNTIME_ENV_KEY] = 'bun';
    clearAllProfileModules();
    const pprof = require('../src');

    assert.equal(typeof pprof.time.start, 'function');
    assert.equal(typeof pprof.heap.start, 'function');

    pprof.time.start({withContexts: true, intervalMicros: 1_000});
    pprof.time.setContext({service: 'bun-smoke'});
    const timeProfile = pprof.time.stop();
    assert.ok(timeProfile.sample.length > 0);
    const encodedTime = await pprof.encode(timeProfile);
    assert.ok(encodedTime.length > 0);

    pprof.heap.start(128 * 1024, 64);
    const heapProfile = pprof.heap.profile();
    pprof.heap.stop();
    assert.ok(heapProfile.sample.length > 0);
    const encodedHeap = await pprof.encode(heapProfile);
    assert.ok(encodedHeap.length > 0);
  });
});
