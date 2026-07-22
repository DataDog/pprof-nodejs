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

import {
  Function,
  Label,
  LabelInput,
  Line,
  Location,
  Profile,
  Sample,
  ValueType,
  StringTable,
  ProfileInput,
} from 'pprof-format';
import {
  GeneratedLocation,
  SourceLocation,
  SourceMapper,
} from './sourcemapper/sourcemapper';
import {
  Allocation,
  AllocationWithStats,
  AllocationProfileNode,
  AllocationProfileNodeWithStats,
  GenerateAllocationLabelsFunction,
  GenerateTimeLabelsFunction,
  ProfileNode,
  TimeProfile,
  TimeProfileNode,
} from './v8-types';

export const NON_JS_THREADS_FUNCTION_NAME = 'Non JS threads activity';
export const GARBAGE_COLLECTION_FUNCTION_NAME = 'Garbage Collection';

/**
 * A stack of function IDs.
 */
type Stack = number[];

/**
 * A function which converts entry into one or more samples, then
 * appends those sample(s) to samples.
 */
type AppendEntryToSamples<T extends ProfileNode> = (
  entry: Entry<T>,
  samples: Sample[],
) => void;

/**
 * Profile node and stack trace to that node.
 */
interface Entry<T extends ProfileNode> {
  node: T;
  stack: Stack;
}

function isGeneratedLocation(
  location: SourceLocation,
): location is GeneratedLocation {
  return (
    location.column !== undefined &&
    location.line !== undefined &&
    location.line > 0
  );
}

/**
 * Controls how a frame's column number is represented in the emitted profile.
 * - `'drop'`: omit the column; only the line number is emitted. This is the
 *   default and matches the historical behavior and standard pprof line
 *   semantics, so existing consumers see unchanged line numbers.
 * - `'pack'`: pack the column into the high 32 bits of the line field
 *   (`column << 32 | line`) — the encoding the Datadog deobfuscation backend
 *   decodes (the same one the Chrome profile intake uses). Required to
 *   deobfuscate single-line (bundled/minified) frames, where the column is the
 *   only discriminator between functions.
 *
 * The enum leaves room for a future `'emit'` mode that would populate a real
 * pprof `Line.column` field once the backend consumes it.
 */
export type ColumnNumbers = 'drop' | 'pack';

export const DEFAULT_COLUMN_NUMBERS: ColumnNumbers = 'drop';

/**
 * Packs a frame's line and column into the single value carried by the pprof
 * Line.line field: the column in the high 32 bits, the line in the low 32 bits
 * (matching the backend's JavaScriptProfileUnminifier.decodeLineAndColumn).
 *
 * BigInt is required: JavaScript's bitwise operators are 32-bit, so `column <<
 * 32` on a plain number is a no-op that returns `column` unchanged. Computing
 * the pack arithmetically as `column * 2**32 + line` instead loses integer
 * precision once the column exceeds 2**21 (the product passes 2**53, Number's
 * safe-integer limit) — and minified single-line bundles routinely have columns
 * that large.
 */
function packLineAndColumn(
  line: number | undefined,
  column: number | undefined,
): number | bigint {
  if (!line) {
    return 0;
  }
  if (!column) {
    return line;
  }
  return (BigInt(column) << 32n) | BigInt(line);
}

function requireAllocationStat(
  value: number | undefined,
  field: string,
): number {
  if (value === undefined) {
    throw new Error(`Allocation profile is missing ${field}.`);
  }
  return value;
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
function serialize<T extends ProfileNode>(
  profile: ProfileInput,
  root: T,
  appendToSamples: AppendEntryToSamples<T>,
  stringTable: StringTable,
  ignoreSamplesPath?: string,
  sourceMapper?: SourceMapper,
  columnNumbers: ColumnNumbers = DEFAULT_COLUMN_NUMBERS,
) {
  const samples: Sample[] = [];
  const locations: Location[] = [];
  const functions: Function[] = [];
  const functionIdMap = new Map<string, number>();
  const locationIdMap = new Map<string, number>();

  let hasMissingMapFiles = false;

  const entries: Array<Entry<T>> = (root.children as T[]).map((n: T) => ({
    node: n,
    stack: [],
  }));
  while (entries.length > 0) {
    const entry = entries.pop()!;
    const node = entry.node;

    // mjs files have a `file://` prefix in the scriptName -> remove it
    const scriptName = node.scriptName.startsWith('file://')
      ? node.scriptName.slice(7)
      : node.scriptName;

    if (ignoreSamplesPath && scriptName.indexOf(ignoreSamplesPath) > -1) {
      continue;
    }
    const stack = entry.stack;
    const location = getLocation(node, scriptName, sourceMapper);
    stack.unshift(location.id as number);
    appendToSamples(entry, samples);
    for (const child of node.children as T[]) {
      entries.push({node: child, stack: stack.slice()});
    }
  }

  profile.sample = samples;
  profile.location = locations;
  profile.function = functions;
  profile.stringTable = stringTable;

  if (hasMissingMapFiles) {
    profile.comment = [stringTable.dedup('dd:has-missing-map-files')];
  }

  function getLocation(
    node: ProfileNode,
    scriptName: string,
    sourceMapper?: SourceMapper,
  ): Location {
    let profLoc: SourceLocation = {
      file: scriptName || '',
      line: node.lineNumber,
      column: node.columnNumber,
      name: node.name,
    };

    if (profLoc.line) {
      if (sourceMapper && isGeneratedLocation(profLoc)) {
        profLoc = sourceMapper.mappingInfo(profLoc);
        if (profLoc.missingMapFile) {
          hasMissingMapFiles = true;
        }
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
    const line = getLine(profLoc, node.scriptId);
    const location = new Location({id, line: [line]});
    locations.push(location);
    return location;
  }

  function getLine(loc: SourceLocation, scriptId?: number): Line {
    // Only pack the column for frames whose source map was declared but missing
    // locally — i.e. exactly the frames that will be sent for server-side
    // unminification (the same condition that sets dd:has-missing-map-files).
    // Locally-resolved frames keep their plain line, so packed values never
    // reach profiles that skip server-side unminification.
    const packColumn = columnNumbers === 'pack' && loc.missingMapFile === true;
    return new Line({
      functionId: getFunction(loc, scriptId).id,
      line: packColumn ? packLineAndColumn(loc.line, loc.column) : loc.line,
    });
  }

  function getFunction(loc: SourceLocation, scriptId?: number): Function {
    let name = loc.name;
    const keyStr = name
      ? `${scriptId}:${name}`
      : `${scriptId}:${loc.line}:${loc.column}`;
    let id = functionIdMap.get(keyStr);
    if (id !== undefined) {
      // id is index+1, since 0 is not valid id.
      return functions[id - 1];
    }
    id = functions.length + 1;
    functionIdMap.set(keyStr, id);
    if (!name) {
      if (loc.line) {
        if (loc.column) {
          name = `(anonymous:L#${loc.line}:C#${loc.column})`;
        } else {
          name = `(anonymous:L#${loc.line})`;
        }
      } else {
        name = '(anonymous)';
      }
    }
    const nameId = stringTable.dedup(name);
    const f = new Function({
      id,
      name: nameId,
      systemName: nameId,
      filename: stringTable.dedup(loc.file || ''),
    });
    functions.push(f);
    return f;
  }
}

/**
 * @return value type for sample counts (type:sample, units:count), and
 * adds strings used in this value type to the table.
 */
function createSampleCountValueType(table: StringTable): ValueType {
  return new ValueType({
    type: table.dedup('sample'),
    unit: table.dedup('count'),
  });
}

/**
 * @return value type for time samples (type:wall, units:nanoseconds), and
 * adds strings used in this value type to the table.
 */
function createTimeValueType(table: StringTable): ValueType {
  return new ValueType({
    type: table.dedup('wall'),
    unit: table.dedup('nanoseconds'),
  });
}

/**
 * @return value type for cpu samples (type:cpu, units:nanoseconds), and
 * adds strings used in this value type to the table.
 */
function createCpuValueType(table: StringTable): ValueType {
  return new ValueType({
    type: table.dedup('cpu'),
    unit: table.dedup('nanoseconds'),
  });
}

function createInUseObjectCountValueType(table: StringTable): ValueType {
  return new ValueType({
    type: table.dedup('inuse_objects'),
    unit: table.dedup('count'),
  });
}

function createAllocatedObjectCountValueType(table: StringTable): ValueType {
  return new ValueType({
    type: table.dedup('alloc_objects'),
    unit: table.dedup('count'),
  });
}

function createInUseSpaceValueType(table: StringTable): ValueType {
  return new ValueType({
    type: table.dedup('inuse_space'),
    unit: table.dedup('bytes'),
  });
}

function createAllocatedSpaceValueType(table: StringTable): ValueType {
  return new ValueType({
    type: table.dedup('alloc_space'),
    unit: table.dedup('bytes'),
  });
}

export function computeTotalHitCount(root: TimeProfileNode): number {
  return (
    root.hitCount +
    (root.children as TimeProfileNode[]).reduce(
      (sum, node) => sum + computeTotalHitCount(node),
      0,
    )
  );
}

/** Perform some modifications on time profile:
 *  - Add non-JS thread activity node if available
 *  - remove `(program)` nodes
 *  - remove `(idle)` nodes with no context
 *  - set `(idle)` nodes' wall time to zero when they have a context
 *  - Convert `(garbage collector)` node to `Garbage Collection`
 *  - Put `non-JS thread activity` node and `Garbage Collection` under a top level `Node.js` node
 * This function does not change the input profile.
 */
function updateTimeProfile(prof: TimeProfile): TimeProfile {
  const newTopLevelChildren: TimeProfileNode[] = [];

  let runtimeNode: TimeProfileNode | undefined;

  function getRuntimeNode(): TimeProfileNode {
    if (!runtimeNode) {
      runtimeNode = {
        name: 'Node.js',
        scriptName: '',
        scriptId: 0,
        lineNumber: 0,
        columnNumber: 0,
        children: [],
        hitCount: 0,
      };
      newTopLevelChildren.push(runtimeNode);
    }
    return runtimeNode;
  }

  for (const child of prof.topDownRoot.children as TimeProfileNode[]) {
    if (child.name === '(program)') {
      continue;
    }
    if (child.name === '(idle)' && child.contexts?.length === 0) {
      continue;
    }
    if (child.name === '(garbage collector)') {
      // Create a new node to avoid modifying the input one
      const newChild: TimeProfileNode = {
        ...child,
        name: GARBAGE_COLLECTION_FUNCTION_NAME,
      };
      getRuntimeNode().children.push(newChild);
    } else {
      newTopLevelChildren.push(child);
    }
  }

  if (prof.hasCpuTime && prof.nonJSThreadsCpuTime) {
    const node: TimeProfileNode = {
      name: NON_JS_THREADS_FUNCTION_NAME,
      scriptName: '',
      scriptId: 0,
      lineNumber: 0,
      columnNumber: 0,
      children: [],
      hitCount: 0, // 0 because this should not be accounted for wall time
      contexts: [
        {
          context: {},
          timestamp: BigInt(0),
          cpuTime: prof.nonJSThreadsCpuTime,
          asyncId: -1,
        },
      ],
    };
    getRuntimeNode().children.push(node);
  }
  return {
    ...prof,
    topDownRoot: {...prof.topDownRoot, children: newTopLevelChildren},
  };
}

/**
 * Converts v8 time profile into into a profile proto.
 * (https://github.com/google/pprof/blob/master/proto/profile.proto)
 *
 * @param prof - profile to be converted.
 * @param intervalMicros - average time (microseconds) between samples.
 */
export function serializeTimeProfile(
  prof: TimeProfile,
  intervalMicros: number,
  sourceMapper?: SourceMapper,
  recomputeSamplingInterval = false,
  generateLabels?: GenerateTimeLabelsFunction,
  lowCardinalityLabels: string[] = [],
  columnNumbers: ColumnNumbers = DEFAULT_COLUMN_NUMBERS,
): Profile {
  // If requested, recompute sampling interval from profile duration and total number of hits,
  // since profile duration should be #hits x interval.
  // Recomputing an average interval is more accurate, since in practice intervals between
  // samples are larger than the requested sampling interval (eg. 12.5ms vs 10ms requested).
  // For very short durations, computation becomes meaningless (eg. if there is only one hit),
  // therefore keep intervalMicros as a lower bound and 2 * intervalMicros as upper bound.
  if (recomputeSamplingInterval) {
    const totalHitCount =
      prof.totalHitCount ?? computeTotalHitCount(prof.topDownRoot);
    if (totalHitCount > 0) {
      intervalMicros = Math.min(
        Math.max(
          Math.floor((prof.endTime - prof.startTime) / totalHitCount),
          intervalMicros,
        ),
        2 * intervalMicros,
      );
    }
  }
  const intervalNanos = intervalMicros * 1000;
  const stringTable = new StringTable();
  const labelCaches: Map<number | bigint, Label>[] = [];
  for (const l of lowCardinalityLabels) {
    labelCaches[stringTable.dedup(l)] = new Map<number | bigint, Label>();
  }
  const dedupLabels = (labels: Label[]) => {
    for (let i = 0; i < labels.length; i++) {
      const label = labels[i];
      const cache = labelCaches[Number(label.key)];
      if (cache !== undefined) {
        const key = label.str ?? label.num;
        const exlabel = cache.get(key);
        if (exlabel === undefined) {
          cache.set(key, label);
        } else if (
          label.str === exlabel.str &&
          label.num === exlabel.num &&
          label.numUnit === exlabel.numUnit
        ) {
          labels[i] = exlabel;
        }
      }
    }
    return labels;
  };

  const appendTimeEntryToSamples: AppendEntryToSamples<TimeProfileNode> = (
    entry: Entry<TimeProfileNode>,
    samples: Sample[],
  ) => {
    let unlabelledHits = entry.node.hitCount;
    let unlabelledCpuTime = 0;
    const isIdle = entry.node.name === '(idle)';
    for (const context of entry.node.contexts || []) {
      const labels = generateLabels
        ? generateLabels({node: entry.node, context})
        : (context.context ?? {});
      const labelsArr = buildLabels(labels, stringTable);
      if (labelsArr.length > 0) {
        // Only assign wall time if there are hits, some special nodes such as `(Non-JS threads)`
        // have zero hit count (since they do not count as wall time) and should not be assigned any
        // wall time. Also, `(idle)` nodes should be assigned zero wall time.
        const values =
          unlabelledHits > 0 ? [1, isIdle ? 0 : intervalNanos] : [0, 0];
        if (prof.hasCpuTime) {
          values.push(context.cpuTime ?? 0);
        }
        const sample = new Sample({
          locationId: entry.stack,
          value: values,
          label: dedupLabels(labelsArr),
        });
        samples.push(sample);
        unlabelledHits--;
      } else if (prof.hasCpuTime) {
        unlabelledCpuTime += context.cpuTime ?? 0;
      }
    }
    if ((!isIdle && unlabelledHits > 0) || unlabelledCpuTime > 0) {
      const labels = generateLabels ? generateLabels({node: entry.node}) : {};
      const values =
        unlabelledHits > 0
          ? [unlabelledHits, isIdle ? 0 : unlabelledHits * intervalNanos]
          : [0, 0];
      if (prof.hasCpuTime) {
        values.push(unlabelledCpuTime);
      }
      const sample = new Sample({
        locationId: entry.stack,
        value: values,
        label: buildLabels(labels, stringTable),
      });
      samples.push(sample);
    }
  };

  const sampleValueType = createSampleCountValueType(stringTable);
  const timeValueType = createTimeValueType(stringTable);

  const sampleTypes = [sampleValueType, timeValueType];
  if (prof.hasCpuTime) {
    const cpuValueType = createCpuValueType(stringTable);
    sampleTypes.push(cpuValueType);
  }

  const profile = {
    sampleType: sampleTypes,
    timeNanos: Date.now() * 1000 * 1000,
    durationNanos: (prof.endTime - prof.startTime) * 1000,
    periodType: timeValueType,
    period: intervalNanos,
  };

  const updatedProf = updateTimeProfile(prof);

  serialize(
    profile,
    updatedProf.topDownRoot,
    appendTimeEntryToSamples,
    stringTable,
    undefined,
    sourceMapper,
    columnNumbers,
  );

  return new Profile(profile);
}

function buildLabels(labelSet: object, stringTable: StringTable): Label[] {
  const labels: Label[] = [];

  for (const [key, value] of Object.entries(labelSet)) {
    const labelInput: LabelInput = {
      key: stringTable.dedup(key),
    };
    switch (typeof value) {
      case 'string':
        labelInput.str = stringTable.dedup(value);
        break;
      case 'number':
      case 'bigint':
        labelInput.num = value;
        break;
      default:
        continue;
    }
    labels.push(new Label(labelInput));
  }

  return labels;
}

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
export function serializeHeapProfile(
  prof: AllocationProfileNode | AllocationProfileNodeWithStats,
  startTimeNanos: number,
  intervalBytes: number,
  ignoreSamplesPath?: string,
  sourceMapper?: SourceMapper,
  generateLabels?: GenerateAllocationLabelsFunction,
  allocations = false,
  columnNumbers: ColumnNumbers = DEFAULT_COLUMN_NUMBERS,
): Profile {
  const appendHeapEntryToSamples: AppendEntryToSamples<
    AllocationProfileNode | AllocationProfileNodeWithStats
  > = (
    entry: Entry<AllocationProfileNode | AllocationProfileNodeWithStats>,
    samples: Sample[],
  ) => {
    if (entry.node.allocations.length > 0) {
      const labels = generateLabels
        ? buildLabels(
            generateLabels({node: entry.node as AllocationProfileNode}),
            stringTable,
          )
        : [];
      for (const alloc of entry.node.allocations) {
        // Live heap only
        if (!allocations) {
          const heapAllocation = alloc as Allocation;
          const sample = new Sample({
            locationId: entry.stack,
            value: [
              heapAllocation.count,
              heapAllocation.sizeBytes * heapAllocation.count,
            ],
            label: labels,
            // TODO: add tag for allocation size
          });
          samples.push(sample);
          continue;
        }

        const stats = alloc as AllocationWithStats;
        const inuseObjects = requireAllocationStat(
          stats.inuseObjects,
          'inuseObjects',
        );
        const inuseSpaceBytes = requireAllocationStat(
          stats.inuseSpaceBytes,
          'inuseSpaceBytes',
        );
        const allocObjects = requireAllocationStat(
          stats.allocObjects,
          'allocObjects',
        );
        const allocSpaceBytes = requireAllocationStat(
          stats.allocSpaceBytes,
          'allocSpaceBytes',
        );
        const sample = new Sample({
          locationId: entry.stack,
          value: [inuseObjects, allocObjects, inuseSpaceBytes, allocSpaceBytes],
          label: labels,
          // TODO: add tag for allocation size
        });
        samples.push(sample);
      }
    }
  };

  const stringTable = new StringTable();
  const sampleTypes = allocations
    ? [
        createInUseObjectCountValueType(stringTable),
        createAllocatedObjectCountValueType(stringTable),
        createInUseSpaceValueType(stringTable),
        createAllocatedSpaceValueType(stringTable),
      ]
    : [
        createInUseObjectCountValueType(stringTable),
        createInUseSpaceValueType(stringTable),
      ];

  const profile = {
    sampleType: sampleTypes,
    timeNanos: startTimeNanos,
    periodType: sampleTypes[sampleTypes.length - 1],
    period: intervalBytes,
  };

  serialize(
    profile,
    prof,
    appendHeapEntryToSamples,
    stringTable,
    ignoreSamplesPath,
    sourceMapper,
    columnNumbers,
  );

  return new Profile(profile);
}
