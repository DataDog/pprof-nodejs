"use strict";
Object.defineProperty(exports, "__esModule", { value: true });
exports.startSamplingHeapProfiler = startSamplingHeapProfiler;
exports.stopSamplingHeapProfiler = stopSamplingHeapProfiler;
exports.getAllocationProfile = getAllocationProfile;
exports.monitorOutOfMemory = monitorOutOfMemory;
const native_backend_loader_1 = require("./native-backend-loader");
const profiler = (0, native_backend_loader_1.loadNativeModule)();
// Wrappers around native heap profiler functions.
function startSamplingHeapProfiler(heapIntervalBytes, heapStackDepth) {
    profiler.heapProfiler.startSamplingHeapProfiler(heapIntervalBytes, heapStackDepth);
}
function stopSamplingHeapProfiler() {
    profiler.heapProfiler.stopSamplingHeapProfiler();
}
function getAllocationProfile() {
    return profiler.heapProfiler.getAllocationProfile();
}
function monitorOutOfMemory(heapLimitExtensionSize, maxHeapLimitExtensionCount, dumpHeapProfileOnSdterr, exportCommand, callback, callbackMode, isMainThread) {
    profiler.heapProfiler.monitorOutOfMemory(heapLimitExtensionSize, maxHeapLimitExtensionCount, dumpHeapProfileOnSdterr, exportCommand, callback, callbackMode, isMainThread);
}
//# sourceMappingURL=heap-profiler-bindings.js.map