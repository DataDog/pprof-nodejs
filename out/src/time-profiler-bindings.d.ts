export declare const TimeProfiler: new (...args: unknown[]) => {
    start: () => void;
    stop: (restart: boolean) => import("./v8-types").TimeProfile;
    dispose: () => void;
    v8ProfilerStuckEventLoopDetected: () => number;
    context: object | undefined;
    metrics: import("./v8-types").TimeProfilerMetrics;
    state: {
        [key: string]: number;
    };
};
export declare const constants: {
    kSampleCount: string;
};
export declare const getNativeThreadId: () => number;
