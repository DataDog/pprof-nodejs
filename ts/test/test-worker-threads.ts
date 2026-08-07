import {execFile, ChildProcess} from 'child_process';
import {promisify} from 'util';
import {Worker} from 'worker_threads';

const exec = promisify(execFile);

describe('Worker Threads', () => {
  let child: ChildProcess | undefined;

  afterEach(() => {
    // A mocha timeout rejects the test but leaves the spawned process running,
    // and `npm test` runs mocha without --exit, so mocha waits for the event
    // loop to drain before exiting. A live child keeps its process handle on
    // the loop, so a child that outlives its test holds the entire run open —
    // if that child is itself wedged, until the CI job limit rather than until
    // the test times out. Hooks still run after a timeout, so reap it here.
    if (child?.exitCode === null && child.signalCode === null) {
      child.kill();
    }
    child = undefined;
  });

  // eslint-ignore-next-line prefer-array-callback
  it('should work', function () {
    this.timeout(20000);
    const nbWorkers = 2;
    const running = exec('node', ['./out/test/worker.js', String(nbWorkers)]);
    child = running.child;
    return running;
  });

  it('should not crash when worker is terminated', async function () {
    this.timeout(30000);
    const nruns = 5;
    const concurrentWorkers = 20;
    for (let i = 0; i < nruns; i++) {
      const workers = [];
      for (let j = 0; j < concurrentWorkers; j++) {
        const worker = new Worker('./out/test/worker2.js');
        worker.postMessage('hello');

        worker.on('message', () => {
          void worker.terminate();
        });

        workers.push(
          new Promise<void>((resolve, reject) => {
            worker.on('exit', exitCode => {
              if (exitCode === 1) {
                resolve();
              } else {
                reject(new Error('Worker exited with code 0'));
              }
            });
          }),
        );
      }
      await Promise.all(workers);
    }
  });
});
