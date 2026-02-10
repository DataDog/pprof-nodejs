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

  it('loads a non-crashing backend when runtime is forced to bun', () => {
    process.env[RUNTIME_ENV_KEY] = 'bun';
    clearAllProfileModules();
    const pprof = require('../src');

    assert.equal(typeof pprof.time.start, 'function');
    assert.equal(typeof pprof.heap.start, 'function');
    assert.throws(
      () => pprof.time.start({}),
      /does not currently support runtime "bun"/
    );
  });
});
