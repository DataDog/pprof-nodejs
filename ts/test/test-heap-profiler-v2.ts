import {strict as assert} from 'assert';
import * as heapProfiler from '../src/heap-profiler';
import * as v8HeapProfiler from '../src/heap-profiler-bindings';

function generateAllocations(): object[] {
  const allocations: object[] = [];
  for (let i = 0; i < 1000; i++) {
    allocations.push({data: new Array(100).fill(i)});
  }
  return allocations;
}

describe('HeapProfiler V2 API', () => {
  let keepAlive: object[] = [];

  before(() => {
    heapProfiler.start(512, 64);
    keepAlive = generateAllocations();
  });

  after(() => {
    heapProfiler.stop();
    keepAlive.length = 0;
  });

  describe('v8ProfileV2', () => {
    it('should return AllocationProfileNode', () => {
      const root = heapProfiler.v8ProfileV2();

      assert.equal(typeof root.name, 'string');
      assert.equal(typeof root.scriptName, 'string');
      assert.equal(typeof root.scriptId, 'number');
      assert.equal(typeof root.lineNumber, 'number');
      assert.equal(typeof root.columnNumber, 'number');
      assert.ok(Array.isArray(root.allocations));

      assert.ok(Array.isArray(root.children));
      assert.equal(typeof root.children.length, 'number');

      if (root.children.length > 0) {
        const child = root.children[0];
        assert.equal(typeof child.name, 'string');
        assert.ok(Array.isArray(child.children));
        assert.ok(Array.isArray(child.allocations));
      }
    });

    it('should throw error when profiler not started', () => {
      heapProfiler.stop();
      assert.throws(
        () => {
          heapProfiler.v8ProfileV2();
        },
        (err: Error) => {
          return err.message === 'Heap profiler is not enabled.';
        }
      );
      heapProfiler.start(512, 64);
    });
  });

  describe('getAllocationProfileV2', () => {
    it('should return AllocationProfileNode directly', () => {
      const root = v8HeapProfiler.getAllocationProfileV2();

      assert.equal(typeof root.name, 'string');
      assert.equal(typeof root.scriptName, 'string');
      assert.ok(Array.isArray(root.children));
      assert.ok(Array.isArray(root.allocations));
    });
  });

  describe('profileV2', () => {
    it('should produce valid pprof Profile', () => {
      const profile = heapProfiler.profileV2();

      assert.ok(profile.sampleType);
      assert.ok(profile.sample);
      assert.ok(profile.location);
      assert.ok(profile.function);
      assert.ok(profile.stringTable);
    });
  });
});
