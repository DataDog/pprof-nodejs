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

function unsupportedRuntimeError() {
  return new Error(
    `@datadog/pprof does not currently support runtime "${runtime}". ` +
      'Use Node.js for profiling until a runtime-specific backend is available.'
  );
}

function unsupportedFunction<T extends (...args: never[]) => unknown>(): T {
  return (() => {
    throw unsupportedRuntimeError();
  }) as unknown as T;
}

function unsupportedClass<T extends new (...args: any[]) => unknown>(): T {
  return class UnsupportedRuntime {
    constructor() {
      throw unsupportedRuntimeError();
    }
  } as T;
}

const unsupportedModule: NativeModule = {
  TimeProfiler: unsupportedClass(),
  constants: {kSampleCount: 'sampleCount'},
  getNativeThreadId: unsupportedFunction(),
  heapProfiler: {
    startSamplingHeapProfiler: unsupportedFunction(),
    stopSamplingHeapProfiler: unsupportedFunction(),
    getAllocationProfile: unsupportedFunction(),
    monitorOutOfMemory: unsupportedFunction(),
  },
};

let cachedModule: NativeModule | undefined;

export function loadNativeModule(): NativeModule {
  if (cachedModule) {
    return cachedModule;
  }

  if (runtime === 'bun') {
    cachedModule = unsupportedModule;
    return cachedModule;
  }

  // eslint-disable-next-line @typescript-eslint/no-var-requires
  const findBinding = require('node-gyp-build');
  cachedModule = findBinding(join(__dirname, '..', '..')) as NativeModule;
  return cachedModule;
}
