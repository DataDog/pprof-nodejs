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

import {__testing} from '../src/runtime';

describe('Runtime detection', () => {
  it('prefers explicit node override', () => {
    const runtime = __testing.detectRuntimeFromInputs({
      envOverride: 'node',
      bunVersion: '1.3.5',
      bunGlobal: {},
    });

    assert.equal(runtime, 'node');
  });

  it('prefers explicit bun override', () => {
    const runtime = __testing.detectRuntimeFromInputs({
      envOverride: 'bun',
      bunVersion: undefined,
      bunGlobal: undefined,
    });

    assert.equal(runtime, 'bun');
  });

  it('detects bun from process.versions.bun', () => {
    const runtime = __testing.detectRuntimeFromInputs({
      envOverride: undefined,
      bunVersion: '1.3.5',
      bunGlobal: undefined,
    });

    assert.equal(runtime, 'bun');
  });

  it('detects bun from globalThis.Bun when bunVersion is unavailable', () => {
    const runtime = __testing.detectRuntimeFromInputs({
      envOverride: undefined,
      bunVersion: undefined,
      bunGlobal: {version: '1.3.5'},
    });

    assert.equal(runtime, 'bun');
  });

  it('defaults to node when bun markers are absent', () => {
    const runtime = __testing.detectRuntimeFromInputs({
      envOverride: undefined,
      bunVersion: undefined,
      bunGlobal: undefined,
    });

    assert.equal(runtime, 'node');
  });
});
