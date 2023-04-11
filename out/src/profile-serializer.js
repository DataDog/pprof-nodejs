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
exports.serializeHeapProfile = exports.serializeCpuProfile = exports.serializeTimeProfile = void 0;
const pprof_format_1 = require("pprof-format");
function isGeneratedLocation(location) {
    return (location.column !== undefined &&
        location.line !== undefined &&
        location.line > 0);
}
/**
 * Takes v8 profile and populates sample, location, and function fields of
 * profile.proto.
 *
 * @param profile - profile.proto with empty sample, location, and function
 * fields.
 * @param root - root of v8 profile tree describing samples to be appended
 * to profile.
 * @param appendToSamples - function which converts entry to sample(s)  and
 * appends these to end of an array of samples.
 * @param stringTable - string table for the existing profile.
 */
function serialize(profile, root, appendToSamples, stringTable, ignoreSamplesPath, sourceMapper) {
    const samples = [];
    const locations = [];
    const functions = [];
    const functionIdMap = new Map();
    const locationIdMap = new Map();
    const entries = root.children.map((n) => ({
        node: n,
        stack: [],
    }));
    while (entries.length > 0) {
        const entry = entries.pop();
        const node = entry.node;
        if (ignoreSamplesPath && node.scriptName.indexOf(ignoreSamplesPath) > -1) {
            continue;
        }
        if (node.name === '(idle)' || node.name === '(program)')
            continue;
        const stack = entry.stack;
        const location = getLocation(node, sourceMapper);
        stack.unshift(location.id);
        appendToSamples(entry, samples);
        for (const child of node.children) {
            entries.push({ node: child, stack: stack.slice() });
        }
    }
    profile.sample = samples;
    profile.location = locations;
    profile.function = functions;
    profile.stringTable = stringTable;
    function getLocation(node, sourceMapper) {
        let profLoc = {
            file: node.scriptName || '',
            line: node.lineNumber,
            column: node.columnNumber,
            name: node.name,
        };
        if (profLoc.line) {
            if (sourceMapper && isGeneratedLocation(profLoc)) {
                profLoc = sourceMapper.mappingInfo(profLoc);
            }
        }
        const keyStr = `${node.scriptId}:${profLoc.line}:${profLoc.column}:${profLoc.name}`;
        let id = locationIdMap.get(keyStr);
        if (id !== undefined) {
            // id is index+1, since 0 is not valid id.
            return locations[id - 1];
        }
        id = locations.length + 1;
        locationIdMap.set(keyStr, id);
        const line = getLine(node.scriptId, profLoc.file, profLoc.name, profLoc.line);
        const location = new pprof_format_1.Location({ id, line: [line] });
        locations.push(location);
        return location;
    }
    function getLine(scriptId, scriptName, name, line) {
        return new pprof_format_1.Line({
            functionId: getFunction(scriptId, scriptName, name).id,
            line,
        });
    }
    function getFunction(scriptId, scriptName, name) {
        const keyStr = `${scriptId}:${name}`;
        let id = functionIdMap.get(keyStr);
        if (id !== undefined) {
            // id is index+1, since 0 is not valid id.
            return functions[id - 1];
        }
        id = functions.length + 1;
        functionIdMap.set(keyStr, id);
        const nameId = stringTable.dedup(name || '(anonymous)');
        const f = new pprof_format_1.Function({
            id,
            name: nameId,
            systemName: nameId,
            filename: stringTable.dedup(scriptName || ''),
        });
        functions.push(f);
        return f;
    }
}
/**
 * @return value type for sample counts (type:sample, units:count), and
 * adds strings used in this value type to the table.
 */
function createSampleCountValueType(table) {
    return new pprof_format_1.ValueType({
        type: table.dedup('sample'),
        unit: table.dedup('count'),
    });
}
/**
 * @return value type for time samples (type:wall, units:nanoseconds), and
 * adds strings used in this value type to the table.
 */
function createTimeValueType(table) {
    return new pprof_format_1.ValueType({
        type: table.dedup('wall'),
        unit: table.dedup('nanoseconds'),
    });
}
/**
 * @return value type for cpu samples (type:cpu, units:nanoseconds), and
 * adds strings used in this value type to the table.
 */
function createCpuValueType(table) {
    return new pprof_format_1.ValueType({
        type: table.dedup('cpu'),
        unit: table.dedup('nanoseconds'),
    });
}
/**
 * @return value type for object counts (type:objects, units:count), and
 * adds strings used in this value type to the table.
 */
function createObjectCountValueType(table) {
    return new pprof_format_1.ValueType({
        type: table.dedup('objects'),
        unit: table.dedup('count'),
    });
}
/**
 * @return value type for memory allocations (type:space, units:bytes), and
 * adds strings used in this value type to the table.
 */
function createAllocationValueType(table) {
    return new pprof_format_1.ValueType({
        type: table.dedup('space'),
        unit: table.dedup('bytes'),
    });
}
function computeTotalHitCount(root) {
    return (root.hitCount +
        root.children.reduce((sum, node) => sum + computeTotalHitCount(node), 0));
}
/**
 * Converts v8 time profile into into a profile proto.
 * (https://github.com/google/pprof/blob/master/proto/profile.proto)
 *
 * @param prof - profile to be converted.
 * @param intervalMicros - average time (microseconds) between samples.
 */
function serializeTimeProfile(prof, intervalMicros, sourceMapper, recomputeSamplingInterval = false) {
    // If requested, recompute sampling interval from profile duration and total number of hits,
    // since profile duration should be #hits x interval.
    // Recomputing an average interval is more accurate, since in practice intervals between
    // samples are larger than the requested sampling interval (eg. 12.5ms vs 10ms requested).
    // For very short durations, computation becomes meaningless (eg. if there is only one hit),
    // therefore keep intervalMicros as a lower bound and 2 * intervalMicros as upper bound.
    if (recomputeSamplingInterval) {
        const totalHitCount = computeTotalHitCount(prof.topDownRoot);
        if (totalHitCount > 0) {
            intervalMicros = Math.min(Math.max(Math.floor((prof.endTime - prof.startTime) /
                computeTotalHitCount(prof.topDownRoot)), intervalMicros), 2 * intervalMicros);
        }
    }
    const intervalNanos = intervalMicros * 1000;
    const appendTimeEntryToSamples = (entry, samples) => {
        if (entry.node.hitCount > 0) {
            const sample = new pprof_format_1.Sample({
                locationId: entry.stack,
                value: [entry.node.hitCount, entry.node.hitCount * intervalNanos],
            });
            samples.push(sample);
        }
    };
    const stringTable = new pprof_format_1.StringTable();
    const sampleValueType = createSampleCountValueType(stringTable);
    const timeValueType = createTimeValueType(stringTable);
    const profile = {
        sampleType: [sampleValueType, timeValueType],
        timeNanos: Date.now() * 1000 * 1000,
        durationNanos: (prof.endTime - prof.startTime) * 1000,
        periodType: timeValueType,
        period: intervalNanos,
    };
    serialize(profile, prof.topDownRoot, appendTimeEntryToSamples, stringTable, undefined, sourceMapper);
    return new pprof_format_1.Profile(profile);
}
exports.serializeTimeProfile = serializeTimeProfile;
function buildLabels(labelSet, stringTable) {
    const labels = [];
    for (const [key, value] of Object.entries(labelSet)) {
        if (typeof value === 'number' || typeof value === 'string') {
            const label = new pprof_format_1.Label({
                key: stringTable.dedup(key),
                num: typeof value === 'number' ? value : undefined,
                str: typeof value === 'string'
                    ? stringTable.dedup(value)
                    : undefined,
            });
            labels.push(label);
        }
    }
    return labels;
}
/**
 * Converts cpu profile into into a profile proto.
 * (https://github.com/google/pprof/blob/master/proto/profile.proto)
 *
 * @param prof - profile to be converted.
 * @param intervalMicros - average time (microseconds) between samples.
 */
function serializeCpuProfile(prof, intervalMicros, sourceMapper) {
    const intervalNanos = intervalMicros * 1000;
    const appendCpuEntryToSamples = (entry, samples) => {
        for (const labelCpu of entry.node.labelSets) {
            const sample = new pprof_format_1.Sample({
                locationId: entry.stack,
                value: [1, labelCpu.cpuTime],
                label: buildLabels(labelCpu.labels, stringTable),
            });
            samples.push(sample);
        }
        if (entry.node.hitCount > 0) {
            const sample = new pprof_format_1.Sample({
                locationId: entry.stack,
                value: [entry.node.hitCount, entry.node.cpuTime],
            });
            samples.push(sample);
        }
    };
    const stringTable = new pprof_format_1.StringTable();
    const sampleValueType = createSampleCountValueType(stringTable);
    // const wallValueType = createTimeValueType(stringTable);
    const cpuValueType = createCpuValueType(stringTable);
    const profile = {
        sampleType: [sampleValueType, cpuValueType /*, wallValueType*/],
        timeNanos: Date.now() * 1000 * 1000,
        durationNanos: prof.endTime - prof.startTime,
        periodType: cpuValueType,
        period: intervalNanos,
    };
    serialize(profile, prof.topDownRoot, appendCpuEntryToSamples, stringTable, undefined, sourceMapper);
    return new pprof_format_1.Profile(profile);
}
exports.serializeCpuProfile = serializeCpuProfile;
/**
 * Converts v8 heap profile into into a profile proto.
 * (https://github.com/google/pprof/blob/master/proto/profile.proto)
 *
 * @param prof - profile to be converted.
 * @param startTimeNanos - start time of profile, in nanoseconds (POSIX time).
 * @param durationsNanos - duration of the profile (wall clock time) in
 * nanoseconds.
 * @param intervalBytes - bytes allocated between samples.
 */
function serializeHeapProfile(prof, startTimeNanos, intervalBytes, ignoreSamplesPath, sourceMapper) {
    const appendHeapEntryToSamples = (entry, samples) => {
        if (entry.node.allocations.length > 0) {
            for (const alloc of entry.node.allocations) {
                const sample = new pprof_format_1.Sample({
                    locationId: entry.stack,
                    value: [alloc.count, alloc.sizeBytes * alloc.count],
                    // TODO: add tag for allocation size
                });
                samples.push(sample);
            }
        }
    };
    const stringTable = new pprof_format_1.StringTable();
    const sampleValueType = createObjectCountValueType(stringTable);
    const allocationValueType = createAllocationValueType(stringTable);
    const profile = {
        sampleType: [sampleValueType, allocationValueType],
        timeNanos: startTimeNanos,
        periodType: allocationValueType,
        period: intervalBytes,
    };
    serialize(profile, prof, appendHeapEntryToSamples, stringTable, ignoreSamplesPath, sourceMapper);
    return new pprof_format_1.Profile(profile);
}
exports.serializeHeapProfile = serializeHeapProfile;
//# sourceMappingURL=profile-serializer.js.map