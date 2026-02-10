/**
 * Copyright 2026 Datadog
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 */

import {
  AllocationProfileNode,
  TimeProfile,
  TimeProfileNodeContext,
  TimeProfilerMetrics,
} from './v8-types';

const MICROS_PER_MILLI = 1000;

function nowMicros(): number {
  return Number(process.hrtime.bigint() / 1000n);
}

function nowMicrosBigInt(): bigint {
  return BigInt(Date.now() * MICROS_PER_MILLI);
}

type TimeProfilerCtorArgs = {
  withContexts?: boolean;
  intervalMicros?: number;
};

export class BunTimeProfiler {
  public context: object | undefined;
  public metrics: TimeProfilerMetrics = {
    usedAsyncContextCount: 0,
    totalAsyncContextCount: 0,
  };
  public state: {[key: string]: number} = {sampleCount: 0};

  private readonly withContexts: boolean;
  private readonly intervalMicros: number;
  private started = false;
  private startTime = 0;

  constructor(...args: unknown[]) {
    const options =
      (args[0] as TimeProfilerCtorArgs | undefined) ??
      ({} as TimeProfilerCtorArgs);
    this.withContexts = options.withContexts === true;
    this.intervalMicros = Math.max(options.intervalMicros ?? 1000, 1);
  }

  start() {
    this.started = true;
    this.startTime = nowMicros();
    this.state.sampleCount = 0;
  }

  stop(restart: boolean): TimeProfile {
    if (!this.started) {
      throw new Error('Wall profiler is not started');
    }

    const windowStartTime = this.startTime;
    const endTime = nowMicros();
    const elapsedMicros = Math.max(endTime - windowStartTime, 1);
    const estimatedSamples = Math.max(
      Math.floor(elapsedMicros / this.intervalMicros),
      1
    );
    this.state.sampleCount = estimatedSamples;

    const context: TimeProfileNodeContext[] | undefined =
      this.withContexts && this.context
        ? [
            {
              context: this.context,
              timestamp: nowMicrosBigInt(),
            },
          ]
        : undefined;

    const timeNode = {
      name: 'Bun runtime',
      scriptName: '',
      scriptId: 0,
      lineNumber: 0,
      columnNumber: 0,
      hitCount: estimatedSamples,
      contexts: context,
      children: [],
    };

    const result = {
      startTime: windowStartTime,
      endTime,
      topDownRoot: {
        name: '(root)',
        scriptName: '',
        scriptId: 0,
        lineNumber: 0,
        columnNumber: 0,
        hitCount: 0,
        children: [timeNode],
      },
    };

    if (restart) {
      this.startTime = endTime;
    }

    return result;
  }

  dispose() {
    this.started = false;
  }

  v8ProfilerStuckEventLoopDetected() {
    return 0;
  }
}

export function bunGetNativeThreadId() {
  return process.pid;
}

let heapSamplingEnabled = false;

export function bunStartSamplingHeapProfiler() {
  heapSamplingEnabled = true;
}

export function bunStopSamplingHeapProfiler() {
  heapSamplingEnabled = false;
}

export function bunGetAllocationProfile(): AllocationProfileNode {
  if (!heapSamplingEnabled) {
    throw new Error('Heap profiler is not enabled.');
  }

  const usage = process.memoryUsage();
  const heapNode: AllocationProfileNode = {
    name: 'Bun heap',
    scriptName: '',
    scriptId: 0,
    lineNumber: 0,
    columnNumber: 0,
    allocations: [
      {
        sizeBytes: Math.max(usage.heapUsed, 1),
        count: 1,
      },
    ],
    children: [],
  };

  return {
    name: '(root)',
    scriptName: '',
    scriptId: 0,
    lineNumber: 0,
    columnNumber: 0,
    allocations: [],
    children: [heapNode],
  };
}

export function bunMonitorOutOfMemory(
  _heapLimitExtensionSize: number,
  _maxHeapLimitExtensionCount: number,
  _dumpHeapProfileOnSdterr: boolean,
  _exportCommand: Array<String> | undefined,
  _callback: unknown,
  _callbackMode: number,
  _isMainThread: boolean
) {
  // Bun does not expose a near-heap-limit callback hook equivalent to V8's native API.
  // Keep this as a no-op to avoid emitting synthetic OOM events or spurious profiles.
}
