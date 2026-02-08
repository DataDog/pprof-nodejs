import {strict as assert} from 'assert';
import * as heapProfiler from '../src/heap-profiler';

function generateAllocations(): object[] {
  const allocations: object[] = [];
  for (let i = 0; i < 1000; i++) {
    allocations.push({data: new Array(100).fill(i)});
  }
  return allocations;
}

describe('AllocationProfileNodeWrapper', () => {
  let keepAlive: object[] = [];

  before(() => {
    heapProfiler.start(512, 64);
    keepAlive = generateAllocations();
  });

  after(() => {
    heapProfiler.stop();
    keepAlive.length = 0;
  });

  it('exposes lazy accessors and valid fields', () => {
    const root = heapProfiler.v8ProfileV2();
    try {
      // Check methods exist
      assert.equal(typeof root.getChildrenCount, 'function');
      assert.equal(typeof root.getChild, 'function');
      assert.equal(typeof root.dispose, 'function');

      // Check properties
      assert.equal(typeof root.name, 'string');
      assert.equal(typeof root.scriptName, 'string');
      assert.equal(typeof root.scriptId, 'number');
      assert.equal(typeof root.lineNumber, 'number');
      assert.equal(typeof root.columnNumber, 'number');
      assert.ok(Array.isArray(root.allocations));

      // Traverse children lazily
      const childCount = root.getChildrenCount();
      assert.equal(typeof childCount, 'number');
      assert.ok(childCount >= 0);

      if (childCount > 0) {
        const child = root.getChild(0);
        assert.equal(typeof child.name, 'string');
        assert.equal(typeof child.scriptName, 'string');
        assert.equal(typeof child.scriptId, 'number');
        assert.ok(Array.isArray(child.allocations));

        for (const alloc of child.allocations) {
          assert.equal(typeof alloc.count, 'number');
          assert.equal(typeof alloc.sizeBytes, 'number');
        }

        const grandchildCount = child.getChildrenCount();
        assert.equal(typeof grandchildCount, 'number');
      }
    } finally {
      root.dispose();
    }
  });

  it('profileV2 produces valid pprof output', () => {
    const profile = heapProfiler.profileV2();

    // Verify profile structure
    assert.ok(profile.sampleType);
    assert.ok(profile.sample);
    assert.ok(profile.location);
    assert.ok(profile.function);
    assert.ok(profile.stringTable);
  });
});
