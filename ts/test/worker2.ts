import {parentPort} from 'node:worker_threads';
import {time} from '../src/index';
import {satisfies} from 'semver';
import {isAsyncContextFrameActive} from '../src/async-context-frame';

const delay = (ms: number) => new Promise(res => setTimeout(res, ms));

const DURATION_MILLIS = 1000;
const INTERVAL_MICROS = 10000;
const withContexts =
  process.platform === 'darwin' || process.platform === 'linux';

const useCPED = withContexts && isAsyncContextFrameActive();

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

function listen() {
  parentPort?.on('message', () => {
    void delay(50).then(() => {
      parentPort?.postMessage('hello');
    });
  });
}

// Establish a sample context, and do it around the listener registration so
// the async context frame holding it stays reachable until we are terminated.
// That leaves a live PersistentContextPtr for ~WallProfiler to walk when it
// runs from the environment cleanup hook; with an empty list the walk is a
// no-op and the teardown path goes untested.
if (useCPED) {
  time.runWithContext({worker: 'worker2'}, listen);
} else {
  time.setContext({worker: 'worker2'});
  listen();
}
