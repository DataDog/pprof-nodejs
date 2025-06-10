import {parentPort} from 'node:worker_threads';
import {time} from '../src/index';

const delay = (ms: number) => new Promise(res => setTimeout(res, ms));

const DURATION_MILLIS = 1000;
const INTERVAL_MICROS = 10000;
const withContexts =
  process.platform === 'darwin' || process.platform === 'linux';

time.start({
  durationMillis: DURATION_MILLIS,
  intervalMicros: INTERVAL_MICROS,
  withContexts: withContexts,
  collectCpuTime: withContexts,
  collectAsyncId: false,
});

parentPort?.on('message', () => {
  delay(50).then(() => {
    parentPort?.postMessage('hello');
  });
});
