import cpuProfiler from './cpu-profiler';
import * as heapProfiler from './heap-profiler';
import * as timeProfiler from './time-profiler';
export { AllocationProfileNode, TimeProfileNode, ProfileNode } from './v8-types';
export { encode, encodeSync } from './profile-encoder';
export { SourceMapper } from './sourcemapper/sourcemapper';
export declare const CpuProfiler: typeof cpuProfiler;
export declare const time: {
    profile: typeof timeProfiler.profile;
    start: typeof timeProfiler.start;
};
export declare const heap: {
    start: typeof heapProfiler.start;
    stop: typeof heapProfiler.stop;
    profile: typeof heapProfiler.profile;
    convertProfile: typeof heapProfiler.convertProfile;
    v8Profile: typeof heapProfiler.v8Profile;
    monitorOutOfMemory: typeof heapProfiler.monitorOutOfMemory;
    CallbackMode: {
        Async: number;
        Interrupt: number;
        Both: number;
    };
};
