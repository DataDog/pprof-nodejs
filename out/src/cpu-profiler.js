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
const profile_serializer_1 = require("./profile-serializer");
const cpu_profiler_bindings_1 = require("./cpu-profiler-bindings");
function isNodeEqual(a, b) {
    if (a.name !== b.name)
        return false;
    if (a.scriptName !== b.scriptName)
        return false;
    if (a.scriptId !== b.scriptId)
        return false;
    if (a.lineNumber !== b.lineNumber)
        return false;
    if (a.columnNumber !== b.columnNumber)
        return false;
    return true;
}
function makeNode(location) {
    return {
        name: location.comment || location.functionName,
        scriptName: location.scriptName || '',
        scriptId: location.scriptId,
        lineNumber: location.line,
        columnNumber: location.column,
        hitCount: 0,
        cpuTime: 0,
        labelSets: [],
        children: [],
    };
}
class CpuProfiler extends cpu_profiler_bindings_1.CpuProfiler {
    profile() {
        if (this.frequency === 0)
            return;
        const profile = super.profile();
        const timeProfile = {
            startTime: profile.startTime,
            endTime: profile.endTime,
            topDownRoot: {
                name: '(root)',
                scriptName: '',
                scriptId: 0,
                lineNumber: 0,
                columnNumber: 0,
                hitCount: 0,
                cpuTime: 0,
                labelSets: [],
                children: [],
            },
        };
        let targetNode = timeProfile.topDownRoot;
        for (const sample of profile.samples) {
            if (!sample)
                continue;
            locations: for (const location of sample.locations) {
                const node = makeNode(location);
                for (const found of targetNode.children) {
                    const foundNode = found;
                    if (isNodeEqual(node, foundNode)) {
                        targetNode = foundNode;
                        continue locations;
                    }
                }
                targetNode.children.push(node);
                targetNode = node;
            }
            if (sample.labels && Object.keys(sample.labels).length > 0) {
                targetNode.labelSets.push({
                    labels: sample.labels,
                    cpuTime: sample.cpuTime,
                });
            }
            else {
                targetNode.cpuTime += sample.cpuTime;
                targetNode.hitCount++;
            }
            targetNode = timeProfile.topDownRoot;
        }
        const intervalMicros = 1000000 / this.frequency;
        return (0, profile_serializer_1.serializeCpuProfile)(timeProfile, intervalMicros);
    }
}
exports.default = CpuProfiler;
//# sourceMappingURL=cpu-profiler.js.map