/**
 * Child process entrypoint for CPED freelist trimming regression test.
 *
 * This file is intentionally not named `test-*.ts` so mocha won't execute it
 * directly. It is executed as a standalone Node.js script from the test suite.
 */

import assert from 'assert';
import {AsyncLocalStorage} from 'async_hooks';
import {satisfies} from 'semver';

// Require from the built output to match how tests run in CI (out/test/*).
// eslint-disable-next-line @typescript-eslint/no-var-requires
const {time} = require('../src');

function isUseCPEDEnabled(): boolean {
  return (
    (satisfies(process.versions.node, '>=24.0.0') &&
      !process.execArgv.includes('--no-async-context-frame')) ||
    (satisfies(process.versions.node, '>=22.7.0') &&
      process.execArgv.includes('--experimental-async-context-frame'))
  );
}

async function main() {
  if (process.platform !== 'darwin' && process.platform !== 'linux') {
    return; // unsupported in this repo's time profiler tests
  }

  // This regression targets the CPED path.
  const useCPED = isUseCPEDEnabled();
  if (!useCPED) return;

  const gc = global.gc;
  if (typeof gc !== 'function') {
    throw new Error('expected --expose-gc');
  }
  const runGc = gc as () => void;

  // Ensure an async context frame exists to hold the profiler context.
  new AsyncLocalStorage().enterWith(1);

  time.start({
    intervalMicros: 1000,
    durationMillis: 10_000,
    withContexts: true,
    lineNumbers: false,
    useCPED: true,
  });

  const als = new AsyncLocalStorage<number>();

  const waveSize = 20_000;
  const maxWaves = 6;
  const minDelta = 5_000;
  const minTotalBeforeGc = 40_000;
  const debug = process.env.DEBUG_CPED_TEST === '1';
  const log = (...args: unknown[]) => {
    if (debug) {
      // eslint-disable-next-line no-console
      console.error(...args);
    }
  };

  async function gcAndYield(times = 3) {
    for (let i = 0; i < times; i++) {
      runGc();
      await new Promise(resolve => setImmediate(resolve));
    }
  }

  async function runWave(count: number): Promise<void> {
    const tasks: Array<Promise<void>> = [];
    for (let i = 0; i < count; i++) {
      const value = i;
      tasks.push(
        als.run(value, async () => {
          await new Promise(resolve => setTimeout(resolve, 0));
          time.setContext({v: value});
        })
      );
    }
    await Promise.all(tasks);
  }

  const baseline = time.getMetrics().totalAsyncContextCount;
  let totalBeforeGc = baseline;
  let wavesRun = 0;
  while (wavesRun < maxWaves && totalBeforeGc < minTotalBeforeGc) {
    await runWave(waveSize);
    totalBeforeGc = time.getMetrics().totalAsyncContextCount;
    wavesRun++;
    log('wave', wavesRun, 'totalBeforeGc', totalBeforeGc);
  }
  const metricsBeforeGc = time.getMetrics();
  log('baseline', baseline, 'metricsBeforeGc', metricsBeforeGc);
  assert(
    totalBeforeGc - baseline >= minDelta,
    `test did not create enough async contexts (baseline=${baseline}, total=${totalBeforeGc})`
  );
  assert(
    totalBeforeGc >= minTotalBeforeGc,
    `test did not reach target async context count (total=${totalBeforeGc})`
  );

  await gcAndYield(6);
  const metricsAfterGc = time.getMetrics();
  const totalAfterGc = metricsAfterGc.totalAsyncContextCount;
  log('metricsAfterGc', metricsAfterGc);
  const maxAllowed = Math.floor(totalBeforeGc * 0.75);
  assert(
    totalAfterGc <= maxAllowed,
    `expected trimming; before=${totalBeforeGc}, after=${totalAfterGc}, max=${maxAllowed}`
  );

  time.stop(false);
}

main().catch(err => {
  // Ensure the child exits non-zero on failure.
  // eslint-disable-next-line no-console
  console.error(err);
  // eslint-disable-next-line no-process-exit
  process.exit(1);
});
