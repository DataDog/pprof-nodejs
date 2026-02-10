import assert from 'node:assert/strict';
import {setTimeout as delay} from 'node:timers/promises';

export async function runSmoke(pprof) {
  assert.equal(typeof pprof.time.start, 'function');
  assert.equal(typeof pprof.time.stop, 'function');
  assert.equal(typeof pprof.heap.start, 'function');
  assert.equal(typeof pprof.heap.profile, 'function');
  assert.equal(typeof pprof.encode, 'function');

  pprof.time.start({
    durationMillis: 25,
    intervalMicros: 1_000,
    withContexts: true,
  });
  pprof.time.setContext({runtime: 'bun-smoke'});
  await delay(25);
  const wallProfile = pprof.time.stop();
  assert.ok(Array.isArray(wallProfile.sample));
  assert.ok(wallProfile.sample.length > 0);

  const encodedWallProfile = await pprof.encode(wallProfile);
  assert.ok(encodedWallProfile.length > 0);

  pprof.heap.start(128 * 1024, 64);
  const heapProfile = pprof.heap.profile();
  pprof.heap.stop();
  assert.ok(heapProfile.sample.length > 0);

  const encodedHeapProfile = await pprof.encode(heapProfile);
  assert.ok(encodedHeapProfile.length > 0);

  return {
    ok: true,
    wallBytes: encodedWallProfile.length,
    heapBytes: encodedHeapProfile.length,
  };
}
