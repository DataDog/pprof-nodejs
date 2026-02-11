import * as heapProfiler from '../src/heap-profiler';
import * as v8HeapProfiler from '../src/heap-profiler-bindings';
import {AllocationProfileNode} from '../src/v8-types';

const ALLOCATION_COUNT = 100_000;
const ALLOCATION_SIZE = 200;

const gc = (global as NodeJS.Global & {gc?: () => void}).gc;
if (!gc) {
  throw new Error('Run with --expose-gc flag');
}

let keepAlive: object[] = [];

function generateAllocations(): object[] {
  const result: object[] = [];
  for (let i = 0; i < ALLOCATION_COUNT; i++) {
    result.push({
      id: i,
      data: new Array(ALLOCATION_SIZE).fill('Hello, world!'),
      nested: {map: new Map([[i, new Array(ALLOCATION_SIZE).fill(i)]])},
    });
  }
  return result;
}

function traverseTree(root: AllocationProfileNode): void {
  const stack: AllocationProfileNode[] = [root];
  while (stack.length > 0) {
    const node = stack.pop()!;
    if (node.children) {
      for (const child of node.children) {
        stack.push(child);
      }
    }
  }
}

function measureMemoryUsage(getProfile: () => AllocationProfileNode): {
  initial: number;
  afterTraversal: number;
} {
  gc!();
  gc!();
  const baseline = process.memoryUsage().heapUsed;

  const profile = getProfile();
  const initial = process.memoryUsage().heapUsed - baseline;

  traverseTree(profile);

  return {
    initial,
    afterTraversal: process.memoryUsage().heapUsed - baseline,
  };
}

process.on('message', (version: 'v1' | 'v2') => {
  heapProfiler.start(128, 128);
  keepAlive = generateAllocations();

  const getProfile =
    version === 'v1'
      ? v8HeapProfiler.getAllocationProfile
      : v8HeapProfiler.getAllocationProfileV2;

  const {initial, afterTraversal} = measureMemoryUsage(getProfile);

  heapProfiler.stop();
  keepAlive.length = 0;

  process.send!({initial, afterTraversal});
});
