// eslint-disable-next-line node/no-unsupported-features/node-builtins
import {execFile} from 'child_process';
import {promisify} from 'util';
import {Worker} from 'worker_threads';

const exec = promisify(execFile);

describe('Worker Threads', () => {
  // eslint-ignore-next-line prefer-array-callback
  it('should work', function () {
    this.timeout(20000);
    const nbWorkers = 2;
    return exec('node', ['./out/test/worker.js', String(nbWorkers)]);
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
          worker.terminate();
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
          })
        );
      }
      await Promise.all(workers);
    }
  });
});
