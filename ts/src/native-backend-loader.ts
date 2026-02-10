/**
 * Copyright 2026 Datadog
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 */

import {join} from 'path';

import {runtime} from './runtime';
import {
  BunTimeProfiler,
  bunGetAllocationProfile,
  bunGetNativeThreadId,
  bunMonitorOutOfMemory,
  bunStartSamplingHeapProfiler,
  bunStopSamplingHeapProfiler,
} from './bun-native-backend';
import {TimeProfile, TimeProfilerMetrics} from './v8-types';

type NativeModule = {
  TimeProfiler: new (...args: unknown[]) => TimeProfilerInstance;
  constants: {kSampleCount: string};
  getNativeThreadId: () => number;
  heapProfiler: {
    startSamplingHeapProfiler: (
      heapIntervalBytes: number,
      heapStackDepth: number
    ) => void;
    stopSamplingHeapProfiler: () => void;
    getAllocationProfile: () => unknown;
    monitorOutOfMemory: (
      heapLimitExtensionSize: number,
      maxHeapLimitExtensionCount: number,
      dumpHeapProfileOnSdterr: boolean,
      exportCommand: Array<String> | undefined,
      callback: unknown,
      callbackMode: number,
      isMainThread: boolean
    ) => void;
  };
};

type TimeProfilerInstance = {
  start: () => void;
  stop: (restart: boolean) => TimeProfile;
  dispose: () => void;
  v8ProfilerStuckEventLoopDetected: () => number;
  context: object | undefined;
  metrics: TimeProfilerMetrics;
  state: {[key: string]: number};
};

const bunModule: NativeModule = {
  TimeProfiler: BunTimeProfiler,
  constants: {kSampleCount: 'sampleCount'},
  getNativeThreadId: bunGetNativeThreadId,
  heapProfiler: {
    startSamplingHeapProfiler: bunStartSamplingHeapProfiler,
    stopSamplingHeapProfiler: bunStopSamplingHeapProfiler,
    getAllocationProfile: bunGetAllocationProfile,
    monitorOutOfMemory: bunMonitorOutOfMemory,
  },
};

let cachedModule: NativeModule | undefined;

export function loadNativeModule(): NativeModule {
  if (cachedModule) {
    return cachedModule;
  }

  if (runtime === 'bun') {
    cachedModule = bunModule;
    return cachedModule;
  }

  // eslint-disable-next-line @typescript-eslint/no-var-requires
  const findBinding = require('node-gyp-build');
  cachedModule = findBinding(join(__dirname, '..', '..')) as NativeModule;
  return cachedModule;
}
