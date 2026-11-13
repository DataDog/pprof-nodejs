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
import * as tmp from 'tmp';
import {Profile} from 'pprof-format';
import {
  ExportProfilesServiceRequest,
  KeyValueAndUnit,
  ProfilesDictionary,
} from 'pprof-format/otel';

import {
  FRAME_MISSING_SOURCE_MAP,
  OtelProfilesBuilder,
  PROFILE_HAS_MISSING_SOURCE_MAPS,
  serializeHeapProfileOtel,
  serializeTimeProfileOtel,
} from '../src/otel-serializer';
import {
  PprofProfileBuilder,
  serializeHeapProfile,
  serializeTimeProfile,
} from '../src/profile-serializer';
import {
  teeVisitor,
  visitHeapProfile,
  visitTimeProfile,
} from '../src/profile-normalizer';
import {SourceMapper} from '../src/sourcemapper/sourcemapper';
import {
  AllocationProfileNode,
  AllocationProfileNodeWithStats,
  TimeProfile,
  TimeProfileNode,
} from '../src/v8-types';
import {v8HeapProfile, v8TimeProfile} from './profiles-for-tests';

const assert = require('assert');

/** Encodes and decodes the request, so that tests check the wire format. */
function roundTrip(
  request: ExportProfilesServiceRequest,
): ExportProfilesServiceRequest {
  return ExportProfilesServiceRequest.decode(request.encode());
}

function attributeValue(attribute: KeyValueAndUnit): string {
  const value = attribute.value!;
  if (value.stringValue !== undefined) return value.stringValue;
  if (value.intValue !== undefined) return String(value.intValue);
  return String(value.doubleValue);
}

/**
 * Totals of each OTel profile's values, keyed by sample type, then by stack
 * (function names, leaf first) and sorted attributes. Zero totals, such as
 * idle wall time, are left out.
 */
function otelTotals(request: ExportProfilesServiceRequest) {
  const dictionary = request.dictionary!;
  const strings = dictionary.stringTable.strings;
  const totals = new Map<string, Map<string, number>>();
  const [profiles] = request.resourceProfiles[0].scopeProfiles.map(
    s => s.profiles,
  );
  for (const profile of profiles) {
    const type = strings[profile.sampleType!.typeStrindex];
    const byKey = new Map<string, number>();
    for (const sample of profile.samples) {
      const stack = dictionary.stackTable[
        sample.stackIndex
      ].locationIndices.map(location => {
        const line = dictionary.locationTable[location].lines[0];
        return strings[
          dictionary.functionTable[line.functionIndex].nameStrindex
        ];
      });
      const attributes = sample.attributeIndices
        .map(i => dictionary.attributeTable[i])
        .map(a => `${strings[a.keyStrindex]}=${attributeValue(a)}`)
        .sort();
      const key = JSON.stringify([stack, attributes]);
      const total = sample.values.reduce<number>((t, v) => t + Number(v), 0);
      if (total !== 0) byKey.set(key, (byKey.get(key) ?? 0) + total);
    }
    totals.set(type, byKey);
  }
  return totals;
}

/** The profiles of the request's only scope. */
function profilesOf(request: ExportProfilesServiceRequest) {
  return request.resourceProfiles[0].scopeProfiles[0].profiles;
}

/** The sample types of the request's profiles, in order. */
function profileTypes(request: ExportProfilesServiceRequest): string[] {
  const strings = request.dictionary!.stringTable.strings;
  return profilesOf(request).map(p => strings[p.sampleType!.typeStrindex]);
}

function profileOfType(request: ExportProfilesServiceRequest, type: string) {
  const index = profileTypes(request).indexOf(type);
  assert.ok(index >= 0, `has a ${type} profile`);
  return profilesOf(request)[index];
}

/** The name of the leaf function of a stack. */
function leafName(
  request: ExportProfilesServiceRequest,
  sample: {stackIndex: number},
): string {
  const dictionary = request.dictionary!;
  const location = dictionary.stackTable[sample.stackIndex].locationIndices[0];
  const fn = dictionary.locationTable[location].lines[0].functionIndex;
  return dictionary.stringTable.strings[
    dictionary.functionTable[fn].nameStrindex
  ];
}

/** The totals of a pprof profile, as otelTotals(), leaving out zero values. */
function pprofTotals(profile: Profile) {
  const strings = profile.stringTable.strings;
  const totals = new Map<string, Map<string, number>>();
  profile.sampleType.forEach((sampleType, i) => {
    const byKey = new Map<string, number>();
    for (const sample of profile.sample) {
      const value = Number(sample.value[i]);
      if (value === 0) continue;
      const stack = sample.locationId.map(id => {
        const location = profile.location[Number(id) - 1];
        const fn = profile.function[Number(location.line[0].functionId) - 1];
        return strings[Number(fn.name)];
      });
      const attributes = sample.label
        .map(
          l =>
            `${strings[Number(l.key)]}=${l.str ? strings[Number(l.str)] : String(l.num)}`,
        )
        .sort();
      const key = JSON.stringify([stack, attributes]);
      byKey.set(key, (byKey.get(key) ?? 0) + value);
    }
    totals.set(strings[Number(sampleType.type)], byKey);
  });
  return totals;
}

/**
 * Checks that an OTel request has the same totals as a pprof profile. If the
 * OTel request folded the sample count into the wall profile, the count must
 * equal the number of wall observations.
 */
function assertSameTotals(otel: ExportProfilesServiceRequest, pprof: Profile) {
  const expected = pprofTotals(pprof);
  if (expected.has('sample') && !profileTypes(otel).includes('sample')) {
    expected.delete('sample');
    const sampleCount = pprof.sample.reduce(
      (total, sample) => total + Number(sample.value[0]),
      0,
    );
    const observations = profileOfType(otel, 'wall').samples.reduce(
      (total, sample) => total + sample.values.length,
      0,
    );
    assert.strictEqual(observations, sampleCount, 'wall observations');
  }
  assert.deepStrictEqual(otelTotals(otel), expected);
}

function makeLeaf(
  name: string,
  props: Partial<TimeProfileNode> = {},
): TimeProfileNode {
  return {
    name,
    scriptName: 'script.js',
    scriptId: 1,
    lineNumber: 1,
    columnNumber: 1,
    hitCount: 1,
    children: [],
    ...props,
  };
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

/**
 * A profile with labelled and unlabelled contexts, idle, GC and CPU time. As
 * in profiles from the native profiler with contexts enabled, each node has
 * exactly one context per hit.
 */
function richTimeProfile(): TimeProfile {
  const prof = makeTimeProfile([
    makeLeaf('handler', {
      hitCount: 2,
      children: [
        makeLeaf('query', {
          scriptId: 2,
          hitCount: 3,
          contexts: [
            {
              context: {endpoint: 'GET /a', span: 1n},
              timestamp: 1n,
              cpuTime: 10,
            },
            {
              context: {endpoint: 'GET /a', span: 1n},
              timestamp: 2n,
              cpuTime: 20,
            },
            {
              context: {endpoint: 'GET /b', span: 2n},
              timestamp: 3n,
              cpuTime: 30,
            },
          ],
        }),
      ],
      contexts: [
        {context: {}, timestamp: 4n, cpuTime: 40},
        {context: {endpoint: 'GET /a', retries: 3}, timestamp: 5n, cpuTime: 50},
      ],
    }),
    makeLeaf('(idle)', {
      scriptId: 3,
      hitCount: 2,
      contexts: [
        {context: {endpoint: 'GET /a'}, timestamp: 6n, cpuTime: 0},
        {context: {}, timestamp: 7n, cpuTime: 5},
      ],
    }),
    makeLeaf('(garbage collector)', {
      scriptId: 4,
      hitCount: 2,
      contexts: [
        {context: {}, timestamp: 8n, cpuTime: 0},
        {context: {}, timestamp: 9n, cpuTime: 60},
      ],
    }),
  ]);
  prof.nonJSThreadsCpuTime = 500;
  return prof;
}

describe('otel-serializer', () => {
  let dateStub: sinon.SinonStub<[], number>;

  before(() => {
    dateStub = sinon.stub(Date, 'now').returns(0);
  });
  after(() => {
    dateStub.restore();
  });

  describe('serializeTimeProfileOtel', () => {
    for (const [name, prof, samplePerContext, folded] of [
      // No contexts: every sample is summed, so the count is kept
      ['the fixture', v8TimeProfile, false, false],
      // Unlabelled contexts are summed without samplePerContext
      ['a profile with contexts', richTimeProfile(), false, false],
      [
        'a profile with contexts, with a sample per context',
        richTimeProfile(),
        true,
        true,
      ],
    ] as Array<[string, TimeProfile, boolean, boolean]>) {
      it(`has the same totals as the pprof serializer for ${name}`, () => {
        const otel = roundTrip(
          serializeTimeProfileOtel(prof, {
            intervalMicros: 1000,
            samplePerContext,
          }),
        );
        const pprof = Profile.decode(serializeTimeProfile(prof, 1000).encode());
        assertSameTotals(otel, pprof);
        assert.strictEqual(
          !profileTypes(otel).includes('sample'),
          folded,
          'sample count folded',
        );
      });
    }

    it('folds the count into wall time when every sample is an observation', () => {
      const request = roundTrip(
        serializeTimeProfileOtel(richTimeProfile(), {
          intervalMicros: 1000,
          samplePerContext: true,
        }),
      );
      const strings = request.dictionary!.stringTable.strings;
      const profiles = profilesOf(request);
      assert.deepStrictEqual(
        profiles.map(p => [
          strings[p.sampleType!.typeStrindex],
          strings[p.sampleType!.unitStrindex],
        ]),
        [
          ['wall', 'nanoseconds'],
          ['cpu', 'nanoseconds'],
        ],
      );
      for (const profile of profiles) {
        assert.strictEqual(strings[profile.periodType!.typeStrindex], 'wall');
        assert.strictEqual(Number(profile.period), 1000 * 1000);
        assert.strictEqual(
          Number(profile.durationNano),
          10 * 1000 * 1000 * 1000,
        );
      }
    });

    it('keeps the count when a sampling event is summed', () => {
      // Seven unlabelled idle hits are summed into one sample with zero wall
      // time, from which the count couldn't be recovered
      const prof = makeTimeProfile([
        makeLeaf('(idle)', {
          hitCount: 7,
          contexts: Array.from({length: 7}, (_, i) => ({
            context: {},
            timestamp: BigInt(i + 1),
            cpuTime: i === 0 ? 1 : 0,
          })),
        }),
      ]);
      const request = roundTrip(
        serializeTimeProfileOtel(prof, {intervalMicros: 1000}),
      );
      assert.deepStrictEqual(profileTypes(request), ['sample', 'wall', 'cpu']);
      assert.deepStrictEqual(
        profileOfType(request, 'sample').samples.map(s => s.values.map(Number)),
        [[7]],
      );
    });

    it('starts the profiles at the start of collection', () => {
      // 1.7e18 nanoseconds is beyond the precision of a double
      dateStub.returns(1700000000123);
      try {
        const prof = {
          ...makeTimeProfile([
            makeLeaf('foo', {
              contexts: [{context: {}, timestamp: 1700000000005_000n}],
            }),
          ]),
          startTime: 5_000_000,
          endTime: 15_000_000,
          epochOffset: 1700000000000_000 - 5_000_000,
        };
        const request = roundTrip(
          serializeTimeProfileOtel(prof, {
            intervalMicros: 1000,
            samplePerContext: true,
          }),
        );
        const wall = profileOfType(request, 'wall');
        assert.strictEqual(wall.timeUnixNano, 1700000000000_000_000n);
        const [timestamp] = wall.samples[0].timestampsUnixNano;
        const start = BigInt(wall.timeUnixNano);
        assert.ok(timestamp >= start);
        assert.ok(timestamp < start + BigInt(wall.durationNano));
      } finally {
        dateStub.returns(0);
      }
    });

    it('starts each dictionary table with its zero value', () => {
      const {dictionary} = roundTrip(
        serializeTimeProfileOtel(richTimeProfile(), {intervalMicros: 1000}),
      );
      const d = dictionary as ProfilesDictionary;
      assert.strictEqual(d.stringTable.strings[0], '');
      assert.strictEqual(d.functionTable[0].nameStrindex, 0);
      assert.deepStrictEqual(d.locationTable[0].lines, []);
      assert.deepStrictEqual(d.stackTable[0].locationIndices, []);
      assert.strictEqual(d.attributeTable[0].value, undefined);
      assert.deepStrictEqual(d.linkTable[0].traceId, new Uint8Array(16));
      assert.deepStrictEqual(d.linkTable[0].spanId, new Uint8Array(8));
      assert.strictEqual(d.mappingTable.length, 1);
    });

    it('merges timestamped samples with the same stack and attributes', () => {
      const request = roundTrip(
        serializeTimeProfileOtel(richTimeProfile(), {
          intervalMicros: 1000,
          samplePerContext: true,
        }),
      );
      const dictionary = request.dictionary!;
      const query = profileOfType(request, 'wall').samples.find(
        sample =>
          leafName(request, sample) === 'query' &&
          sample.attributeIndices.some(
            i => attributeValue(dictionary.attributeTable[i]) === 'GET /a',
          ),
      )!;
      assert.deepStrictEqual(query.values.map(Number), [1e6, 1e6]);
      assert.deepStrictEqual(query.timestampsUnixNano, [1000, 2000]);
    });

    it('sums samples without a timestamp into one value', () => {
      const prof = makeTimeProfile([makeLeaf('foo', {hitCount: 5})]);
      const request = roundTrip(
        serializeTimeProfileOtel(prof, {intervalMicros: 1000}),
      );
      assert.deepStrictEqual(
        profileOfType(request, 'sample').samples.map(s => s.values.map(Number)),
        [[5]],
      );
      const wall = profileOfType(request, 'wall');
      assert.deepStrictEqual(
        wall.samples.map(s => s.values.map(Number)),
        [[5e6]],
      );
      assert.deepStrictEqual(wall.samples[0].timestampsUnixNano, []);
    });

    it('keeps idle samples in the wall profile with zero wall time', () => {
      const request = roundTrip(
        serializeTimeProfileOtel(richTimeProfile(), {
          intervalMicros: 1000,
          samplePerContext: true,
        }),
      );
      const wall = profileOfType(request, 'wall');
      const cpu = profileOfType(request, 'cpu');
      // Idle samples are timestamped evidence of sampling, without wall time
      const idle = wall.samples.filter(s => leafName(request, s) === '(idle)');
      assert.deepStrictEqual(
        idle.flatMap(s => s.values.map(Number)),
        [0, 0],
      );
      assert.deepStrictEqual(
        idle.flatMap(s => s.timestampsUnixNano.map(Number)).sort(),
        [6000, 7000],
      );
      // Their CPU time is kept; zero CPU time is left out
      assert.deepStrictEqual(
        cpu.samples
          .filter(s => leafName(request, s) === '(idle)')
          .flatMap(s => s.values.map(Number)),
        [5],
      );
      // Non-JS threads aren't sampling events: only CPU time, no wall time
      const nonJS = (s: {stackIndex: number}) =>
        leafName(request, s) === 'Non JS threads activity';
      assert.ok(!wall.samples.some(nonJS));
      assert.ok(cpu.samples.some(nonJS));
    });

    it('turns labels into links with extractLink', () => {
      const traceId = Uint8Array.from({length: 16}, (_, i) => i + 1);
      const request = roundTrip(
        serializeTimeProfileOtel(richTimeProfile(), {
          intervalMicros: 1000,
          extractLink: labels => {
            const span = labels.find(([key]) => key === 'span');
            return {
              link: span && {
                traceId,
                spanId: Buffer.from(
                  span[1].toString(16).padStart(16, '0'),
                  'hex',
                ),
              },
              labels: labels.filter(([key]) => key !== 'span'),
            };
          },
        }),
      );
      const dictionary = request.dictionary!;
      const strings = dictionary.stringTable.strings;
      assert.strictEqual(dictionary.linkTable.length, 3);
      assert.deepStrictEqual(dictionary.linkTable[1].traceId, traceId);
      assert.ok(
        dictionary.attributeTable.every(a => strings[a.keyStrindex] !== 'span'),
      );
      const spanIds = dictionary.linkTable
        .slice(1)
        .map(link => Number(Buffer.from(link.spanId).readBigUInt64BE()));
      assert.deepStrictEqual(spanIds.sort(), [1, 2]);
      const linked = profileOfType(request, 'wall').samples.filter(
        s => s.linkIndex !== 0,
      );
      assert.ok(linked.length > 0);
      for (const sample of linked) {
        assert.ok(sample.linkIndex < dictionary.linkTable.length);
      }
    });

    it('validates and copies link IDs', () => {
      const prof = makeTimeProfile([
        makeLeaf('foo', {contexts: [{context: {span: 1}, timestamp: 1n}]}),
      ]);
      const serialize = (link: {traceId: Uint8Array; spanId: Uint8Array}) =>
        serializeTimeProfileOtel(prof, {
          intervalMicros: 1000,
          extractLink: labels => ({link, labels}),
        });
      assert.throws(
        () =>
          serialize({traceId: new Uint8Array(8), spanId: new Uint8Array(8)}),
        TypeError,
      );
      // A trace or span ID of all zeros is invalid, so either means no link
      for (const [traceByte, spanByte] of [
        [0, 0],
        [0, 1],
        [1, 0],
      ]) {
        const request = roundTrip(
          serialize({
            traceId: new Uint8Array(16).fill(traceByte),
            spanId: new Uint8Array(8).fill(spanByte),
          }),
        );
        assert.strictEqual(
          request.dictionary!.linkTable.length,
          1,
          `trace ${traceByte}, span ${spanByte}`,
        );
        assert.strictEqual(
          profileOfType(request, 'wall').samples[0].linkIndex,
          0,
        );
      }
      // A caller reusing its arrays doesn't change links already added
      const spanId = new Uint8Array(8).fill(1);
      const request = serialize({traceId: new Uint8Array(16).fill(1), spanId});
      spanId.fill(2);
      assert.deepStrictEqual(
        roundTrip(request).dictionary!.linkTable[1].spanId,
        new Uint8Array(8).fill(1),
      );
    });

    it('keeps attributes apart whatever their keys and values contain', () => {
      const prof = makeTimeProfile([
        makeLeaf('foo', {
          hitCount: 2,
          contexts: [
            {context: {a: 'b\0string\0c'}, timestamp: 1n},
            {context: {'a\0string\0b': 'c'}, timestamp: 2n},
          ],
        }),
      ]);
      const request = roundTrip(
        serializeTimeProfileOtel(prof, {intervalMicros: 1000}),
      );
      assert.strictEqual(request.dictionary!.attributeTable.length, 3);
      assert.strictEqual(profileOfType(request, 'wall').samples.length, 2);
    });

    it('sets the resource attributes and scope', () => {
      const request = roundTrip(
        serializeTimeProfileOtel(v8TimeProfile, {
          intervalMicros: 1000,
          resourceAttributes: [
            {key: 'service.name', value: {stringValue: 'svc'}},
          ],
          scope: {name: 'my-profiler', version: '1.2.3'},
        }),
      );
      const [resourceProfiles] = request.resourceProfiles;
      assert.strictEqual(
        resourceProfiles.resource!.attributes[0].value!.stringValue,
        'svc',
      );
      assert.strictEqual(
        resourceProfiles.scopeProfiles[0].scope!.name,
        'my-profiler',
      );
      assert.strictEqual(
        resourceProfiles.scopeProfiles[0].scope!.version,
        '1.2.3',
      );
      const defaultScope = roundTrip(
        serializeTimeProfileOtel(v8TimeProfile, {intervalMicros: 1000}),
      ).resourceProfiles[0].scopeProfiles[0].scope!;
      assert.strictEqual(defaultScope.name, '@datadog/pprof');
    });

    it('encodes number labels as int or double attributes', () => {
      // pprof can only hold integer labels; OTel attributes keep fractions
      const prof = makeTimeProfile([
        makeLeaf('foo', {
          contexts: [
            {context: {ratio: 0.5, count: 3, big: 2n ** 60n}, timestamp: 1n},
          ],
        }),
      ]);
      const {dictionary} = roundTrip(
        serializeTimeProfileOtel(prof, {intervalMicros: 1000}),
      );
      const strings = dictionary!.stringTable.strings;
      const byKey = new Map(
        dictionary!.attributeTable
          .slice(1)
          .map(a => [strings[a.keyStrindex], a.value!]),
      );
      assert.strictEqual(byKey.get('ratio')!.doubleValue, 0.5);
      assert.strictEqual(Number(byKey.get('count')!.intValue), 3);
      assert.strictEqual(BigInt(byKey.get('big')!.intValue!), 2n ** 60n);
    });

    it('carries the columns of frames', () => {
      const prof = makeTimeProfile([
        makeLeaf('foo', {lineNumber: 3, columnNumber: 7}),
      ]);
      const {dictionary} = roundTrip(
        serializeTimeProfileOtel(prof, {intervalMicros: 1000}),
      );
      const line = dictionary!.locationTable[1].lines[0];
      assert.strictEqual(Number(line.line), 3);
      assert.strictEqual(Number(line.column), 7);
    });

    it('keeps functions from different original files apart', () => {
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
      const request = roundTrip(
        serializeTimeProfileOtel(prof, {intervalMicros: 1000, sourceMapper}),
      );
      const dictionary = request.dictionary!;
      const strings = dictionary.stringTable.strings;
      assert.deepStrictEqual(
        dictionary.functionTable
          .slice(1)
          .map(f => strings[f.filenameStrindex])
          .sort(),
        ['/app/a.ts', '/app/b.ts'],
      );
      assert.strictEqual(dictionary.locationTable.length, 3);
      assert.strictEqual(dictionary.stackTable.length, 3);
    });

    it('shares functions that encode the same, across scripts', () => {
      const prof = makeTimeProfile([
        makeLeaf('foo', {scriptId: 1, lineNumber: 5}),
        makeLeaf('foo', {scriptId: 2, lineNumber: 7}),
      ]);
      const {dictionary} = roundTrip(
        serializeTimeProfileOtel(prof, {intervalMicros: 1000}),
      );
      assert.strictEqual(dictionary!.functionTable.length, 2);
      assert.strictEqual(dictionary!.locationTable.length, 3);
    });

    it('marks frames with missing source maps and their profiles', async () => {
      const fs = await import('fs');
      const path = await import('path');
      const dir = tmp.dirSync().name;
      const script = path.join(dir, 'missing-map.js');
      fs.writeFileSync(script, '//# sourceMappingURL=nonexistent.js.map\n');
      const sourceMapper = await SourceMapper.create([dir]);
      const prof = makeTimeProfile([
        makeLeaf('minified', {scriptName: script}),
        makeLeaf('plain', {scriptId: 2}),
      ]);
      const request = roundTrip(
        serializeTimeProfileOtel(prof, {intervalMicros: 1000, sourceMapper}),
      );
      const dictionary = request.dictionary!;
      const strings = dictionary.stringTable.strings;
      const attributes = (indices: number[]) =>
        indices.map(i => {
          const attribute = dictionary.attributeTable[i];
          return [strings[attribute.keyStrindex], attribute.value!.boolValue];
        });

      const marked = dictionary.locationTable
        .slice(1)
        .map(location => [
          strings[
            dictionary.functionTable[location.lines[0].functionIndex]
              .nameStrindex
          ],
          attributes(location.attributeIndices),
        ]);
      assert.deepStrictEqual(marked.sort(), [
        ['minified', [[FRAME_MISSING_SOURCE_MAP, true]]],
        ['plain', []],
      ]);
      for (const profile of profilesOf(request)) {
        assert.deepStrictEqual(attributes(profile.attributeIndices), [
          [PROFILE_HAS_MISSING_SOURCE_MAPS, true],
        ]);
      }
    });

    it('marks nothing when all source maps are found', () => {
      const request = roundTrip(
        serializeTimeProfileOtel(richTimeProfile(), {intervalMicros: 1000}),
      );
      assert.ok(
        request.dictionary!.locationTable.every(
          l => l.attributeIndices.length === 0,
        ),
      );
      for (const profile of profilesOf(request)) {
        assert.deepStrictEqual(profile.attributeIndices, []);
      }
    });
  });

  describe('serializeHeapProfileOtel', () => {
    it('has the same totals as the pprof serializer', () => {
      const otel = roundTrip(
        serializeHeapProfileOtel(v8HeapProfile, {
          startTimeNanos: 0n,
          intervalBytes: 512,
        }),
      );
      const pprof = Profile.decode(
        serializeHeapProfile(v8HeapProfile, 0, 512).encode(),
      );
      assertSameTotals(otel, pprof);
      assert.deepStrictEqual(profileTypes(otel), [
        'inuse_objects',
        'inuse_space',
      ]);
    });

    it('has allocation profiles in allocation mode', () => {
      const withStats = (
        node: AllocationProfileNode,
      ): AllocationProfileNodeWithStats => ({
        ...node,
        allocations: node.allocations.map(a => ({
          inuseObjects: a.count,
          inuseSpaceBytes: a.sizeBytes * a.count,
          allocObjects: 2 * a.count,
          allocSpaceBytes: 2 * a.sizeBytes * a.count,
        })),
        children: node.children.map(withStats),
      });
      const prof = withStats(v8HeapProfile);
      const otel = roundTrip(
        serializeHeapProfileOtel(prof, {
          startTimeNanos: 0n,
          intervalBytes: 512,
          allocations: true,
        }),
      );
      const pprof = Profile.decode(
        serializeHeapProfile(
          prof,
          0,
          512,
          undefined,
          undefined,
          undefined,
          true,
        ).encode(),
      );
      assertSameTotals(otel, pprof);
      assert.strictEqual(profilesOf(otel).length, 4);
    });
  });

  describe('OtelProfilesBuilder', () => {
    const heapNode = (
      name: string,
      props: Partial<AllocationProfileNode> = {},
    ): AllocationProfileNode => ({
      name,
      scriptName: 'script.js',
      scriptId: 1,
      lineNumber: 1,
      columnNumber: 1,
      allocations: [{sizeBytes: 64, count: 2}],
      children: [],
      ...props,
    });
    const heapRoot = (children: AllocationProfileNode[]) =>
      heapNode('(root)', {
        scriptName: '',
        scriptId: 0,
        allocations: [],
        children,
      });
    const timeOptions = {intervalMicros: 1000, samplePerContext: true};

    it('puts several profiles in one request, with the same totals', () => {
      const builder = new OtelProfilesBuilder();
      visitTimeProfile(richTimeProfile(), builder.visitor(), timeOptions);
      visitHeapProfile(v8HeapProfile, builder.visitor(), {
        startTimeNanos: 0n,
        intervalBytes: 512,
      });
      const combined = roundTrip(builder.build());
      assert.deepStrictEqual(profileTypes(combined), [
        'wall',
        'cpu',
        'inuse_objects',
        'inuse_space',
      ]);
      const separate = new Map([
        ...otelTotals(
          roundTrip(serializeTimeProfileOtel(richTimeProfile(), timeOptions)),
        ),
        ...otelTotals(
          roundTrip(
            serializeHeapProfileOtel(v8HeapProfile, {
              startTimeNanos: 0n,
              intervalBytes: 512,
            }),
          ),
        ),
      ]);
      assert.deepStrictEqual(otelTotals(combined), separate);
    });

    it('gives all profiles the union of their time ranges', () => {
      // A time profile of 10 seconds from 1e18, and a heap profile taken 2
      // seconds after it ended
      const startNanos = 1_000_000_000_000_000_000n;
      dateStub.returns(Number((startNanos + 12_000_000_000n) / 1_000_000n));
      try {
        const builder = new OtelProfilesBuilder();
        visitTimeProfile(
          {
            ...makeTimeProfile([
              makeLeaf('foo', {
                contexts: [{context: {}, timestamp: 1_000_000_000_005_000n}],
              }),
            ]),
            startTime: 0,
            endTime: 10_000_000,
            epochOffset: 1_000_000_000_000_000,
          },
          builder.visitor(),
          timeOptions,
        );
        visitHeapProfile(v8HeapProfile, builder.visitor(), {
          startTimeNanos: startNanos + 12_000_000_000n,
          intervalBytes: 512,
        });
        const request = roundTrip(builder.build());
        const ranges = profilesOf(request).map(p => [
          BigInt(p.timeUnixNano),
          BigInt(p.durationNano),
        ]);
        assert.strictEqual(ranges.length, 4);
        for (const range of ranges) {
          assert.deepStrictEqual(range, [startNanos, 12_000_000_000n]);
        }
        // A later build starts again from each profile's own range
        visitHeapProfile(v8HeapProfile, builder.visitor(), {
          startTimeNanos: startNanos + 20_000_000_000n,
          intervalBytes: 512,
        });
        for (const profile of profilesOf(roundTrip(builder.build()))) {
          assert.strictEqual(BigInt(profile.timeUnixNano), startNanos);
          assert.strictEqual(BigInt(profile.durationNano), 20_000_000_000n);
        }
      } finally {
        dateStub.returns(0);
      }
    });

    it('shares dictionary entries between profiles', () => {
      const builder = new OtelProfilesBuilder();
      visitTimeProfile(
        makeTimeProfile([
          makeLeaf('shared', {children: [makeLeaf('timeOnly', {scriptId: 2})]}),
        ]),
        builder.visitor(),
        timeOptions,
      );
      visitHeapProfile(
        heapRoot([
          heapNode('shared', {children: [heapNode('heapOnly', {scriptId: 3})]}),
        ]),
        builder.visitor(),
        {startTimeNanos: 0n, intervalBytes: 512},
      );
      const request = roundTrip(builder.build());
      const dictionary = request.dictionary!;
      const strings = dictionary.stringTable.strings;
      const names = dictionary.functionTable
        .slice(1)
        .map(f => strings[f.nameStrindex]);
      assert.deepStrictEqual(names.sort(), ['heapOnly', 'shared', 'timeOnly']);
      // No duplicate locations or stacks
      const keys = (items: unknown[]) =>
        items.map(item => JSON.stringify(item));
      const locations = keys(dictionary.locationTable.map(l => l.lines));
      assert.strictEqual(new Set(locations).size, locations.length);
      const stacks = keys(dictionary.stackTable.map(st => st.locationIndices));
      assert.strictEqual(new Set(stacks).size, stacks.length);
      // The frame both profiles share is one location, with one stack
      const sharedStack = (samples: Array<{stackIndex: number}>) =>
        samples.find(s => leafName(request, s) === 'shared')!.stackIndex;
      assert.strictEqual(
        sharedStack(profileOfType(request, 'sample').samples),
        sharedStack(profileOfType(request, 'inuse_space').samples),
      );
    });

    it("doesn't fold a count that observations wouldn't match", () => {
      // A zero-count sample with a period value would add an observation that
      // isn't a sampling event, so the count can't be its number of
      // observations
      const builder = new OtelProfilesBuilder();
      const visitor = builder.visitor();
      const cpu = {type: 'cpu', unit: 'nanoseconds'};
      visitor.metadata({
        sampleTypes: [{type: 'sample', unit: 'count'}, cpu],
        periodType: cpu,
        period: 1,
        timeNanos: 0n,
      });
      const stackId = visitor.stack([
        visitor.frame({name: 'f', file: 'a.js', missingMapFile: false}),
      ]);
      visitor.sample({stackId, values: [1, 100], labels: [], timestamp: 1n});
      visitor.sample({stackId, values: [0, 50], labels: [], timestamp: 2n});
      visitor.end!({hasMissingMapFiles: false});
      const request = roundTrip(builder.build());
      assert.deepStrictEqual(profileTypes(request), ['sample', 'cpu']);
      assert.deepStrictEqual(
        profileOfType(request, 'sample').samples.map(s => s.values.map(Number)),
        [[1]],
      );
    });

    const cpu = {type: 'cpu', unit: 'nanoseconds'};
    /** Adds a profile with one CPU sample through the visitor. */
    function visitCpuSample(
      visitor: ReturnType<OtelProfilesBuilder['visitor']>,
      hasMissingMapFiles = false,
    ) {
      visitor.metadata({
        sampleTypes: [cpu],
        periodType: cpu,
        period: 1,
        timeNanos: 0n,
      });
      const stackId = visitor.stack([
        visitor.frame({name: 'f', file: 'a.js', missingMapFile: false}),
      ]);
      visitor.sample({stackId, values: [10], labels: []});
      visitor.end!({hasMissingMapFiles});
    }

    it("sets a visit's attributes and original payload on its profiles", () => {
      const builder = new OtelProfilesBuilder();
      const payload = new TextEncoder().encode('{"start":"now"}');
      visitTimeProfile(
        richTimeProfile(),
        builder.visitor({
          attributes: [
            {key: 'datadog.profile.snapshot', value: {stringValue: 'periodic'}},
            {
              key: 'datadog.profile.endpoint_counts',
              value: {
                kvlistValue: {
                  values: [
                    {key: 'GET /a', value: {intValue: 3}},
                    {key: 'GET /b', value: {intValue: 1}},
                  ],
                },
              },
            },
          ],
          originalPayload: {format: 'datadog-event-json', payload},
        }),
        timeOptions,
      );
      visitCpuSample(builder.visitor());
      const request = roundTrip(builder.build());
      const dictionary = request.dictionary!;
      const strings = dictionary.stringTable.strings;
      const [wall, cpuOfTime, cpuAlone] = profilesOf(request);
      for (const profile of [wall, cpuOfTime]) {
        const attributes = profile.attributeIndices.map(
          i => dictionary.attributeTable[i],
        );
        assert.deepStrictEqual(
          attributes.map(a => strings[a.keyStrindex]),
          ['datadog.profile.snapshot', 'datadog.profile.endpoint_counts'],
        );
        assert.strictEqual(attributes[0].value!.stringValue, 'periodic');
        assert.deepStrictEqual(
          attributes[1].value!.kvlistValue!.values.map(kv => [
            kv.key,
            Number(kv.value!.intValue),
          ]),
          [
            ['GET /a', 3],
            ['GET /b', 1],
          ],
        );
        assert.strictEqual(profile.originalPayloadFormat, 'datadog-event-json');
        assert.deepStrictEqual(
          Buffer.from(profile.originalPayload),
          Buffer.from(payload),
        );
      }
      // The two profiles of the visit share the attribute table entries
      assert.deepStrictEqual(wall.attributeIndices, cpuOfTime.attributeIndices);
      // Another visit's profiles get neither
      assert.deepStrictEqual(cpuAlone.attributeIndices, []);
      assert.strictEqual(cpuAlone.originalPayloadFormat, '');
      assert.strictEqual(cpuAlone.originalPayload.length, 0);
    });

    it('interns profile attributes by value', () => {
      const builder = new OtelProfilesBuilder();
      const list = (...ints: number[]) => ({
        arrayValue: {values: ints.map(intValue => ({intValue}))},
      });
      visitCpuSample(
        builder.visitor({
          attributes: [
            {key: 'a', value: list(1, 2)},
            {key: 'n', value: {intValue: 1}},
          ],
        }),
      );
      visitCpuSample(
        builder.visitor({
          attributes: [
            {key: 'a', value: list(1, 2)},
            {key: 'a', value: list(2, 1)},
            {key: 'n', value: {doubleValue: 1}},
          ],
        }),
      );
      const [first, second] = profilesOf(roundTrip(builder.build()));
      // Equal values share an entry; order and type tell values apart
      assert.strictEqual(first.attributeIndices[0], second.attributeIndices[0]);
      assert.strictEqual(
        new Set([...first.attributeIndices, ...second.attributeIndices]).size,
        4,
      );
    });

    it('adds the missing source map attribute to the visit attributes', () => {
      const builder = new OtelProfilesBuilder();
      visitCpuSample(
        builder.visitor({
          attributes: [{key: 'k', value: {stringValue: 'v'}}],
        }),
        true,
      );
      const request = roundTrip(builder.build());
      const dictionary = request.dictionary!;
      const strings = dictionary.stringTable.strings;
      assert.deepStrictEqual(
        profilesOf(request)[0].attributeIndices.map(
          i => strings[dictionary.attributeTable[i].keyStrindex],
        ),
        ['k', PROFILE_HAS_MISSING_SOURCE_MAPS],
      );
    });

    it('gives samples without frames the zero-value stack', () => {
      const builder = new OtelProfilesBuilder();
      const visitor = builder.visitor();
      const timeline = {type: 'timeline', unit: 'nanoseconds'};
      visitor.metadata({
        sampleTypes: [timeline],
        periodType: timeline,
        period: 1,
        timeNanos: 0n,
      });
      const stackId = visitor.stack([]);
      assert.strictEqual(stackId, 0);
      visitor.sample({stackId, values: [5], labels: [], timestamp: 1n});
      visitor.end!({hasMissingMapFiles: false});
      const request = roundTrip(builder.build());
      assert.strictEqual(request.dictionary!.stackTable.length, 1);
      assert.deepStrictEqual(
        profilesOf(request)[0].samples.map(s => [s.stackIndex, s.values]),
        [[0, [5]]],
      );
    });

    it('marks only the profiles that have missing source maps', async () => {
      const fs = await import('fs');
      const path = await import('path');
      const dir = tmp.dirSync().name;
      const script = path.join(dir, 'missing-map.js');
      fs.writeFileSync(script, '//# sourceMappingURL=nonexistent.js.map\n');
      const sourceMapper = await SourceMapper.create([dir]);
      const builder = new OtelProfilesBuilder();
      visitTimeProfile(
        makeTimeProfile([
          makeLeaf('minified', {
            scriptName: script,
            contexts: [{context: {}, timestamp: 1n, cpuTime: 1}],
          }),
        ]),
        builder.visitor(),
        {...timeOptions, sourceMapper},
      );
      visitHeapProfile(v8HeapProfile, builder.visitor(), {
        startTimeNanos: 0n,
        intervalBytes: 512,
      });
      const request = roundTrip(builder.build());
      const strings = request.dictionary!.stringTable.strings;
      const flagged = profilesOf(request).map(p => [
        strings[p.sampleType!.typeStrindex],
        p.attributeIndices.length,
      ]);
      assert.deepStrictEqual(flagged, [
        ['wall', 1],
        ['cpu', 1],
        ['inuse_objects', 0],
        ['inuse_space', 0],
      ]);
    });
  });

  describe('package exports', () => {
    it('exports the OTel builder and serializers', async () => {
      const pprof = await import('../src');
      assert.strictEqual(pprof.OtelProfilesBuilder, OtelProfilesBuilder);
      assert.strictEqual(
        pprof.serializeTimeProfileOtel,
        serializeTimeProfileOtel,
      );
      assert.strictEqual(
        pprof.serializeHeapProfileOtel,
        serializeHeapProfileOtel,
      );
    });
  });

  describe('teeVisitor', () => {
    // Time profiles are timestamped when normalized; the stubbed Date.now()
    // makes separate visits comparable
    it('builds pprof and OTel from one visit, as separate visits would', () => {
      const options = {intervalMicros: 1000, samplePerContext: true};
      // Visit the OTel builder first, so that the pprof builder sees
      // different frame and stack IDs than it returns itself
      const otel = new OtelProfilesBuilder();
      visitHeapProfile(v8HeapProfile, otel.visitor(), {
        startTimeNanos: 0n,
        intervalBytes: 512,
      });
      const pprof = new PprofProfileBuilder();
      visitTimeProfile(
        richTimeProfile(),
        teeVisitor(otel.visitor(), pprof.visitor()),
        options,
      );

      const separateOtel = new OtelProfilesBuilder();
      visitHeapProfile(v8HeapProfile, separateOtel.visitor(), {
        startTimeNanos: 0n,
        intervalBytes: 512,
      });
      visitTimeProfile(richTimeProfile(), separateOtel.visitor(), options);
      const separatePprof = new PprofProfileBuilder();
      visitTimeProfile(richTimeProfile(), separatePprof.visitor(), options);

      assert.deepStrictEqual(
        Buffer.from(otel.build().encode()),
        Buffer.from(separateOtel.build().encode()),
      );
      assert.deepStrictEqual(
        Buffer.from(pprof.build().encode()),
        Buffer.from(separatePprof.build().encode()),
      );
    });

    it('is exported from the package', async () => {
      const pprof = await import('../src');
      assert.strictEqual(pprof.teeVisitor, teeVisitor);
    });
  });

  after(() => {
    tmp.setGracefulCleanup();
  });
});
