/**
 * Copyright 2026 Datadog. All Rights Reserved.
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

/**
 * Format-neutral normalization of V8 profiles.
 *
 * Turns the raw V8 time and heap profile trees into a stream of resolved
 * frames and samples, applying everything that is a matter of profile
 * semantics rather than of any particular output format: source mapping,
 * synthetic runtime frames, per-context sample expansion, CPU time
 * attribution, label generation and sample values. Output formats (pprof in
 * profile-serializer.ts, and potentially others) are implemented as
 * {@link ProfileVisitor}s on top of it.
 */

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
 * A resolved stack frame: `file://` prefix stripped and source map applied.
 */
export interface Frame {
  /**
   * Function name after source mapping. Empty or absent for anonymous
   * functions; use {@link frameFunctionName} to get a display name.
   */
  name?: string;
  file: string;
  line?: number;
  column?: number;
  scriptId?: number;
  /**
   * True when the frame's script declares a sourceMappingURL but the map
   * could not be found locally, i.e. the frame is a candidate for server-side
   * unminification.
   */
  missingMapFile: boolean;
}

export type SampleLabelValue = string | number | bigint;

/** A label as a key/value pair. */
export type SampleLabel = [key: string, value: SampleLabelValue];

export interface NormalizedSample {
  /** The stack, as the ID returned by {@link ProfileVisitor.stack}. */
  stackId: number;
  /** One value for each of {@link ProfileMetadata.sampleTypes}. */
  values: number[];
  labels: SampleLabel[];
  /**
   * Time at which the sample was taken, in nanoseconds since epoch (with
   * microsecond precision). Only set on samples that represent a single
   * observation with a known time, i.e. time profile samples for a context.
   * Undefined for aggregated samples.
   */
  timestamp?: bigint;
}

export interface SampleType {
  type: string;
  unit: string;
}

export interface ProfileMetadata {
  sampleTypes: SampleType[];
  periodType: SampleType;
  period: number;
  /** Time of collection in nanoseconds since epoch. */
  timeNanos: number;
  durationNanos?: number;
}

/**
 * Receives a normalized profile. Calls arrive in this order: metadata() once,
 * then frame(), stack() and sample() interleaved. Each distinct frame and
 * stack is delivered exactly once, before the first stack or sample that
 * references it. Only stacks that have samples are delivered.
 *
 * All samples of a profile tree node are delivered consecutively. A stack ID
 * normally belongs to a single node, but can recur later if two nodes resolve
 * to the same frames (e.g. when source maps map distinct generated locations
 * to the same original location).
 */
export interface ProfileVisitor {
  metadata(metadata: ProfileMetadata): void;
  /**
   * @returns the ID by which stacks will refer to this frame.
   */
  frame(frame: Frame): number;
  /**
   * @param frameIds - the stack, leaf first, as IDs returned by frame(). The
   * array is not reused by the caller, so the visitor may retain it.
   * @returns the ID by which samples will refer to this stack.
   */
  stack(frameIds: number[]): number;
  sample(sample: NormalizedSample): void;
}

export interface VisitResult {
  /** True if at least one frame has {@link Frame.missingMapFile} set. */
  hasMissingMapFiles: boolean;
}

/** A normalized profile collected into arrays. */
export interface NormalizedProfile extends ProfileMetadata, VisitResult {
  frames: Frame[];
  /** Stacks, leaf first, as indices into {@link frames}. */
  stacks: number[][];
  /** Samples' stack IDs are indices into {@link stacks}. */
  samples: NormalizedSample[];
}

export interface TimeProfileNormalizerOptions {
  /** Average time in microseconds between samples, as requested. */
  intervalMicros: number;
  sourceMapper?: SourceMapper;
  /**
   * Recompute the sampling interval from the profile duration and total hit
   * count, bounded to [intervalMicros, 2 * intervalMicros].
   */
  recomputeSamplingInterval?: boolean;
  generateLabels?: GenerateTimeLabelsFunction;
  /**
   * Emit a separate, timestamped sample for every context, even when it has
   * no labels. By default, the hits and CPU time of contexts without labels
   * are aggregated into a single sample per node, without a timestamp. Set
   * this when every sample should have a timestamp, e.g. for OTel profiles,
   * whose samples should all have the same shape. Hits without a context
   * (e.g. when contexts are not collected) are still aggregated.
   */
  samplePerContext?: boolean;
}

export interface HeapProfileNormalizerOptions {
  /** Start time of profile, in nanoseconds since epoch. */
  startTimeNanos: number;
  /** Bytes allocated between samples. */
  intervalBytes: number;
  ignoreSamplesPath?: string;
  sourceMapper?: SourceMapper;
  generateLabels?: GenerateAllocationLabelsFunction;
  /** Root was collected in allocation mode (nodes carry allocation stats). */
  allocations?: boolean;
}

/**
 * Emits a sample for the node currently being visited.
 */
type EmitSample = (
  values: number[],
  labels: SampleLabel[],
  timestamp?: bigint,
) => void;

/**
 * Converts a node into zero or more samples and passes them to emit.
 */
type EmitSamples<T extends ProfileNode> = (node: T, emit: EmitSample) => void;

/**
 * Profile node and the interned stack of its parent (-1 for top level nodes).
 */
interface Entry<T extends ProfileNode> {
  node: T;
  parentStack: number;
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
 * @returns the frame's function name, or a synthesized
 * `(anonymous:L#line:C#column)` name for anonymous functions.
 */
export function frameFunctionName(frame: Frame): string {
  if (frame.name) {
    return frame.name;
  }
  if (frame.line) {
    if (frame.column) {
      return `(anonymous:L#${frame.line}:C#${frame.column})`;
    }
    return `(anonymous:L#${frame.line})`;
  }
  return '(anonymous)';
}

function toSampleLabels(labelSet: object): SampleLabel[] {
  const labels: SampleLabel[] = [];
  for (const [key, value] of Object.entries(labelSet)) {
    switch (typeof value) {
      case 'string':
      case 'number':
      case 'bigint':
        labels.push([key, value]);
        break;
      default:
        continue;
    }
  }
  return labels;
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
 * Walks the V8 profile tree depth first, delivering each distinct frame and
 * stack, and the samples emitted for each node to the visitor.
 *
 * Stacks are interned by prefix: a node's stack is identified by its parent's
 * stack and its own frame ID, so stacks with equal frames get the same ID
 * without comparing whole arrays. A stack is only materialized and delivered
 * to the visitor when its first sample is emitted.
 */
function visitTree<T extends ProfileNode>(
  root: T,
  visitor: ProfileVisitor,
  emitSamples: EmitSamples<T>,
  ignoreSamplesPath?: string,
  sourceMapper?: SourceMapper,
): VisitResult {
  // Interned frames: internal frame index by key, and the visitor's frame
  // ID for each internal frame index.
  const frameIndexMap = new Map<string, number>();
  const frameIds: number[] = [];
  let hasMissingMapFiles = false;

  // Interned stacks, by internal stack index: the parent stack index (-1 for
  // none), the internal frame index, and the visitor's stack ID (undefined
  // until delivered).
  const stackIndexMap = new Map<number, number>();
  const stackParents: number[] = [];
  const stackFrames: number[] = [];
  const stackIds: Array<number | undefined> = [];

  let currentStack = -1;
  const emit: EmitSample = (values, labels, timestamp) => {
    visitor.sample({
      stackId: getStackId(currentStack),
      values,
      labels,
      timestamp,
    });
  };

  const entries: Array<Entry<T>> = (root.children as T[]).map((n: T) => ({
    node: n,
    parentStack: -1,
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
    const stack = internStack(
      entry.parentStack,
      getFrameIndex(node, scriptName),
    );
    currentStack = stack;
    emitSamples(node, emit);
    for (const child of node.children as T[]) {
      entries.push({node: child, parentStack: stack});
    }
  }

  return {hasMissingMapFiles};

  function internStack(parent: number, frame: number): number {
    // Unique while there are fewer than 2^26 frames and 2^27 stacks, so the
    // key stays below Number.MAX_SAFE_INTEGER.
    const key = (parent + 1) * 0x4000000 + frame;
    let stack = stackIndexMap.get(key);
    if (stack === undefined) {
      stack = stackParents.length;
      stackParents.push(parent);
      stackFrames.push(frame);
      stackIds.push(undefined);
      stackIndexMap.set(key, stack);
    }
    return stack;
  }

  function getStackId(stack: number): number {
    let id = stackIds[stack];
    if (id === undefined) {
      const stackFrameIds: number[] = [];
      for (let s = stack; s !== -1; s = stackParents[s]) {
        stackFrameIds.push(frameIds[stackFrames[s]]);
      }
      id = visitor.stack(stackFrameIds);
      stackIds[stack] = id;
    }
    return id;
  }

  function getFrameIndex(node: ProfileNode, scriptName: string): number {
    let loc: SourceLocation = {
      file: scriptName || '',
      line: node.lineNumber,
      column: node.columnNumber,
      name: node.name,
    };

    if (loc.line) {
      if (sourceMapper && isGeneratedLocation(loc)) {
        loc = sourceMapper.mappingInfo(loc);
        if (loc.missingMapFile) {
          hasMissingMapFiles = true;
        }
      }
    }
    // The resolved file is part of a frame's identity: after source mapping,
    // one script can map to several original files. The file's length makes
    // the key unambiguous.
    const file = loc.file || '';
    const keyStr = `${node.scriptId}:${loc.line}:${loc.column}:${loc.missingMapFile ? 1 : 0}:${file.length}:${file}:${loc.name}`;
    let index = frameIndexMap.get(keyStr);
    if (index === undefined) {
      index = frameIds.length;
      frameIds.push(
        visitor.frame({
          name: loc.name,
          file,
          line: loc.line,
          column: loc.column,
          scriptId: node.scriptId,
          missingMapFile: loc.missingMapFile === true,
        }),
      );
      frameIndexMap.set(keyStr, index);
    }
    return index;
  }
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
 * Normalizes a V8 time profile and passes it to the visitor.
 *
 * Sample values are [sample count, wall nanoseconds] plus CPU nanoseconds if
 * the profile has CPU time.
 */
export function visitTimeProfile(
  prof: TimeProfile,
  visitor: ProfileVisitor,
  options: TimeProfileNormalizerOptions,
): VisitResult {
  const {
    sourceMapper,
    recomputeSamplingInterval,
    generateLabels,
    samplePerContext,
  } = options;
  let intervalMicros = options.intervalMicros;
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

  const timeType = {type: 'wall', unit: 'nanoseconds'};
  const sampleTypes = [{type: 'sample', unit: 'count'}, timeType];
  if (prof.hasCpuTime) {
    sampleTypes.push({type: 'cpu', unit: 'nanoseconds'});
  }
  visitor.metadata({
    sampleTypes,
    timeNanos: Date.now() * 1000 * 1000,
    durationNanos: (prof.endTime - prof.startTime) * 1000,
    periodType: timeType,
    period: intervalNanos,
  });

  const emitTimeSamples: EmitSamples<TimeProfileNode> = (
    node: TimeProfileNode,
    emit: EmitSample,
  ) => {
    let unlabelledHits = node.hitCount;
    let unlabelledCpuTime = 0;
    const isIdle = node.name === '(idle)';
    for (const context of node.contexts || []) {
      const labels = toSampleLabels(
        generateLabels
          ? generateLabels({node, context})
          : (context.context ?? {}),
      );
      if (labels.length > 0 || samplePerContext) {
        // Only assign wall time if there are hits, some special nodes such as `(Non-JS threads)`
        // have zero hit count (since they do not count as wall time) and should not be assigned any
        // wall time. Also, `(idle)` nodes should be assigned zero wall time.
        const values =
          unlabelledHits > 0 ? [1, isIdle ? 0 : intervalNanos] : [0, 0];
        if (prof.hasCpuTime) {
          values.push(context.cpuTime ?? 0);
        }
        // Context timestamps are in microseconds; 0 marks a synthetic context.
        emit(
          values,
          labels,
          context.timestamp ? BigInt(context.timestamp) * 1000n : undefined,
        );
        unlabelledHits--;
      } else if (prof.hasCpuTime) {
        unlabelledCpuTime += context.cpuTime ?? 0;
      }
    }
    if ((!isIdle && unlabelledHits > 0) || unlabelledCpuTime > 0) {
      const labels = generateLabels ? generateLabels({node}) : {};
      const values =
        unlabelledHits > 0
          ? [unlabelledHits, isIdle ? 0 : unlabelledHits * intervalNanos]
          : [0, 0];
      if (prof.hasCpuTime) {
        values.push(unlabelledCpuTime);
      }
      emit(values, toSampleLabels(labels));
    }
  };

  return visitTree(
    updateTimeProfile(prof).topDownRoot,
    visitor,
    emitTimeSamples,
    undefined,
    sourceMapper,
  );
}

/**
 * Normalizes a V8 heap profile and passes it to the visitor.
 *
 * Sample values are [inuse objects, inuse bytes], or [inuse objects, alloc
 * objects, inuse bytes, alloc bytes] in allocation mode.
 */
export function visitHeapProfile(
  prof: AllocationProfileNode | AllocationProfileNodeWithStats,
  visitor: ProfileVisitor,
  options: HeapProfileNormalizerOptions,
): VisitResult {
  const {ignoreSamplesPath, sourceMapper, generateLabels, allocations} =
    options;

  const sampleTypes = allocations
    ? [
        {type: 'inuse_objects', unit: 'count'},
        {type: 'alloc_objects', unit: 'count'},
        {type: 'inuse_space', unit: 'bytes'},
        {type: 'alloc_space', unit: 'bytes'},
      ]
    : [
        {type: 'inuse_objects', unit: 'count'},
        {type: 'inuse_space', unit: 'bytes'},
      ];
  visitor.metadata({
    sampleTypes,
    timeNanos: options.startTimeNanos,
    periodType: sampleTypes[sampleTypes.length - 1],
    period: options.intervalBytes,
  });

  const emitHeapSamples: EmitSamples<
    AllocationProfileNode | AllocationProfileNodeWithStats
  > = (
    node: AllocationProfileNode | AllocationProfileNodeWithStats,
    emit: EmitSample,
  ) => {
    if (node.allocations.length === 0) {
      return;
    }
    const labels = generateLabels
      ? toSampleLabels(generateLabels({node: node as AllocationProfileNode}))
      : [];
    for (const alloc of node.allocations) {
      // Live heap only
      if (!allocations) {
        const heapAllocation = alloc as Allocation;
        emit(
          [
            heapAllocation.count,
            heapAllocation.sizeBytes * heapAllocation.count,
          ],
          labels,
          // TODO: add tag for allocation size
        );
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
      emit(
        [inuseObjects, allocObjects, inuseSpaceBytes, allocSpaceBytes],
        labels,
        // TODO: add tag for allocation size
      );
    }
  };

  return visitTree(
    prof,
    visitor,
    emitHeapSamples,
    ignoreSamplesPath,
    sourceMapper,
  );
}

function collect(
  visit: (visitor: ProfileVisitor) => VisitResult,
): NormalizedProfile {
  let metadata: ProfileMetadata | undefined;
  const frames: Frame[] = [];
  const stacks: number[][] = [];
  const samples: NormalizedSample[] = [];
  const {hasMissingMapFiles} = visit({
    metadata: m => {
      metadata = m;
    },
    frame: f => frames.push(f) - 1,
    stack: s => stacks.push(s) - 1,
    sample: s => {
      samples.push(s);
    },
  });
  return {...metadata!, frames, stacks, samples, hasMissingMapFiles};
}

/**
 * Normalizes a V8 time profile into a {@link NormalizedProfile}.
 * See {@link visitTimeProfile}.
 */
export function normalizeTimeProfile(
  prof: TimeProfile,
  options: TimeProfileNormalizerOptions,
): NormalizedProfile {
  return collect(visitor => visitTimeProfile(prof, visitor, options));
}

/**
 * Normalizes a V8 heap profile into a {@link NormalizedProfile}.
 * See {@link visitHeapProfile}.
 */
export function normalizeHeapProfile(
  prof: AllocationProfileNode | AllocationProfileNodeWithStats,
  options: HeapProfileNormalizerOptions,
): NormalizedProfile {
  return collect(visitor => visitHeapProfile(prof, visitor, options));
}
