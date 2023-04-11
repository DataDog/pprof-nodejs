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
Object.defineProperty(exports, "__esModule", { value: true });
exports.start = exports.profile = void 0;
const delay_1 = require("delay");
const profile_serializer_1 = require("./profile-serializer");
const time_profiler_bindings_1 = require("./time-profiler-bindings");
const DEFAULT_INTERVAL_MICROS = 1000;
const majorVersion = process.version.slice(1).split('.').map(Number)[0];
async function profile(options) {
    const stop = start(options.intervalMicros || DEFAULT_INTERVAL_MICROS, options.name, options.sourceMapper, options.lineNumbers);
    await (0, delay_1.default)(options.durationMillis);
    return stop();
}
exports.profile = profile;
function ensureRunName(name) {
    return name || `pprof-${Date.now()}-${Math.random()}`;
}
// NOTE: refreshing doesn't work if giving a profile name.
function start(intervalMicros = DEFAULT_INTERVAL_MICROS, name, sourceMapper, lineNumbers = true) {
    const profiler = new time_profiler_bindings_1.TimeProfiler(intervalMicros);
    let runName = start();
    return majorVersion < 16 ? stopOld : stop;
    function start() {
        const runName = ensureRunName(name);
        profiler.start(runName, lineNumbers);
        return runName;
    }
    // Node.js versions prior to v16 leak memory if not disposed and recreated
    // between each profile. As disposing deletes current profile data too,
    // we must stop then dispose then start.
    function stopOld(restart = false) {
        const result = profiler.stop(runName, lineNumbers);
        profiler.dispose();
        if (restart) {
            runName = start();
        }
        return (0, profile_serializer_1.serializeTimeProfile)(result, intervalMicros, sourceMapper, true);
    }
    // For Node.js v16+, we want to start the next profile before we stop the
    // current one as otherwise the active profile count could reach zero which
    // means V8 might tear down the symbolizer thread and need to start it again.
    function stop(restart = false) {
        let nextRunName;
        if (restart) {
            nextRunName = start();
        }
        const result = profiler.stop(runName, lineNumbers);
        if (nextRunName) {
            runName = nextRunName;
        }
        if (!restart)
            profiler.dispose();
        return (0, profile_serializer_1.serializeTimeProfile)(result, intervalMicros, sourceMapper, true);
    }
}
exports.start = start;
//# sourceMappingURL=time-profiler.js.map