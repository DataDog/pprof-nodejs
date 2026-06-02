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
      count: alloc.count,
      sizeBytes: alloc.sizeBytes * alloc.count,
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
          count: 4,
          sizeBytes: 400,
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

describe('OOMMonitoring', () => {
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
