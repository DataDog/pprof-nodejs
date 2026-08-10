import {Worker, isMainThread, workerData, parentPort} from 'worker_threads';
import {pbkdf2} from 'crypto';
import {time} from '../src/index';
import {Profile, ValueType} from 'pprof-format';
import {getAndVerifyPresence, getAndVerifyString} from './profiles-for-tests';
import {satisfies} from 'semver';

import assert from 'assert';
import {appendFileSync} from 'fs';

const DURATION_MILLIS = 1000;
const intervalMicros = 10000;
const withContexts =
  process.platform === 'darwin' || process.platform === 'linux';
const useCPED =
  withContexts &&
  ((satisfies(process.versions.node, '>=24.0.0') &&
    !process.execArgv.includes('--no-async-context-frame')) ||
    (satisfies(process.versions.node, '>=22.7.0') &&
      process.execArgv.includes('--experimental-async-context-frame')));
const collectAsyncId =
  withContexts && satisfies(process.versions.node, '>=24.0.0');

// Phase instrumentation for the intermittent stall this test hits on CI:
// `should work` times out at 20s while the workload is almost entirely
// wall-clock deadline work that finishes in ~4-5s regardless of machine speed
// (with the deadline cut to 1ms the whole thing runs in ~60ms), so a run that
// reaches 20s is wedged rather than slow. Reproduces on Linux Node 22 at
// roughly one job in four, as well as on darwin-arm64 and win32.
//
// Collecting the marks costs a string and an array push; leaving them in means
// anyone hunting the stall can see which phase it died in rather than
// reconstructing it. Set DD_WORKER_TRACE_FILE to also append each mark to a
// file as it happens — a file rather than stdout because execFile buffers a
// child's output and only delivers it when the child closes, so a wedged child
// produces nothing, and appending as we go means the trail survives the child
// being killed.
//
// Deliberately no timer, no diagnostic-report call and no process.exit here.
// An earlier version armed a watchdog that did all three and wedged CI jobs
// for 40+ minutes; bisection pinned it to arming the timer, though the
// mechanism was never identified. Nothing below runs on a schedule.
const TRACE_FILE = process.env.DD_WORKER_TRACE_FILE;
const START = Date.now();
const progress: string[] = [];

function mark(msg: string): void {
  const line = `+${Date.now() - START}ms ${msg}`;
  progress.push(line);
  if (TRACE_FILE) {
    // Synchronous: the point is that the line is on disk before whatever
    // happens next, including the process being killed.
    appendFileSync(TRACE_FILE, `${line}\n`);
  }
}

let nextWorkerId = 0;

function createWorker(durationMs: number): Promise<Profile[]> {
  return new Promise((resolve, reject) => {
    const profiles: Profile[] = [];
    const chain = nextWorkerId++;
    mark(`chain ${chain}: spawning first worker`);
    new Worker(__filename, {workerData: {durationMs}})
      .on('exit', exitCode => {
        mark(`chain ${chain}: first worker exited (${exitCode})`);
        if (exitCode !== 0) reject();
        setTimeout(
          () => {
            // Run a second worker after the first one exited to test for proper
            // cleanup after first worker. This used to segfault.
            mark(`chain ${chain}: spawning second worker`);
            new Worker(__filename, {workerData: {durationMs}})
              .on('exit', exitCode => {
                mark(`chain ${chain}: second worker exited (${exitCode})`);
                if (exitCode !== 0) reject();
                resolve(profiles);
              })
              .on('error', reject)
              .on('message', profile => {
                mark(`chain ${chain}: second worker sent a profile`);
                profiles.push(profile);
              });
          },
          Math.floor(Math.random() * durationMs),
        );
      })
      .on('error', reject)
      .on('message', profile => {
        mark(`chain ${chain}: first worker sent a profile`);
        profiles.push(profile);
      });
  });
}

async function executeWorkers(nbWorkers: number, durationMs: number) {
  const workers = [];
  for (let i = 0; i < nbWorkers; i++) {
    workers.push(createWorker(durationMs));
  }
  return Promise.all(workers).then(profiles => profiles.flat());
}

function getCpuUsage() {
  const cpu = process.cpuUsage();
  return cpu.user + cpu.system;
}

async function main(durationMs: number) {
  mark('main: starting profiler');
  time.start({
    durationMillis: durationMs * 3,
    intervalMicros,
    withContexts,
    collectCpuTime: withContexts,
    useCPED: useCPED,
    collectAsyncId: collectAsyncId,
  });
  if (withContexts) {
    time.setContext({});
  }

  const cpu0 = getCpuUsage();
  const nbWorkers = Number(process.argv[2] ?? 2);

  // start workers
  mark(`main: spawning ${nbWorkers} worker chains`);
  const workers = executeWorkers(nbWorkers, durationMs);

  const deadline = Date.now() + durationMs;
  // wait for all work to finish
  mark('main: awaiting first bar/foo round');
  await Promise.all([bar(deadline), foo(deadline)]);
  mark('main: first bar/foo round done, awaiting worker chains');
  const workerProfiles = await workers;
  mark('main: worker chains done');

  // restart and check profile
  const profile1 = time.stop(true);
  mark('main: stop(restart=true) returned');
  const cpu1 = getCpuUsage();

  workerProfiles.forEach(checkProfile);
  checkProfile(profile1);
  if (withContexts) {
    checkCpuTime(profile1, cpu1 - cpu0, workerProfiles);
  }
  const newDeadline = Date.now() + durationMs;
  mark('main: awaiting second bar/foo round');
  await Promise.all([bar(newDeadline), foo(newDeadline)]);
  mark('main: second bar/foo round done');

  const profile2 = time.stop();
  mark('main: stop() returned');
  const cpu2 = getCpuUsage();
  checkProfile(profile2);
  if (withContexts) {
    checkCpuTime(profile2, cpu2 - cpu1);
  }
  mark('main: done');
}

async function worker(durationMs: number) {
  time.start({
    durationMillis: durationMs,
    intervalMicros,
    withContexts,
    collectCpuTime: withContexts,
    useCPED: useCPED,
    collectAsyncId: collectAsyncId,
  });
  if (withContexts) {
    time.setContext({});
  }

  const deadline = Date.now() + durationMs;
  await Promise.all([bar(deadline), foo(deadline)]);

  const profile = time.stop();
  parentPort?.postMessage(profile);
}

if (isMainThread) {
  void main(DURATION_MILLIS);
} else {
  void worker(workerData.durationMs);
}

function valueName(profile: Profile, vt: ValueType) {
  const type = getAndVerifyString(profile.stringTable!, vt, 'type');
  const unit = getAndVerifyString(profile.stringTable!, vt, 'unit');
  return `${type}/${unit}`;
}

function sampleName(profile: Profile, sampleType: ValueType[]) {
  return sampleType.map(valueName.bind(null, profile));
}

function getCpuTime(profile: Profile) {
  let jsCpuTime = 0;
  let nonJsCpuTime = 0;
  if (!withContexts) return {jsCpuTime, nonJsCpuTime};
  for (const sample of profile.sample!) {
    const locationId = sample.locationId[0];
    const location = getAndVerifyPresence(
      profile.location!,
      locationId as number,
    );
    const functionId = location.line![0].functionId;
    const fn = getAndVerifyPresence(profile.function!, functionId as number);
    const fn_name = profile.stringTable.strings[fn.name as number];
    if (fn_name === time.constants.NON_JS_THREADS_FUNCTION_NAME) {
      nonJsCpuTime += sample.value![2] as number;
      assert.strictEqual(sample.value![0], 0);
      assert.strictEqual(sample.value![1], 0);
    } else {
      jsCpuTime += sample.value![2] as number;
    }
  }

  return {jsCpuTime, nonJsCpuTime};
}

function checkCpuTime(
  profile: Profile,
  processCpuTimeMicros: number,
  workerProfiles: Profile[] = [],
  maxRelativeError = 0.1,
) {
  let workersJsCpuTime = 0;
  let workersNonJsCpuTime = 0;

  for (const workerProfile of workerProfiles) {
    const {jsCpuTime, nonJsCpuTime} = getCpuTime(workerProfile);
    workersJsCpuTime += jsCpuTime;
    workersNonJsCpuTime += nonJsCpuTime;
  }

  const {jsCpuTime: mainJsCpuTime, nonJsCpuTime: mainNonJsCpuTime} =
    getCpuTime(profile);

  // workers should not report non-JS CPU time
  assert.strictEqual(
    workersNonJsCpuTime,
    0,
    'worker non-JS CPU time should be null',
  );

  const totalCpuTimeMicros =
    (mainJsCpuTime + mainNonJsCpuTime + workersJsCpuTime) / 1000;
  const err =
    Math.abs(totalCpuTimeMicros - processCpuTimeMicros) / processCpuTimeMicros;
  const msg = `process cpu time: ${
    processCpuTimeMicros / 1000
  }ms\ntotal profile cpu time: ${
    totalCpuTimeMicros / 1000
  }ms\nmain JS cpu time: ${mainJsCpuTime / 1000000}ms\nworker JS cpu time: ${
    workersJsCpuTime / 1000000
  }\nnon-JS cpu time: ${mainNonJsCpuTime / 1000000}ms\nerror: ${err}`;
  assert.ok(
    err <= maxRelativeError,
    `total profile CPU time should be close to process cpu time:\n${msg}`,
  );
}

function checkProfile(profile: Profile) {
  assert.deepStrictEqual(sampleName(profile, profile.sampleType!), [
    'sample/count',
    'wall/nanoseconds',
    ...(withContexts ? ['cpu/nanoseconds'] : []),
  ]);
  assert.strictEqual(typeof profile.timeNanos, 'number');
  assert.strictEqual(typeof profile.durationNanos, 'number');
  assert.strictEqual(typeof profile.period, 'number');
  assert.strictEqual(
    valueName(profile, profile.periodType!),
    'wall/nanoseconds',
  );

  assert.ok(profile.sample.length > 0, 'No samples');

  for (const sample of profile.sample!) {
    assert.deepStrictEqual(sample.label, []);

    for (const value of sample.value!) {
      assert.strictEqual(typeof value, 'number');
    }

    for (const locationId of sample.locationId!) {
      const location = getAndVerifyPresence(
        profile.location!,
        locationId as number,
      );

      for (const {functionId, line} of location.line!) {
        const fn = getAndVerifyPresence(
          profile.function!,
          functionId as number,
        );

        getAndVerifyString(profile.stringTable!, fn, 'name');
        getAndVerifyString(profile.stringTable!, fn, 'systemName');
        getAndVerifyString(profile.stringTable!, fn, 'filename');
        assert.strictEqual(typeof line, 'number');
      }
    }
  }
}

async function bar(deadline: number) {
  let done = false;
  setTimeout(() => {
    done = true;
  }, deadline - Date.now());
  while (!done) {
    await new Promise<void>(resolve => {
      pbkdf2('secret', 'salt', 100000, 64, 'sha512', () => {
        resolve();
      });
    });
  }
}

function fooWork() {
  let sum = 0;
  for (let i = 0; i < 1e7; i++) {
    sum += sum;
  }
  return sum;
}

async function foo(deadline: number) {
  let done = false;
  setTimeout(() => {
    done = true;
  }, deadline - Date.now());

  while (!done) {
    await new Promise<void>(resolve => {
      fooWork();
      setImmediate(() => resolve());
    });
  }
}
