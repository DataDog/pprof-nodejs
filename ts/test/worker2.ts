import {parentPort} from 'node:worker_threads';
import {time} from '../src/index';
import {satisfies} from 'semver';
import {isAsyncContextFrameActive} from '../src/async-context-frame';

const delay = (ms: number) => new Promise(res => setTimeout(res, ms));

const DURATION_MILLIS = 1000;
const INTERVAL_MICROS = 10000;
const withContexts =
  process.platform === 'darwin' || process.platform === 'linux';

const useCPED =
  withContexts &&
  isAsyncContextFrameActive() &&
  satisfies(process.versions.node, '>=22.7.0');

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
  void delay(50).then(() => {
    parentPort?.postMessage('hello');
  });
});
