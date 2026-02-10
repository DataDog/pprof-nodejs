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
import delay from 'delay';

import {BunTimeProfiler, bunMonitorOutOfMemory} from '../src/bun-native-backend';

describe('BunTimeProfiler', () => {
  it('uses microseconds for profile start/end timestamps', async () => {
    const profiler = new BunTimeProfiler({intervalMicros: 1000});
    profiler.start();
    await delay(20);

    const profile = profiler.stop(false);
    const durationMicros = profile.endTime - profile.startTime;

    assert.ok(durationMicros > 10_000);
    assert.ok(durationMicros < 500_000);
  });

  it('advances start time when stop is called with restart=true', async () => {
    const profiler = new BunTimeProfiler({intervalMicros: 1000});
    profiler.start();
    await delay(10);
    const first = profiler.stop(true);
    await delay(10);
    const second = profiler.stop(true);
    profiler.dispose();

    assert.ok(first.startTime < first.endTime);
    assert.ok(second.startTime >= first.endTime);
    assert.ok(second.startTime < second.endTime);
  });
});

describe('bunMonitorOutOfMemory', () => {
  it('is a no-op on Bun and does not invoke callback', () => {
    let callbackInvocations = 0;

    bunMonitorOutOfMemory(
      1024,
      1,
      false,
      [],
      () => {
        callbackInvocations++;
      },
      1,
      true
    );

    assert.equal(callbackInvocations, 0);
  });
});
