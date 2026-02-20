/**
 * Copyright 2025 Datadog, Inc
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * Tests for GetValueFromMap through the TimeProfiler API.
 *
 * These tests verify that GetValueFromMap works correctly in its actual usage context:
 * - The profiler creates a key object (AsyncLocalStorage or internal key)
 * - Contexts are stored in the CPED map using that key
 * - GetValueFromMap retrieves contexts using the same key during signal handling
 *
 * This is the real-world usage pattern, and these tests confirm the structure
 * layout and key address extraction work correctly on Node 24.x / V8 13.6.
 */

import assert from 'assert';
import {join} from 'path';
import {AsyncLocalStorage} from 'async_hooks';
import {satisfies} from 'semver';

const findBinding = require('node-gyp-build');
const profiler = findBinding(join(__dirname, '..', '..'));

const useCPED =
  (satisfies(process.versions.node, '>=24.0.0') &&
    !process.execArgv.includes('--no-async-context-frame')) ||
  (satisfies(process.versions.node, '>=22.7.0') &&
    process.execArgv.includes('--experimental-async-context-frame'));

const supportedPlatform =
  process.platform === 'darwin' || process.platform === 'linux';

if (useCPED && supportedPlatform) {
  describe('GetValueFromMap (through TimeProfiler)', () => {
    describe('basic context storage and retrieval', () => {
      it('should store and retrieve a simple object context', () => {
        const als = new AsyncLocalStorage();
        const profiler = createProfiler(als);

        als.enterWith([]);

        const context = {label: 'test-context'};
        profiler.context = context;

        const retrieved = profiler.context;
        assert.strictEqual(
          retrieved,
          context,
          'Should retrieve the same object'
        );

        profiler.dispose();
      });

      it('should store and retrieve context with multiple properties', () => {
        const als = new AsyncLocalStorage();
        const profiler = createProfiler(als);

        als.enterWith([]);

        const context = {
          spanId: '1234567890',
          traceId: 'abcdef123456',
          operation: 'test-operation',
          resource: '/api/endpoint',
          tags: {environment: 'test', version: '1.0'},
        };

        profiler.context = context;
        const retrieved = profiler.context;

        assert.deepStrictEqual(retrieved, context);
        assert.strictEqual(retrieved.spanId, context.spanId);
        assert.strictEqual(retrieved.traceId, context.traceId);
        assert.deepStrictEqual(retrieved.tags, context.tags);

        profiler.dispose();
      });

      it('should handle context updates', () => {
        const als = new AsyncLocalStorage();
        const profiler = createProfiler(als);

        als.enterWith([]);

        const context1 = {label: 'first'};
        profiler.context = context1;
        assert.strictEqual(profiler.context, context1);

        const context2 = {label: 'second'};
        profiler.context = context2;
        assert.strictEqual(profiler.context, context2);

        const context3 = {label: 'third', extra: 'data'};
        profiler.context = context3;
        assert.strictEqual(profiler.context, context3);

        profiler.dispose();
      });

      it('should return undefined for undefined context', () => {
        const als = new AsyncLocalStorage();
        const profiler = createProfiler(als);

        als.enterWith([]);

        profiler.context = undefined;
        const retrieved = profiler.context;

        assert.strictEqual(retrieved, undefined);

        profiler.dispose();
      });
    });

    describe('multiple context frames', () => {
      it('should isolate contexts in different async frames', () => {
        const als = new AsyncLocalStorage();
        const profiler = createProfiler(als);

        const context1 = {frame: 'frame1'};
        const context2 = {frame: 'frame2'};

        // Frame 1
        als.run([], () => {
          profiler.context = context1;
          assert.deepStrictEqual(profiler.context, context1);
        });

        // Frame 2
        als.run([], () => {
          assert.strictEqual(profiler.context, undefined);
          profiler.context = context2;
          assert.deepStrictEqual(profiler.context, context2);
        });

        // Outside frames
        assert.strictEqual(profiler.context, undefined);

        profiler.dispose();
      });

      it('should handle nested async frames', () => {
        const als = new AsyncLocalStorage();
        const profiler = createProfiler(als);

        const outerContext = {level: 'outer'};
        const innerContext = {level: 'inner'};

        als.run([], () => {
          profiler.context = outerContext;
          assert.deepStrictEqual(profiler.context, outerContext);

          als.run([], () => {
            profiler.context = innerContext;
            assert.deepStrictEqual(profiler.context, innerContext);
          });

          // Back to outer context frame
          assert.deepStrictEqual(profiler.context, outerContext);
        });

        profiler.dispose();
      });
    });
  });
}

function createProfiler(als: AsyncLocalStorage<any>) {
  return new profiler.TimeProfiler({
    intervalMicros: 10000,
    durationMillis: 500,
    withContexts: true,
    useCPED: true,
    CPEDKey: als,
    lineNumbers: false,
    workaroundV8Bug: false,
    collectCpuTime: false,
    collectAsyncId: true,
    isMainThread: true,
  });
}
