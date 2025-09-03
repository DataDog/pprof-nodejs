import {parentPort} from 'node:worker_threads';
import {time} from '../src/index';
import {satisfies} from 'semver';

const delay = (ms: number) => new Promise(res => setTimeout(res, ms));

const DURATION_MILLIS = 1000;
const INTERVAL_MICROS = 10000;
const withContexts =
  process.platform === 'darwin' || process.platform === 'linux';

const useCPED =
  satisfies(process.versions.node, '>=24.0.0') &&
  !process.execArgv.includes('--no-async-context-frame');

const collectAsyncId =
  withContexts && satisfies(process.versions.node, '>=24.0.0');

time.start({
  durationMillis: DURATION_MILLIS,
  intervalMicros: INTERVAL_MICROS,
  withContexts: withContexts,
  collectCpuTime: withContexts,
  collectAsyncId: collectAsyncId,
  useCPED: useCPED,
});

parentPort?.on('message', () => {
  delay(50).then(() => {
    parentPort?.postMessage('hello');
  });
});
