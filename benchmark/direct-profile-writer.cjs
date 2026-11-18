'use strict';

const assert = require('node:assert/strict');
const {performance} = require('node:perf_hooks');
const {visitTimeProfile} = require('../out/src/profile-normalizer');
const {
  serializeEncodedProfile,
  serializeTimeProfile,
} = require('../out/src/profile-serializer');

const count = Number(process.env.SAMPLES ?? 6500);
const depth = Number(process.env.DEPTH ?? 16);
const stackCount = Number(process.env.STACKS ?? 64);
const iterations = Number(process.env.ITERATIONS ?? 80);
const contexts = Array.from({length: count}, (_, index) => ({
  timestamp: 1700000000000000n + BigInt(index) * 10000n,
  cpuTime: 1000,
}));
let leaf = {
  name: 'dispatcher',
  scriptName: '/service.js',
  hitCount: 0,
  children: Array.from({length: stackCount}, (_, index) => {
    const from = Math.floor((index * count) / stackCount);
    const to = Math.floor(((index + 1) * count) / stackCount);
    return {
      name: `handler${index}`,
      scriptName: '/service.js',
      hitCount: to - from,
      children: [],
      contexts: contexts.slice(from, to),
    };
  }),
};
for (let index = 0; index < depth; index++) {
  leaf = {
    name: `frame${index}`,
    scriptName: '/service.js',
    hitCount: 0,
    children: [leaf],
  };
}
const profile = {
  startTime: 0,
  endTime: count * 10000,
  epochOffset: 1700000000000000,
  hasCpuTime: true,
  topDownRoot: {name: '(root)', scriptName: '', hitCount: 0, children: [leaf]},
};
const intervalMicros = 10000;
const generateLabels = ({context}) => ({
  'thread name': 'Main',
  'thread id': 1,
  'span id': (context.timestamp << 12n) | 13n,
  end_timestamp_ns: context.timestamp * 1000n,
});
const lowCardinality = ['thread name', 'thread id'];
const paths = {
  objects: () =>
    serializeTimeProfile(
      profile,
      intervalMicros,
      undefined,
      true,
      generateLabels,
      lowCardinality,
    ),
  writer: () =>
    serializeEncodedProfile(
      visitor =>
        visitTimeProfile(profile, visitor, {
          intervalMicros,
          generateLabels,
          recomputeSamplingInterval: true,
        }),
      lowCardinality,
      'drop',
    ),
};

function summarize(values) {
  values.sort((left, right) => left - right);
  return [0.5, 0.95].map(percentile =>
    Number(values[Math.floor(values.length * percentile)].toFixed(2)),
  );
}

async function main() {
  const now = Date.now;
  Date.now = () => 1700000000000;
  const writer = paths.writer();
  const objects = paths.objects();
  Date.now = now;
  assert.deepEqual(await writer.encodeAsync(), await objects.encodeAsync());
  const results = {objects: [], writer: []};
  for (let index = -20; index < iterations; index++) {
    const order =
      index % 2 === 0 ? ['objects', 'writer'] : ['writer', 'objects'];
    for (const name of order) {
      const start = performance.now();
      const collected = paths[name]();
      const pause = performance.now() - start;
      await collected.encodeAsync();
      if (index >= 0)
        results[name].push({pause, total: performance.now() - start});
    }
  }
  for (const [name, samples] of Object.entries(results)) {
    let collected;
    global.gc();
    const before = process.memoryUsage();
    collected = paths[name]();
    global.gc();
    const retained = process.memoryUsage();
    console.log(name, {
      collectionMs: summarize(samples.map(sample => sample.pause)),
      totalMs: summarize(samples.map(sample => sample.total)),
      retainedKiB: Math.round(
        (retained.heapUsed +
          retained.arrayBuffers -
          before.heapUsed -
          before.arrayBuffers) /
          1024,
      ),
    });
    await collected.encodeAsync();
  }
}

main().catch(error => {
  console.error(error);
  process.exitCode = 1;
});
