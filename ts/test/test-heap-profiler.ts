/**
 * Copyright 2017 Google Inc. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

import * as sinon from 'sinon';

import * as heapProfiler from '../src/heap-profiler';
import * as v8HeapProfiler from '../src/heap-profiler-bindings';
import {
  AllocationProfileNode,
  AllocationProfileNodeWithStats,
  LabelSet,
} from '../src/v8-types';
import {fork} from 'child_process';
import path from 'path';
import fs from 'fs';

import {
  heapProfileExcludePath,
  heapProfileIncludePath,
  heapProfileWithExternal,
  v8HeapProfile,
  v8HeapWithPathProfile,
  heapProfileIncludePathWithLabels,
} from './profiles-for-tests';

const copy = require('deep-copy');
const assert = require('assert');

function withAllocationStats(
  node: AllocationProfileNode,
): AllocationProfileNodeWithStats {
  return {
    ...node,
    allocations: node.allocations.map(alloc => ({
      inuseObjects: alloc.count,
      inuseSpaceBytes: alloc.sizeBytes * alloc.count,
      allocObjects: alloc.count,
      allocSpaceBytes: alloc.sizeBytes * alloc.count,
    })),
    children: node.children.map(withAllocationStats),
  };
}

const v8AllocationProfile: AllocationProfileNodeWithStats = {
  name: '(root)',
  scriptName: '(root)',
  scriptId: 0,
  lineNumber: 0,
  columnNumber: 0,
  allocations: [],
  children: [
    {
      name: 'allocatingFunction',
      scriptName: 'script1',
      scriptId: 1,
      lineNumber: 1,
      columnNumber: 1,
      allocations: [
        {
          inuseObjects: 4,
          inuseSpaceBytes: 400,
          allocObjects: 10,
          allocSpaceBytes: 1000,
        },
      ],
      children: [],
    },
  ],
};

describe('HeapProfiler', () => {
  let startStub: sinon.SinonStub<[number, number, boolean?], void>;
  let stopStub: sinon.SinonStub<[], void>;
  let profileStub: sinon.SinonStub<
    [],
    AllocationProfileNode | AllocationProfileNodeWithStats
  >;
  let dateStub: sinon.SinonStub<[], number>;
  let memoryUsageStub: sinon.SinonStub<[], NodeJS.MemoryUsage>;
  beforeEach(() => {
    startStub = sinon.stub(v8HeapProfiler, 'startSamplingHeapProfiler');
    stopStub = sinon.stub(v8HeapProfiler, 'stopSamplingHeapProfiler');
    dateStub = sinon.stub(Date, 'now').returns(0);
  });

  afterEach(() => {
    heapProfiler.stop();
    startStub.restore();
    stopStub.restore();
    profileStub?.restore();
    dateStub.restore();
    memoryUsageStub?.restore();
  });
  describe('profile', () => {
    it('should return a profile equal to the expected profile when external memory is allocated', async () => {
      profileStub = sinon
        .stub(v8HeapProfiler, 'getAllocationProfile')
        .returns(copy(v8HeapProfile));
      memoryUsageStub = sinon.stub(process, 'memoryUsage').returns({
        external: 1024,
        rss: 2048,
        heapTotal: 4096,
        heapUsed: 2048,
        arrayBuffers: 512,
      });
      const intervalBytes = 1024 * 512;
      const stackDepth = 32;
      heapProfiler.start(intervalBytes, stackDepth);
      const profile = heapProfiler.profile();
      assert.deepEqual(heapProfileWithExternal, profile);
    });

    it('should return a profile equal to the expected profile when including all samples', async () => {
      profileStub = sinon
        .stub(v8HeapProfiler, 'getAllocationProfile')
        .returns(copy(v8HeapWithPathProfile));
      memoryUsageStub = sinon.stub(process, 'memoryUsage').returns({
        external: 0,
        rss: 2048,
        heapTotal: 4096,
        heapUsed: 2048,
        arrayBuffers: 512,
      });
      const intervalBytes = 1024 * 512;
      const stackDepth = 32;
      heapProfiler.start(intervalBytes, stackDepth);
      const profile = heapProfiler.profile();
      assert.deepEqual(heapProfileIncludePath, profile);
    });

    it('should return a profile equal to the expected profile when excluding profiler samples', async () => {
      profileStub = sinon
        .stub(v8HeapProfiler, 'getAllocationProfile')
        .returns(copy(v8HeapWithPathProfile));
      memoryUsageStub = sinon.stub(process, 'memoryUsage').returns({
        external: 0,
        rss: 2048,
        heapTotal: 4096,
        heapUsed: 2048,
        arrayBuffers: 512,
      });
      const intervalBytes = 1024 * 512;
      const stackDepth = 32;
      heapProfiler.start(intervalBytes, stackDepth);
      const profile = heapProfiler.profile('@google-cloud/profiler');
      assert.deepEqual(heapProfileExcludePath, profile);
    });

    it('should return a profile equal to the expected profile when adding labels', async () => {
      profileStub = sinon
        .stub(v8HeapProfiler, 'getAllocationProfile')
        .returns(copy(v8HeapWithPathProfile));
      memoryUsageStub = sinon.stub(process, 'memoryUsage').returns({
        external: 0,
        rss: 2048,
        heapTotal: 4096,
        heapUsed: 2048,
        arrayBuffers: 512,
      });
      const intervalBytes = 1024 * 512;
      const stackDepth = 32;
      heapProfiler.start(intervalBytes, stackDepth);
      const labels: LabelSet = {baz: 'bar'};
      const profile = heapProfiler.profile(undefined, undefined, () => {
        return labels;
      });
      assert.deepEqual(heapProfileIncludePathWithLabels, profile);
    });

    it('should use allocation profile mode when allocations is passed', async () => {
      profileStub = sinon
        .stub(v8HeapProfiler, 'getAllocationProfile')
        .returns(copy(v8AllocationProfile));
      memoryUsageStub = sinon.stub(process, 'memoryUsage').returns({
        external: 0,
        rss: 2048,
        heapTotal: 4096,
        heapUsed: 2048,
        arrayBuffers: 512,
      });
      const intervalBytes = 1024 * 512;
      const stackDepth = 32;
      heapProfiler.start(intervalBytes, stackDepth, true);
      const profile = heapProfiler.profile();
      const sampleTypeNames = profile.sampleType.map(
        sampleType => profile.stringTable.strings[Number(sampleType.type)],
      );
      assert.deepEqual(sampleTypeNames, [
        'inuse_objects',
        'alloc_objects',
        'inuse_space',
        'alloc_space',
      ]);
      assert.deepEqual(profile.sample[0].value, [4, 10, 400, 1000]);
      assert.equal(profileStub.calledOnce, true);
      assert.equal(profileStub.firstCall.args.length, 0);
    });

    it('should preserve allocation stats for external memory in allocation mode', async () => {
      profileStub = sinon
        .stub(v8HeapProfiler, 'getAllocationProfile')
        .returns(withAllocationStats(copy(v8HeapProfile)));
      memoryUsageStub = sinon.stub(process, 'memoryUsage').returns({
        external: 1024,
        rss: 2048,
        heapTotal: 4096,
        heapUsed: 2048,
        arrayBuffers: 512,
      });
      const intervalBytes = 1024 * 512;
      const stackDepth = 32;
      heapProfiler.start(intervalBytes, stackDepth, true);
      const profile = heapProfiler.profile();
      assert.ok(profile.sample.some(sample => sample.value[2] === 1024));
    });

    it('should throw when profileV2 is requested from allocation mode', async () => {
      const intervalBytes = 1024 * 512;
      const stackDepth = 32;
      heapProfiler.start(intervalBytes, stackDepth, true);
      assert.throws(
        () => {
          heapProfiler.profileV2();
        },
        (err: Error) => {
          return err.message === 'profileV2 does not support allocation mode.';
        },
      );
    });

    it('should throw error when not started', () => {
      assert.throws(
        () => {
          heapProfiler.profile();
        },
        (err: Error) => {
          return err.message === 'Heap profiler is not enabled.';
        },
      );
    });

    it('should throw error when started then stopped', () => {
      const intervalBytes = 1024 * 512;
      const stackDepth = 32;
      heapProfiler.start(intervalBytes, stackDepth);
      heapProfiler.stop();
      assert.throws(
        () => {
          heapProfiler.profile();
        },
        (err: Error) => {
          return err.message === 'Heap profiler is not enabled.';
        },
      );
    });
  });

  describe('start', () => {
    it('should call startSamplingHeapProfiler', () => {
      const intervalBytes1 = 1024 * 512;
      const stackDepth1 = 32;
      heapProfiler.start(intervalBytes1, stackDepth1);
      assert.ok(
        startStub.calledWith(intervalBytes1, stackDepth1, false),
        'expected startSamplingHeapProfiler to be called',
      );
    });
    it('should pass allocations to startSamplingHeapProfiler', () => {
      const intervalBytes1 = 1024 * 512;
      const stackDepth1 = 32;
      heapProfiler.start(intervalBytes1, stackDepth1, true);
      assert.ok(
        startStub.calledWith(intervalBytes1, stackDepth1, true),
        'expected startSamplingHeapProfiler to be called with allocations',
      );
    });
    it('should throw error when enabled and started with different parameters', () => {
      const intervalBytes1 = 1024 * 512;
      const stackDepth1 = 32;
      heapProfiler.start(intervalBytes1, stackDepth1);
      assert.ok(
        startStub.calledWith(intervalBytes1, stackDepth1, false),
        'expected startSamplingHeapProfiler to be called',
      );
      startStub.resetHistory();
      const intervalBytes2 = 1024 * 128;
      const stackDepth2 = 64;
      try {
        heapProfiler.start(intervalBytes2, stackDepth2);
      } catch (e) {
        assert.strictEqual(
          (e as Error).message,
          'Heap profiler is already started  with intervalBytes 524288 and' +
            ' stackDepth 64',
        );
      }
      assert.ok(
        !startStub.called,
        'expected startSamplingHeapProfiler not to be called second time',
      );
    });
  });

  describe('stop', () => {
    it('should not call stopSamplingHeapProfiler if profiler not started', () => {
      heapProfiler.stop();
      assert.ok(!stopStub.called, 'stop() should have been no-op.');
    });
    it('should call stopSamplingHeapProfiler if profiler started', () => {
      heapProfiler.start(1024 * 512, 32);
      heapProfiler.stop();
      assert.ok(
        stopStub.called,
        'expected stopSamplingHeapProfiler to be called',
      );
    });
  });
});

describe('foreign heap sampler', () => {
  // Regression test: V8's sampling heap profiler can be enabled by something
  // other than pprof (inspector, DevTools, another agent). getAllocationProfile
  // and mapAllocationProfile then see a live V8 profile with no per-isolate
  // state, and used to dereference an empty shared_ptr. Forked because the
  // failure is a SIGSEGV.
  it('should not crash when V8 heap sampling was enabled outside of pprof', async function () {
    this.timeout(30000);

    const proc = fork(path.join(__dirname, 'heap-foreign-sampler.js'), {
      silent: true,
      // Under the asan CI job the child inherits LD_PRELOAD=libasan and runs
      // LeakSanitizer at exit. The child ends on process.exit(), so V8's heap
      // is never torn down and every live object is reported as leaked,
      // failing the child for reasons that have nothing to do with what this
      // test checks. ASAN itself stays on, so a genuine memory error in the
      // code under test is still caught.
      env: {...process.env, LSAN_OPTIONS: 'detect_leaks=0'},
    });
    let output = '';
    proc.stdout?.on('data', chunk => {
      output += chunk;
    });
    proc.stderr?.on('data', chunk => {
      output += chunk;
    });

    await new Promise<void>((resolve, reject) => {
      proc.on('error', reject);
      // 'close' rather than 'exit': it fires once the piped stdio has been
      // drained, so `output` is complete when it lands in the failure message.
      proc.on('close', (code, signal) => {
        if (code === 0) {
          resolve();
        } else {
          reject(
            new Error(
              `heap-foreign-sampler exited with code=${code} signal=${signal}\n${output}`,
            ),
          );
        }
      });
    });
  });
});

describe('OOMMonitoring', () => {
  async function runOomFixture(script: string, heapLimitExtensionSize: string) {
    const proc = fork(path.join(__dirname, script), [heapLimitExtensionSize], {
      execArgv: ['--expose-gc', '--max-old-space-size=64'],
      silent: true,
    });
    let output = '';

    proc.stdout?.on('data', chunk => {
      output += chunk;
    });
    proc.stderr?.on('data', chunk => {
      output += chunk;
    });

    return new Promise<{code: number | null; output: string}>(
      (resolve, reject) => {
        proc.on('error', reject);
        proc.on('exit', code => {
          resolve({code, output});
        });
      },
    );
  }

  async function assertHeapLimitIsRestored(heapLimitExtensionSize: string) {
    const {code, output} = await runOomFixture(
      'oom-restore-heap-limit.js',
      heapLimitExtensionSize,
    );
    assert.strictEqual(
      code,
      0,
      `oom-restore-heap-limit exited with ${code}\n${output}`,
    );
  }

  it('should restore an automatic heap limit extension', async function () {
    this.timeout(30000);
    await assertHeapLimitIsRestored('auto');
  });

  it('should restore a configured heap limit extension', async function () {
    this.timeout(30000);
    await assertHeapLimitIsRestored(String(64 * 1024 * 1024));
  });

  async function grantedHeapLimitExtension(heapLimitExtensionSize: string) {
    const {output} = await runOomFixture(
      'oom-heap-limit-extension.js',
      heapLimitExtensionSize,
    );
    const limits = [...output.matchAll(/^limit (\d+)$/gm)].map(match =>
      Number(match[1]),
    );
    // The callback logs the old generation limit v8 handed it. v8 reports
    // heap_size_limit as that limit plus one maximum young generation, so the
    // young generation is the difference - derived from the run itself rather
    // than assumed from --max-old-space-size.
    const oldGeneration = Number(output.match(/current_heap_limit=(\d+)/)?.[1]);
    assert.ok(
      Number.isFinite(oldGeneration),
      `the near heap limit callback never ran\n${output}`,
    );
    assert.ok(limits.length > 0, `no heap limit was reported\n${output}`);
    return {
      granted: Math.max(...limits) - limits[0],
      youngGeneration: limits[0] - oldGeneration,
      output,
    };
  }

  it('should grant exactly one young generation when set to auto', async function () {
    this.timeout(30000);
    const {granted, youngGeneration, output} =
      await grantedHeapLimitExtension('auto');
    assert.strictEqual(
      granted,
      youngGeneration,
      `expected one young generation of headroom\n${output}`,
    );
  });

  // A size of 0 must keep meaning "grant no top-level extension" so that
  // upgrading does not silently start extending the heap of callers already
  // passing 0. The reentrant rescue grant that lets an in-progress capture
  // finish may still raise the limit, so this asserts the amount is not a
  // young generation rather than that nothing moved - regressing to
  // "0 means auto" grants exactly one young generation.
  it('should not grant a top-level extension when the size is 0', async function () {
    this.timeout(30000);
    const {granted, youngGeneration, output} =
      await grantedHeapLimitExtension('0');
    assert.notStrictEqual(
      granted,
      youngGeneration,
      `expected no young-generation extension, got ${granted}\n${output}`,
    );
  });

  it('should call external process upon OOM', async function () {
    // this test is very slow on some configs (asan/valgrind)
    this.timeout(20000);
    const proc = fork(path.join(__dirname, 'oom.js'), {
      execArgv: ['--max-old-space-size=50'],
    });
    const checkFilePath = 'oom_check.log';
    if (fs.existsSync(checkFilePath)) {
      fs.unlinkSync(checkFilePath);
    }
    // wait for proc to exit
    await new Promise<void>((resolve, reject) => {
      proc.on('exit', code => {
        if (code === 0) {
          reject();
        } else {
          resolve();
        }
      });
    });
    assert.equal(fs.readFileSync(checkFilePath), 'ok');
    fs.unlinkSync(checkFilePath);
  });
});
