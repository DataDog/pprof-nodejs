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
  ProfileInput,
  Sample,
  ValueType,
  StringTable,
} from 'pprof-format';
import {
  Frame,
  frameFunctionName,
  HeapProfileNormalizerOptions,
  ProfileVisitor,
  SampleLabel,
  SampleType,
  TimeProfileNormalizerOptions,
  visitHeapProfile,
  visitTimeProfile,
  VisitResult,
} from './profile-normalizer';
import {SourceMapper} from './sourcemapper/sourcemapper';
import {
  AllocationProfileNode,
  AllocationProfileNodeWithStats,
  GenerateAllocationLabelsFunction,
  GenerateTimeLabelsFunction,
  TimeProfile,
} from './v8-types';

export {
  computeTotalHitCount,
  GARBAGE_COLLECTION_FUNCTION_NAME,
  NON_JS_THREADS_FUNCTION_NAME,
} from './profile-normalizer';

/**
 * Controls how a frame's column number is represented in the emitted profile.
 * - `'drop'`: omit the column; only the line number is emitted. This is the
 *   default and matches the historical behavior and standard pprof line
 *   semantics, so existing consumers see unchanged line numbers.
 * - `'pack'`: pack the column into the high 32 bits of the line field
 *   (`column << 32 | line`) — the encoding the Datadog deobfuscation backend
 *   decodes (the same one the Chrome profile intake uses). Required to
 *   deobfuscate single-line (bundled/minified) frames, where the column is the
 *   only discriminator between functions. Only frames whose source map was
 *   declared but missing locally are packed.
 * - `'emit'`: like `'pack'`, but records the column in the dedicated pprof
 *   `Line.column` field instead of packing it into the line field, so the line
 *   field stays standards-compliant and the two concerns are cleanly separated.
 *   Scoped to the same frames as `'pack'` — only those whose source map was
 *   declared but missing locally, i.e. the frames bound for server-side
 *   unminification. The Datadog backend performs the `column << 32 | line`
 *   packing itself (for Node.js profiles) when it consumes the column field.
 *   Requires pprof-format >= 2.3.0, the version that added `Line.column`.
 */
export type ColumnNumbers = 'drop' | 'pack' | 'emit';

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

/**
 * Encodes a normalized profile as a profile proto.
 *
 * @param visit - function that delivers the normalized profile to a visitor.
 * @param lowCardinalityLabels - keys of labels with few distinct values, whose
 * Label objects are shared between samples to save memory.
 */
function serialize(
  visit: (visitor: ProfileVisitor) => VisitResult,
  lowCardinalityLabels: string[] = [],
  columnNumbers: ColumnNumbers = DEFAULT_COLUMN_NUMBERS,
): Profile {
  const stringTable = new StringTable();
  const samples: Sample[] = [];
  const locations: Location[] = [];
  const stacks: number[][] = [];
  const functions: Function[] = [];
  const functionIdMap = new Map<string, number>();
  const packColumns = columnNumbers === 'pack';
  const emitColumns = columnNumbers === 'emit';
  const labelCaches: Map<number | bigint, Label>[] = [];
  for (const l of lowCardinalityLabels) {
    labelCaches[stringTable.dedup(l)] = new Map<number | bigint, Label>();
  }
  let profile: ProfileInput = {};

  const {hasMissingMapFiles} = visit({
    metadata: metadata => {
      const valueType = (t: SampleType) =>
        new ValueType({
          type: stringTable.dedup(t.type),
          unit: stringTable.dedup(t.unit),
        });
      profile = {
        sampleType: metadata.sampleTypes.map(valueType),
        // pprof Profile objects have always had a number here; keep it one.
        // It rounds the time to a double, like Date.now() * 1000 * 1000 did.
        timeNanos: Number(metadata.timeNanos),
        durationNanos: metadata.durationNanos,
        periodType: valueType(metadata.periodType),
        period: metadata.period,
      };
    },
    frame: frame => {
      // id is index+1, since 0 is not valid id.
      const id = locations.length + 1;
      locations.push(new Location({id, line: [getLine(frame)]}));
      return id;
    },
    stack: frameIds => stacks.push(frameIds) - 1,
    sample: sample => {
      samples.push(
        new Sample({
          locationId: stacks[sample.stackId],
          value: sample.values,
          label: dedupLabels(buildLabels(sample.labels)),
        }),
      );
    },
  });

  profile.sample = samples;
  profile.location = locations;
  profile.function = functions;
  profile.stringTable = stringTable;

  if (hasMissingMapFiles) {
    profile.comment = [stringTable.dedup('dd:has-missing-map-files')];
  }

  return new Profile(profile);

  function getLine(frame: Frame): Line {
    // Both 'pack' and 'emit' carry the column only for frames whose source map
    // was declared but missing locally — i.e. exactly the frames bound for
    // server-side unminification (the same condition that sets
    // dd:has-missing-map-files). Locally-resolved frames keep a plain line and
    // no column, so column data never reaches profiles that skip server-side
    // unminification.
    const carryColumn = frame.missingMapFile;
    return new Line({
      functionId: getFunction(frame).id,
      // 'pack' encodes the column into the high 32 bits of the line field.
      line:
        packColumns && carryColumn
          ? packLineAndColumn(frame.line, frame.column)
          : frame.line,
      // 'emit' records the column in the dedicated pprof Line.column field
      // instead; the backend does the line/column packing itself.
      column: emitColumns && carryColumn ? frame.column : undefined,
    });
  }

  function getFunction(frame: Frame): Function {
    // A function is identified by script, file and name, or by script, file
    // and position for anonymous functions; the prefixes keep the two kinds
    // apart. The file matters because after source mapping, one script can
    // map to several original files.
    const file = frame.file;
    const keyStr = frame.name
      ? `n:${frame.scriptId}:${file.length}:${file}:${frame.name}`
      : `a:${frame.scriptId}:${frame.line}:${frame.column}:${file}`;
    let id = functionIdMap.get(keyStr);
    if (id !== undefined) {
      // id is index+1, since 0 is not valid id.
      return functions[id - 1];
    }
    id = functions.length + 1;
    functionIdMap.set(keyStr, id);
    const nameId = stringTable.dedup(frameFunctionName(frame));
    const f = new Function({
      id,
      name: nameId,
      systemName: nameId,
      filename: stringTable.dedup(frame.file),
    });
    functions.push(f);
    return f;
  }

  function buildLabels(sampleLabels: SampleLabel[]): Label[] {
    return sampleLabels.map(([key, value]) => {
      const labelInput: LabelInput = {key: stringTable.dedup(key)};
      if (typeof value === 'string') {
        labelInput.str = stringTable.dedup(value);
      } else {
        labelInput.num = value;
      }
      return new Label(labelInput);
    });
  }

  function dedupLabels(labels: Label[]) {
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
  }
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
  const options: TimeProfileNormalizerOptions = {
    intervalMicros,
    sourceMapper,
    recomputeSamplingInterval,
    generateLabels,
  };
  return serialize(
    visitor => visitTimeProfile(prof, visitor, options),
    lowCardinalityLabels,
    columnNumbers,
  );
}

/**
 * Converts v8 heap profile into into a profile proto.
 * (https://github.com/google/pprof/blob/master/proto/profile.proto)
 *
 * @param prof - profile to be converted.
 * @param startTimeNanos - start time of profile, in nanoseconds (POSIX time).
 * @param intervalBytes - bytes allocated between samples.
 */
export function serializeHeapProfile(
  prof: AllocationProfileNode | AllocationProfileNodeWithStats,
  startTimeNanos: number | bigint,
  intervalBytes: number,
  ignoreSamplesPath?: string,
  sourceMapper?: SourceMapper,
  generateLabels?: GenerateAllocationLabelsFunction,
  allocations = false,
  columnNumbers: ColumnNumbers = DEFAULT_COLUMN_NUMBERS,
): Profile {
  const options: HeapProfileNormalizerOptions = {
    startTimeNanos: BigInt(startTimeNanos),
    intervalBytes,
    ignoreSamplesPath,
    sourceMapper,
    generateLabels,
    allocations,
  };
  return serialize(
    visitor => visitHeapProfile(prof, visitor, options),
    [],
    columnNumbers,
  );
}
