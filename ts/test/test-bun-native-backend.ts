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
import {TimeProfileNode} from '../src/v8-types';

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

  it('records context transitions while sampling', async () => {
    const profiler = new BunTimeProfiler({
      intervalMicros: 1000,
      withContexts: true,
    });
    profiler.start();
    profiler.context = {};
    await delay(5);
    profiler.context = {vehicle: 'car'};
    await delay(5);
    profiler.context = {vehicle: 'car', brand: 'mercedes'};
    await delay(5);

    const profile = profiler.stop(false);
    const node = profile.topDownRoot.children[0] as TimeProfileNode;
    const timeline = node.contexts ?? [];
    const labels = timeline.map(timeContext => timeContext.context ?? {});

    assert.ok(timeline.length >= 3);
    assert.deepEqual(labels[0], {});
    assert.deepEqual(labels[1], {vehicle: 'car'});
    assert.deepEqual(labels[2], {vehicle: 'car', brand: 'mercedes'});
  });

  it('marks profiles with CPU sample metadata when collectCpuTime is enabled', async () => {
    const profiler = new BunTimeProfiler({
      intervalMicros: 1000,
      withContexts: true,
      collectCpuTime: true,
    });
    profiler.start();
    profiler.context = {service: 'bun'};
    await delay(10);

    const profile = profiler.stop(false);
    const node = profile.topDownRoot.children[0] as TimeProfileNode;
    const firstContext = node.contexts?.[0];

    assert.equal(profile.hasCpuTime, true);
    assert.ok(typeof firstContext?.cpuTime === 'number');
    assert.ok((firstContext?.cpuTime ?? 0) >= 0);
  });

  it('handles bigint context labels and deduplicates equivalent context objects', async () => {
    const profiler = new BunTimeProfiler({
      intervalMicros: 1000,
      withContexts: true,
    });
    profiler.start();
    profiler.context = {requestId: 123n, route: '/health'};
    await delay(5);
    profiler.context = {route: '/health', requestId: 123n};
    await delay(5);

    const profile = profiler.stop(false);
    const node = profile.topDownRoot.children[0] as TimeProfileNode;
    const timeline = node.contexts ?? [];

    assert.equal(timeline.length, 1);
    assert.deepEqual(timeline[0].context, {
      requestId: 123n,
      route: '/health',
    });
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
