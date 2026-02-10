"use strict";
/**
 * Copyright 2017 Google Inc. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
var __importDefault = (this && this.__importDefault) || function (mod) {
    return (mod && mod.__esModule) ? mod : { "default": mod };
};
Object.defineProperty(exports, "__esModule", { value: true });
exports.getNativeThreadId = exports.constants = void 0;
exports.profile = profile;
exports.start = start;
exports.stop = stop;
exports.getState = getState;
exports.setContext = setContext;
exports.getContext = getContext;
exports.getMetrics = getMetrics;
exports.isStarted = isStarted;
exports.v8ProfilerStuckEventLoopDetected = v8ProfilerStuckEventLoopDetected;
const delay_1 = __importDefault(require("delay"));
const profile_serializer_1 = require("./profile-serializer");
const time_profiler_bindings_1 = require("./time-profiler-bindings");
Object.defineProperty(exports, "getNativeThreadId", { enumerable: true, get: function () { return time_profiler_bindings_1.getNativeThreadId; } });
const worker_threads_1 = require("worker_threads");
const { kSampleCount } = time_profiler_bindings_1.constants;
const DEFAULT_INTERVAL_MICROS = 1000;
const DEFAULT_DURATION_MILLIS = 60000;
let gProfiler;
let gSourceMapper;
let gIntervalMicros;
let gV8ProfilerStuckEventLoopDetected = 0;
/** Make sure to stop profiler before node shuts down, otherwise profiling
 * signal might cause a crash if it occurs during shutdown */
process.once('exit', () => {
    if (isStarted())
        stop();
});
const DEFAULT_OPTIONS = {
    durationMillis: DEFAULT_DURATION_MILLIS,
    intervalMicros: DEFAULT_INTERVAL_MICROS,
    lineNumbers: false,
    withContexts: false,
    workaroundV8Bug: true,
    collectCpuTime: false,
    collectAsyncId: false,
    useCPED: false,
};
async function profile(options = {}) {
    options = { ...DEFAULT_OPTIONS, ...options };
    start(options);
    await (0, delay_1.default)(options.durationMillis);
    return stop();
}
// Temporarily retained for backwards compatibility with older tracer
function start(options = {}) {
    options = { ...DEFAULT_OPTIONS, ...options };
    if (gProfiler) {
        throw new Error('Wall profiler is already started');
    }
    gProfiler = new time_profiler_bindings_1.TimeProfiler({ ...options, isMainThread: worker_threads_1.isMainThread });
    gSourceMapper = options.sourceMapper;
    gIntervalMicros = options.intervalMicros;
    gV8ProfilerStuckEventLoopDetected = 0;
    gProfiler.start();
    // If contexts are enabled without using CPED, set an initial empty context
    if (options.withContexts && !options.useCPED) {
        setContext({});
    }
}
function stop(restart = false, generateLabels, lowCardinalityLabels) {
    if (!gProfiler) {
        throw new Error('Wall profiler is not started');
    }
    const profile = gProfiler.stop(restart);
    if (restart) {
        gV8ProfilerStuckEventLoopDetected =
            gProfiler.v8ProfilerStuckEventLoopDetected();
        // Workaround for v8 bug, where profiler event processor thread is stuck in
        // a loop eating 100% CPU, leading to empty profiles.
        // Fully stop and restart the profiler to reset the profile to a valid state.
        if (gV8ProfilerStuckEventLoopDetected > 0) {
            gProfiler.stop(false);
            gProfiler.start();
        }
    }
    else {
        gV8ProfilerStuckEventLoopDetected = 0;
    }
    const serializedProfile = (0, profile_serializer_1.serializeTimeProfile)(profile, gIntervalMicros, gSourceMapper, true, generateLabels, lowCardinalityLabels);
    if (!restart) {
        gProfiler.dispose();
        gProfiler = undefined;
        gSourceMapper = undefined;
    }
    return serializedProfile;
}
function getState() {
    if (!gProfiler) {
        throw new Error('Wall profiler is not started');
    }
    return gProfiler.state;
}
function setContext(context) {
    if (!gProfiler) {
        throw new Error('Wall profiler is not started');
    }
    gProfiler.context = context;
}
function getContext() {
    if (!gProfiler) {
        throw new Error('Wall profiler is not started');
    }
    return gProfiler.context;
}
function getMetrics() {
    if (!gProfiler) {
        throw new Error('Wall profiler is not started');
    }
    return gProfiler.metrics;
}
function isStarted() {
    return !!gProfiler;
}
// Return 0 if no issue detected, 1 if possible issue, 2 if issue detected for certain
function v8ProfilerStuckEventLoopDetected() {
    return gV8ProfilerStuckEventLoopDetected;
}
exports.constants = {
    kSampleCount,
    GARBAGE_COLLECTION_FUNCTION_NAME: profile_serializer_1.GARBAGE_COLLECTION_FUNCTION_NAME,
    NON_JS_THREADS_FUNCTION_NAME: profile_serializer_1.NON_JS_THREADS_FUNCTION_NAME,
};
//# sourceMappingURL=time-profiler.js.map