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
const NANOS_PER_MICRO = 1000;

function nowMicros(): number {
  return Number(process.hrtime.bigint() / 1000n);
}

function nowMicrosBigInt(): bigint {
  return BigInt(Date.now() * MICROS_PER_MILLI);
}

type TimeProfilerCtorArgs = {
  withContexts?: boolean;
  intervalMicros?: number;
  collectCpuTime?: boolean;
};

function cloneContext(context: object | undefined): object | undefined {
  if (!context) {
    return context;
  }
  return {...(context as Record<string, unknown>)};
}

function cpuUsageDeltaNanos(
  current: NodeJS.CpuUsage,
  previous: NodeJS.CpuUsage
): number {
  const userMicros = Math.max(current.user - previous.user, 0);
  const systemMicros = Math.max(current.system - previous.system, 0);
  return (userMicros + systemMicros) * NANOS_PER_MICRO;
}

function stableContextValue(
  value: unknown,
  seen?: WeakSet<object>
): string {
  switch (typeof value) {
    case 'string':
      return `s:${value}`;
    case 'number':
      return `n:${value}`;
    case 'bigint':
      return `bi:${value.toString(10)}`;
    case 'boolean':
      return `b:${value ? 1 : 0}`;
    case 'undefined':
      return 'u:';
    case 'symbol':
      return `sy:${String(value)}`;
    case 'function':
      return `fn:${(value as Function).name ?? ''}`;
    case 'object':
      if (value === null) {
        return 'null:';
      }

      if (Array.isArray(value)) {
        const nextSeen = seen ?? new WeakSet<object>();
        if (nextSeen.has(value)) {
          return 'a:[circular]';
        }
        nextSeen.add(value);
        const encodedItems = value.map(item =>
          stableContextValue(item, nextSeen)
        );
        nextSeen.delete(value);
        return `a:[${encodedItems.join(',')}]`;
      }

      const objectValue = value as Record<string, unknown>;
      const nextSeen = seen ?? new WeakSet<object>();
      if (nextSeen.has(objectValue)) {
        return 'o:[circular]';
      }
      nextSeen.add(objectValue);

      const keys = Object.keys(objectValue).sort();
      let encoded = 'o:{';
      for (const key of keys) {
        encoded += `${key}:${stableContextValue(objectValue[key], nextSeen)};`;
      }
      encoded += '}';

      nextSeen.delete(objectValue);
      return encoded;
    default:
      return `${typeof value}:`;
  }
}

function contextSignature(context: object | undefined): string {
  if (typeof context === 'undefined') {
    return '__undefined__';
  }
  if (context === null) {
    return '__null__';
  }
  const contextObject = context as Record<string, unknown>;
  const contextKeys = Object.keys(contextObject).sort();
  let signature = '';
  for (const key of contextKeys) {
    signature += `${key}\u0000${stableContextValue(contextObject[key])}\u0000`;
  }
  return signature;
}

export class BunTimeProfiler {
  public metrics: TimeProfilerMetrics = {
    usedAsyncContextCount: 0,
    totalAsyncContextCount: 0,
  };
  public state: {[key: string]: number} = {sampleCount: 0};

  private readonly withContexts: boolean;
  private readonly intervalMicros: number;
  private readonly collectCpuTime: boolean;
  private started = false;
  private startTime = 0;
  private contextTimeline: TimeProfileNodeContext[] = [];
  private currentContext: object | undefined;
  private currentContextSignature = contextSignature(undefined);
  private lastRecordedContextSignature = contextSignature(undefined);
  private lastContextCpuUsage: NodeJS.CpuUsage | undefined;

  constructor(...args: unknown[]) {
    const options =
      (args[0] as TimeProfilerCtorArgs | undefined) ??
      ({} as TimeProfilerCtorArgs);
    this.withContexts = options.withContexts === true;
    this.intervalMicros = Math.max(options.intervalMicros ?? 1000, 1);
    this.collectCpuTime = options.collectCpuTime === true;
  }

  get context(): object | undefined {
    return this.currentContext;
  }

  set context(context: object | undefined) {
    const nextContext = cloneContext(context);
    this.currentContext = nextContext;
    this.currentContextSignature = contextSignature(nextContext);
    if (this.started && this.withContexts) {
      if (this.lastRecordedContextSignature === this.currentContextSignature) {
        return;
      }
      this.recordContext(nextContext);
    }
  }

  private recordContext(context: object | undefined) {
    const nextContext: TimeProfileNodeContext = {
      context,
      timestamp: nowMicrosBigInt(),
    };
    if (this.collectCpuTime) {
      const currentCpuUsage = process.cpuUsage();
      if (this.lastContextCpuUsage) {
        nextContext.cpuTime = cpuUsageDeltaNanos(
          currentCpuUsage,
          this.lastContextCpuUsage
        );
      } else {
        nextContext.cpuTime = 0;
      }
      this.lastContextCpuUsage = currentCpuUsage;
    }
    this.contextTimeline.push(nextContext);
    this.lastRecordedContextSignature = contextSignature(context);
  }

  private normalizedContextTimeline(
    endTimestampMicros: bigint
  ): TimeProfileNodeContext[] | undefined {
    if (!this.withContexts || this.contextTimeline.length === 0) {
      return undefined;
    }

    const minSampleWindow = BigInt(this.intervalMicros);
    const normalized: TimeProfileNodeContext[] = [];

    for (let i = 0; i < this.contextTimeline.length; i++) {
      const current = this.contextTimeline[i];
      const nextTimestamp =
        i + 1 < this.contextTimeline.length
          ? this.contextTimeline[i + 1].timestamp
          : endTimestampMicros;
      const durationMicros =
        nextTimestamp > current.timestamp
          ? nextTimestamp - current.timestamp
          : 0n;

      // Ignore ultra-short context flips that are below one sampling period.
      if (durationMicros < minSampleWindow) {
        continue;
      }

      const last = normalized[normalized.length - 1];
      if (
        last &&
        contextSignature(last.context) === contextSignature(current.context)
      ) {
        if (typeof current.cpuTime === 'number') {
          last.cpuTime = (last.cpuTime ?? 0) + current.cpuTime;
        }
        continue;
      }

      normalized.push({
        context: current.context,
        timestamp: current.timestamp,
        cpuTime: current.cpuTime,
      });
    }

    if (normalized.length > 0) {
      return normalized;
    }

    const lastRawContext = this.contextTimeline[this.contextTimeline.length - 1];
    return [
      {
        context: lastRawContext.context,
        timestamp: lastRawContext.timestamp,
        cpuTime: lastRawContext.cpuTime,
      },
    ];
  }

  start() {
    this.started = true;
    this.startTime = nowMicros();
    this.state.sampleCount = 0;
    this.contextTimeline = [];
    this.lastRecordedContextSignature = contextSignature(undefined);
    this.lastContextCpuUsage = this.collectCpuTime
      ? process.cpuUsage()
      : undefined;
    if (this.withContexts && this.currentContext) {
      this.recordContext(this.currentContext);
    }
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

    const stopTimestampMicros = nowMicrosBigInt();
    const context = this.normalizedContextTimeline(stopTimestampMicros);

    if (this.collectCpuTime) {
      const currentCpuUsage = process.cpuUsage();
      if (context && context.length > 0 && this.lastContextCpuUsage) {
        const lastContext = context[context.length - 1];
        const cpuTime = cpuUsageDeltaNanos(
          currentCpuUsage,
          this.lastContextCpuUsage
        );
        lastContext.cpuTime = (lastContext.cpuTime ?? 0) + cpuTime;
      }
      this.lastContextCpuUsage = currentCpuUsage;
    }

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
      hasCpuTime: this.collectCpuTime,
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
      this.state.sampleCount = 0;
      this.contextTimeline = [];
      this.lastRecordedContextSignature = contextSignature(undefined);
      this.lastContextCpuUsage = this.collectCpuTime
        ? process.cpuUsage()
        : undefined;
      if (this.withContexts && this.currentContext) {
        this.recordContext(this.currentContext);
      }
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
