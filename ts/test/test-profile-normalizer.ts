/*
 * Copyright 2026 Datadog, Inc
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

import * as sinon from 'sinon';
import {Profile} from 'pprof-format';

import {
  Frame,
  frameFunctionName,
  GARBAGE_COLLECTION_FUNCTION_NAME,
  NON_JS_THREADS_FUNCTION_NAME,
  NormalizedProfile,
  normalizeHeapProfile,
  normalizeTimeProfile,
  ProfileVisitor,
  visitTimeProfile,
} from '../src/profile-normalizer';
import {
  serializeHeapProfile,
  serializeTimeProfile,
} from '../src/profile-serializer';
import {SourceMapper} from '../src/sourcemapper/sourcemapper';
import {TimeProfile, TimeProfileNode} from '../src/v8-types';
import {
  labelEncodingProfile,
  v8AnonymousFunctionHeapProfile,
  v8HeapProfile,
  v8HeapWithPathProfile,
  v8TimeProfile,
} from './profiles-for-tests';

const assert = require('assert');

/**
 * Renders each sample of a normalized profile as a comparable string of its
 * function names (leaf first), values and labels.
 */
function describeNormalizedSamples(prof: NormalizedProfile): string[] {
  return prof.samples.map(s =>
    JSON.stringify({
      stack: prof.stacks[s.stackId].map(i => frameFunctionName(prof.frames[i])),
      values: s.values,
      labels: s.labels.map(([k, v]) => [k, String(v)]),
    }),
  );
}

/**
 * Same as describeNormalizedSamples, for a pprof profile.
 */
function describePprofSamples(prof: Profile): string[] {
  const strings = prof.stringTable.strings;
  return prof.sample.map(s =>
    JSON.stringify({
      stack: s.locationId.map(id => {
        const location = prof.location[Number(id) - 1];
        const fn = prof.function[Number(location.line[0].functionId) - 1];
        return strings[Number(fn.name)];
      }),
      values: s.value.map(Number),
      labels: s.label.map(l => [
        strings[Number(l.key)],
        l.str ? strings[Number(l.str)] : String(l.num),
      ]),
    }),
  );
}

function makeTimeProfile(children: TimeProfileNode[]): TimeProfile {
  return {
    startTime: 0,
    endTime: 10 * 1000 * 1000,
    hasCpuTime: true,
    topDownRoot: {
      name: '(root)',
      scriptName: 'root',
      scriptId: 0,
      lineNumber: 0,
      columnNumber: 0,
      hitCount: 0,
      children,
    },
  };
}

function makeLeaf(
  name: string,
  props: Partial<TimeProfileNode> = {},
): TimeProfileNode {
  return {
    name,
    scriptName: 'script',
    scriptId: 1,
    lineNumber: 1,
    columnNumber: 1,
    hitCount: 1,
    children: [],
    ...props,
  };
}

describe('profile-normalizer', () => {
  let dateStub: sinon.SinonStub<[], number>;

  before(() => {
    dateStub = sinon.stub(Date, 'now').returns(0);
  });
  after(() => {
    dateStub.restore();
  });

  describe('normalizeTimeProfile', () => {
    it('should have the same samples as the serialized pprof profile', () => {
      const normalized = normalizeTimeProfile(v8TimeProfile, {
        intervalMicros: 1000,
      });
      const pprof = serializeTimeProfile(v8TimeProfile, 1000);
      assert.deepStrictEqual(
        describeNormalizedSamples(normalized),
        describePprofSamples(pprof),
      );
      assert.strictEqual(normalized.frames.length, pprof.location.length);
      assert.strictEqual(normalized.period, pprof.period);
      assert.strictEqual(normalized.durationNanos, pprof.durationNanos);
    });

    it('should give the collection time exactly, as a bigint', () => {
      // 1.7e18 nanoseconds is beyond the precision of a double
      dateStub.returns(1700000000123);
      try {
        const normalized = normalizeTimeProfile(makeTimeProfile([]), {
          intervalMicros: 1000,
        });
        assert.strictEqual(normalized.timeNanos, 1700000000123000000n);
        // The pprof profile keeps the number it has always had
        const pprof = serializeTimeProfile(makeTimeProfile([]), 1000);
        assert.strictEqual(pprof.timeNanos, 1700000000123 * 1000 * 1000);
      } finally {
        dateStub.returns(0);
      }
    });

    it('should give the start of the collection period', () => {
      dateStub.returns(1700000000123);
      try {
        // With the native profiler's epoch offset, exactly
        const prof = {
          ...makeTimeProfile([]),
          startTime: 5_000_000,
          endTime: 15_000_000,
          epochOffset: 1700000000000_000 - 5_000_000,
        };
        const exact = normalizeTimeProfile(prof, {intervalMicros: 1000});
        assert.strictEqual(exact.startTimeNanos, 1700000000000_000_000n);
        assert.strictEqual(exact.durationNanos, 10_000_000_000);
        // Without one, estimated as the collection time minus the duration
        const estimated = normalizeTimeProfile(makeTimeProfile([]), {
          intervalMicros: 1000,
        });
        assert.strictEqual(
          estimated.startTimeNanos,
          1700000000123_000_000n - 10_000_000_000n,
        );
      } finally {
        dateStub.returns(0);
      }
    });

    it('should describe wall and sample count types, and CPU when present', () => {
      const withCpu = normalizeTimeProfile(makeTimeProfile([]), {
        intervalMicros: 1000,
      });
      assert.deepStrictEqual(withCpu.sampleTypes, [
        {type: 'sample', unit: 'count'},
        {type: 'wall', unit: 'nanoseconds'},
        {type: 'cpu', unit: 'nanoseconds'},
      ]);
      assert.deepStrictEqual(withCpu.periodType, {
        type: 'wall',
        unit: 'nanoseconds',
      });
      const withoutCpu = normalizeTimeProfile(
        {...makeTimeProfile([]), hasCpuTime: false},
        {intervalMicros: 1000},
      );
      assert.strictEqual(withoutCpu.sampleTypes.length, 2);
    });

    it('should keep only string, number and bigint labels', () => {
      const normalized = normalizeTimeProfile(
        labelEncodingProfile as unknown as TimeProfile,
        {intervalMicros: 1000},
      );
      assert.deepStrictEqual(normalized.samples[0].labels, [
        ['someStr', 'foo'],
        ['someNum', 42],
        ['someBigint', 18446744073709551557n],
      ]);
    });

    it('should put synthetic runtime frames under a Node.js frame', () => {
      const prof = makeTimeProfile([
        makeLeaf('(program)'),
        makeLeaf('(garbage collector)'),
        makeLeaf('foo'),
      ]);
      prof.nonJSThreadsCpuTime = 500;
      const normalized = normalizeTimeProfile(prof, {intervalMicros: 1000});
      const stacks = normalized.samples.map(s =>
        normalized.stacks[s.stackId].map(i =>
          frameFunctionName(normalized.frames[i]),
        ),
      );
      assert.deepStrictEqual(stacks.sort(), [
        [GARBAGE_COLLECTION_FUNCTION_NAME, 'Node.js'],
        [NON_JS_THREADS_FUNCTION_NAME, 'Node.js'],
        ['foo'],
      ]);
      const nonJS = normalized.samples.find(
        s =>
          normalized.frames[normalized.stacks[s.stackId][0]].name ===
          NON_JS_THREADS_FUNCTION_NAME,
      )!;
      assert.deepStrictEqual(nonJS.values, [0, 0, 500]);
      assert.strictEqual(nonJS.timestamp, undefined);
    });

    it('should emit a sample per labelled context and give idle zero wall time', () => {
      const prof = makeTimeProfile([
        makeLeaf('(idle)', {
          hitCount: 2,
          contexts: [
            {context: {a: 'x'}, timestamp: 1n, cpuTime: 10},
            {context: {}, timestamp: 2n, cpuTime: 20},
          ],
        }),
      ]);
      const normalized = normalizeTimeProfile(prof, {intervalMicros: 1000});
      assert.deepStrictEqual(
        normalized.samples.map(s => [s.values, s.labels, s.timestamp]),
        [
          // Labelled samples carry the context timestamp in nanoseconds.
          [[1, 0, 10], [['a', 'x']], 1000n],
          // The unlabelled context's hit and CPU time go into one aggregated
          // sample, without a timestamp.
          [[1, 0, 20], [], undefined],
        ],
      );
    });

    it('should emit a timestamped sample per context with samplePerContext', () => {
      const prof = makeTimeProfile([
        makeLeaf('(idle)', {
          hitCount: 2,
          contexts: [
            {context: {a: 'x'}, timestamp: 1n, cpuTime: 10},
            {context: {}, timestamp: 2n, cpuTime: 20},
          ],
        }),
        makeLeaf('foo', {
          scriptId: 2,
          // One more hit than contexts: that one stays aggregated.
          hitCount: 3,
          contexts: [
            {context: {}, timestamp: 3n, cpuTime: 30},
            {context: {}, timestamp: 4n, cpuTime: 40},
          ],
        }),
      ]);
      const describe = (p: NormalizedProfile) =>
        p.samples.map(s => [
          frameFunctionName(p.frames[p.stacks[s.stackId][0]]),
          s.values,
          s.labels,
          s.timestamp,
        ]);
      const perContext = normalizeTimeProfile(prof, {
        intervalMicros: 1000,
        samplePerContext: true,
      });
      assert.deepStrictEqual(describe(perContext), [
        ['foo', [1, 1000000, 30], [], 3000n],
        ['foo', [1, 1000000, 40], [], 4000n],
        ['foo', [1, 1000000, 0], [], undefined],
        ['(idle)', [1, 0, 10], [['a', 'x']], 1000n],
        ['(idle)', [1, 0, 20], [], 2000n],
      ]);
      // Totals match the default, aggregating mode.
      const aggregated = normalizeTimeProfile(prof, {intervalMicros: 1000});
      assert.deepStrictEqual(describe(aggregated), [
        ['foo', [3, 3000000, 70], [], undefined],
        ['(idle)', [1, 0, 10], [['a', 'x']], 1000n],
        ['(idle)', [1, 0, 20], [], undefined],
      ]);
    });

    it('should pass the context to generateLabels', () => {
      const context = {context: {}, timestamp: 42n};
      const prof = makeTimeProfile([makeLeaf('foo', {contexts: [context]})]);
      const generateLabels = sinon.stub().returns({ts: 42n});
      const normalized = normalizeTimeProfile(prof, {
        intervalMicros: 1000,
        generateLabels,
      });
      sinon.assert.calledOnce(generateLabels);
      assert.strictEqual(generateLabels.getCall(0).args[0].context, context);
      assert.deepStrictEqual(normalized.samples[0].labels, [['ts', 42n]]);
    });

    it('should keep frames from different original files apart', () => {
      // One generated script whose two positions map to different original
      // files, with the same function name and position
      const sourceMapper = {
        mappingInfo: (loc: {line: number}) => ({
          file: loc.line === 1 ? '/app/a.ts' : '/app/b.ts',
          line: 1,
          column: 1,
          name: 'render',
        }),
      } as unknown as SourceMapper;
      const prof = makeTimeProfile([
        makeLeaf('x', {scriptId: 42, lineNumber: 1, columnNumber: 5}),
        makeLeaf('y', {scriptId: 42, lineNumber: 2, columnNumber: 5}),
      ]);
      const normalized = normalizeTimeProfile(prof, {
        intervalMicros: 1000,
        sourceMapper,
      });
      assert.deepStrictEqual(normalized.frames.map(f => f.file).sort(), [
        '/app/a.ts',
        '/app/b.ts',
      ]);
      assert.strictEqual(normalized.stacks.length, 2);

      const pprof = serializeTimeProfile(prof, 1000, sourceMapper);
      const strings = pprof.stringTable.strings;
      assert.deepStrictEqual(
        pprof.function.map(f => strings[Number(f.filename)]).sort(),
        ['/app/a.ts', '/app/b.ts'],
      );
    });

    it('should keep named and anonymous functions apart', () => {
      // A function named like an anonymous function's position
      const prof = makeTimeProfile([
        makeLeaf('1:1', {lineNumber: 5, columnNumber: 5}),
        makeLeaf('', {lineNumber: 1, columnNumber: 1}),
      ]);
      const pprof = serializeTimeProfile(prof, 1000);
      const strings = pprof.stringTable.strings;
      assert.deepStrictEqual(
        pprof.function.map(f => strings[Number(f.name)]).sort(),
        ['(anonymous:L#1:C#1)', '1:1'],
      );
    });

    it('should strip file:// prefixes from script names', () => {
      const prof = makeTimeProfile([
        makeLeaf('foo', {scriptName: 'file:///app/index.mjs'}),
      ]);
      const normalized = normalizeTimeProfile(prof, {intervalMicros: 1000});
      assert.strictEqual(normalized.frames[0].file, '/app/index.mjs');
    });
  });

  describe('visitTimeProfile', () => {
    it('should deliver metadata first, frames and stacks once before use, and end last', () => {
      const events: string[] = [];
      const frameIds = new Set<number>();
      const stackIds = new Set<number>();
      let nextId = 100;
      const visitor: ProfileVisitor = {
        metadata: () => {
          events.push('metadata');
        },
        frame: () => {
          events.push('frame');
          const id = nextId++;
          frameIds.add(id);
          return id;
        },
        stack: ids => {
          events.push('stack');
          for (const id of ids) {
            assert.ok(frameIds.has(id), `frame ${id} delivered before stack`);
          }
          const id = nextId++;
          stackIds.add(id);
          return id;
        },
        sample: s => {
          events.push('sample');
          assert.ok(
            stackIds.has(s.stackId),
            `stack ${s.stackId} delivered before sample`,
          );
        },
      };
      visitor.end = result => {
        events.push('end');
        assert.deepStrictEqual(result, {hasMissingMapFiles: false});
      };
      const result = visitTimeProfile(v8TimeProfile, visitor, {
        intervalMicros: 1000,
      });
      assert.strictEqual(events[0], 'metadata');
      assert.strictEqual(events.lastIndexOf('metadata'), 0);
      assert.ok(events.includes('sample'));
      assert.strictEqual(events.indexOf('end'), events.length - 1);
      assert.strictEqual(result.hasMissingMapFiles, false);
    });

    it('should deliver a stack per distinct frame sequence that has samples', () => {
      // Two sibling nodes resolving to the same frame share a stack, and the
      // sample-less "parent" node's stack is never delivered.
      const prof = makeTimeProfile([
        makeLeaf('parent', {
          hitCount: 0,
          children: [makeLeaf('foo', {scriptId: 2}), makeLeaf('bar')],
        }),
        makeLeaf('parent', {
          hitCount: 0,
          children: [makeLeaf('foo', {scriptId: 2})],
        }),
      ]);
      const normalized = normalizeTimeProfile(prof, {intervalMicros: 1000});
      const stacks = normalized.stacks.map(s =>
        s.map(i => frameFunctionName(normalized.frames[i])),
      );
      assert.deepStrictEqual(stacks.sort(), [
        ['bar', 'parent'],
        ['foo', 'parent'],
      ]);
      assert.strictEqual(normalized.samples.length, 3);
      const fooStack = normalized.stacks.findIndex(
        s => normalized.frames[s[0]].name === 'foo',
      );
      assert.strictEqual(
        normalized.samples.filter(s => s.stackId === fooStack).length,
        2,
      );
    });
  });

  describe('normalizeHeapProfile', () => {
    it('should have the same samples as the serialized pprof profile', () => {
      for (const prof of [v8HeapProfile, v8AnonymousFunctionHeapProfile]) {
        const normalized = normalizeHeapProfile(prof, {
          startTimeNanos: 0n,
          intervalBytes: 64,
        });
        const pprof = serializeHeapProfile(prof, 0, 64);
        assert.deepStrictEqual(
          describeNormalizedSamples(normalized),
          describePprofSamples(pprof),
        );
        assert.deepStrictEqual(normalized.sampleTypes, [
          {type: 'inuse_objects', unit: 'count'},
          {type: 'inuse_space', unit: 'bytes'},
        ]);
      }
    });

    it('should omit samples under ignoreSamplesPath', () => {
      const ignored = '@google-cloud/profiler';
      const normalized = normalizeHeapProfile(v8HeapWithPathProfile, {
        startTimeNanos: 0n,
        intervalBytes: 64,
        ignoreSamplesPath: ignored,
      });
      assert.ok(normalized.frames.length > 0);
      for (const frame of normalized.frames) {
        assert.ok(!frame.file.includes(ignored));
      }
    });
  });

  describe('frameFunctionName', () => {
    const frame = (props: Partial<Frame>): Frame => ({
      file: 'f.js',
      missingMapFile: false,
      ...props,
    });

    it('should return the name when there is one', () => {
      assert.strictEqual(frameFunctionName(frame({name: 'foo'})), 'foo');
    });

    it('should synthesize names for anonymous functions', () => {
      assert.strictEqual(
        frameFunctionName(frame({name: '', line: 3, column: 5})),
        '(anonymous:L#3:C#5)',
      );
      assert.strictEqual(
        frameFunctionName(frame({line: 3})),
        '(anonymous:L#3)',
      );
      assert.strictEqual(frameFunctionName(frame({})), '(anonymous)');
    });
  });
});
