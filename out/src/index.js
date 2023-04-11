"use strict";
Object.defineProperty(exports, "__esModule", { value: true });
exports.heap = exports.time = exports.CpuProfiler = exports.SourceMapper = exports.encodeSync = exports.encode = void 0;
/**
 * Copyright 2019 Google Inc. All Rights Reserved.
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
const fs_1 = require("fs");
const cpu_profiler_1 = require("./cpu-profiler");
const heapProfiler = require("./heap-profiler");
const profile_encoder_1 = require("./profile-encoder");
const timeProfiler = require("./time-profiler");
var profile_encoder_2 = require("./profile-encoder");
Object.defineProperty(exports, "encode", { enumerable: true, get: function () { return profile_encoder_2.encode; } });
Object.defineProperty(exports, "encodeSync", { enumerable: true, get: function () { return profile_encoder_2.encodeSync; } });
var sourcemapper_1 = require("./sourcemapper/sourcemapper");
Object.defineProperty(exports, "SourceMapper", { enumerable: true, get: function () { return sourcemapper_1.SourceMapper; } });
exports.CpuProfiler = cpu_profiler_1.default;
exports.time = {
    profile: timeProfiler.profile,
    start: timeProfiler.start,
};
exports.heap = {
    start: heapProfiler.start,
    stop: heapProfiler.stop,
    profile: heapProfiler.profile,
    convertProfile: heapProfiler.convertProfile,
    v8Profile: heapProfiler.v8Profile,
    monitorOutOfMemory: heapProfiler.monitorOutOfMemory,
    CallbackMode: heapProfiler.CallbackMode,
};
// If loaded with --require, start profiling.
if (module.parent && module.parent.id === 'internal/preload') {
    const stop = exports.time.start();
    process.on('exit', () => {
        // The process is going to terminate imminently. All work here needs to
        // be synchronous.
        const profile = stop();
        const buffer = (0, profile_encoder_1.encodeSync)(profile);
        (0, fs_1.writeFileSync)(`pprof-profile-${process.pid}.pb.gz`, buffer);
    });
}
//# sourceMappingURL=index.js.map