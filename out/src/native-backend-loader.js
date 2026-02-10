"use strict";
/**
 * Copyright 2026 Datadog
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 */
Object.defineProperty(exports, "__esModule", { value: true });
exports.__testing = void 0;
exports.loadNativeModule = loadNativeModule;
const fs_1 = require("fs");
const path_1 = require("path");
const runtime_1 = require("./runtime");
const bun_native_backend_1 = require("./bun-native-backend");
const bunModule = {
    TimeProfiler: bun_native_backend_1.BunTimeProfiler,
    constants: { kSampleCount: 'sampleCount' },
    getNativeThreadId: bun_native_backend_1.bunGetNativeThreadId,
    heapProfiler: {
        startSamplingHeapProfiler: bun_native_backend_1.bunStartSamplingHeapProfiler,
        stopSamplingHeapProfiler: bun_native_backend_1.bunStopSamplingHeapProfiler,
        getAllocationProfile: bun_native_backend_1.bunGetAllocationProfile,
        monitorOutOfMemory: bun_native_backend_1.bunMonitorOutOfMemory,
    },
};
let cachedModule;
function loadNativeModule() {
    if (cachedModule) {
        return cachedModule;
    }
    if (runtime_1.runtime === 'bun') {
        cachedModule = bunModule;
        return cachedModule;
    }
    const rootDir = (0, path_1.join)(__dirname, '..', '..');
    // eslint-disable-next-line @typescript-eslint/no-var-requires
    const findBinding = require('node-gyp-build');
    cachedModule = loadNodeNativeModule(rootDir, findBinding, require);
    return cachedModule;
}
function loadNodeNativeModule(rootDir, findBinding, nodeRequire) {
    try {
        return findBinding(rootDir);
    }
    catch (error) {
        if (!isMissingNativeBuildError(error) ||
            !hasLocalNativeBuildArtifacts(rootDir)) {
            throw error;
        }
        return loadFromBuildRelease(rootDir, nodeRequire);
    }
}
function isMissingNativeBuildError(error) {
    return (error instanceof Error &&
        error.message.includes('No native build was found for runtime='));
}
function hasLocalNativeBuildArtifacts(rootDir) {
    return (0, fs_1.existsSync)((0, path_1.join)(rootDir, 'build', 'Release'));
}
function loadFromBuildRelease(rootDir, nodeRequire) {
    const releaseDir = (0, path_1.join)(rootDir, 'build', 'Release');
    const preferredPath = (0, path_1.join)(releaseDir, 'dd_pprof.node');
    if ((0, fs_1.existsSync)(preferredPath)) {
        return nodeRequire(preferredPath);
    }
    const candidates = (0, fs_1.readdirSync)(releaseDir)
        .filter(name => name.endsWith('.node'))
        .sort();
    if (candidates.length === 0) {
        throw new Error(`No native .node artifact found under ${releaseDir} after fallback`);
    }
    return nodeRequire((0, path_1.join)(releaseDir, candidates[0]));
}
exports.__testing = {
    loadNodeNativeModule,
};
//# sourceMappingURL=native-backend-loader.js.map