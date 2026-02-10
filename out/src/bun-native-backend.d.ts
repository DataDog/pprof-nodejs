/**
 * Copyright 2026 Datadog
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 */
import { AllocationProfileNode, TimeProfile, TimeProfilerMetrics } from './v8-types';
export declare class BunTimeProfiler {
    metrics: TimeProfilerMetrics;
    state: {
        [key: string]: number;
    };
    private readonly withContexts;
    private readonly intervalMicros;
    private readonly collectCpuTime;
    private started;
    private startTime;
    private contextTimeline;
    private currentContext;
    private currentContextSignature;
    private lastRecordedContextSignature;
    private lastContextCpuUsage;
    constructor(...args: unknown[]);
    get context(): object | undefined;
    set context(context: object | undefined);
    private recordContext;
    private normalizedContextTimeline;
    start(): void;
    stop(restart: boolean): TimeProfile;
    dispose(): void;
    v8ProfilerStuckEventLoopDetected(): number;
}
export declare function bunGetNativeThreadId(): number;
export declare function bunStartSamplingHeapProfiler(): void;
export declare function bunStopSamplingHeapProfiler(): void;
export declare function bunGetAllocationProfile(): AllocationProfileNode;
export declare function bunMonitorOutOfMemory(_heapLimitExtensionSize: number, _maxHeapLimitExtensionCount: number, _dumpHeapProfileOnSdterr: boolean, _exportCommand: Array<String> | undefined, _callback: unknown, _callbackMode: number, _isMainThread: boolean): void;
