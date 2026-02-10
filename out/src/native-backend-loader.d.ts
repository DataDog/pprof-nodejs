/**
 * Copyright 2026 Datadog
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 */
import { TimeProfile, TimeProfilerMetrics } from './v8-types';
type NativeModule = {
    TimeProfiler: new (...args: unknown[]) => TimeProfilerInstance;
    constants: {
        kSampleCount: string;
    };
    getNativeThreadId: () => number;
    heapProfiler: {
        startSamplingHeapProfiler: (heapIntervalBytes: number, heapStackDepth: number) => void;
        stopSamplingHeapProfiler: () => void;
        getAllocationProfile: () => unknown;
        monitorOutOfMemory: (heapLimitExtensionSize: number, maxHeapLimitExtensionCount: number, dumpHeapProfileOnSdterr: boolean, exportCommand: Array<String> | undefined, callback: unknown, callbackMode: number, isMainThread: boolean) => void;
    };
};
type TimeProfilerInstance = {
    start: () => void;
    stop: (restart: boolean) => TimeProfile;
    dispose: () => void;
    v8ProfilerStuckEventLoopDetected: () => number;
    context: object | undefined;
    metrics: TimeProfilerMetrics;
    state: {
        [key: string]: number;
    };
};
export declare function loadNativeModule(): NativeModule;
declare function loadNodeNativeModule(rootDir: string, findBinding: (rootPath: string) => NativeModule, nodeRequire: (modulePath: string) => unknown): NativeModule;
export declare const __testing: {
    loadNodeNativeModule: typeof loadNodeNativeModule;
};
export {};
