/*
 * Copyright 2026 Datadog, Inc
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

'use strict';

// Runs in a forked process because the failure mode is an abort, which would
// take the whole mocha run down with it.
//
// Append's reallocate path used to assert that the copied record had
// `valid == 1`. invalidate() sets that byte to 0 and appending afterwards is
// supported, so an append too large to fit in place aborted the process:
//
//     Assertion `new_rec->valid == 1' failed.
//
// Only the reallocate path is affected — an append that fits the current
// capacity is written in place and never copies the header. A fresh record has
// 36 bytes of attrs_data capacity (64 - sizeof(header)), so the value below is
// comfortably past it.

import assert from 'assert';

import {otelThreadCtx} from '../src/index';

const VALUE = 'x'.repeat(200);

const ctx = new otelThreadCtx.ThreadContext(
  Buffer.alloc(16, 1),
  Buffer.alloc(8, 2),
);

ctx.run(() => {
  ctx.invalidate();
  ctx.appendAttributes([VALUE]);

  const bytes = ctx.debugBytes();
  const attrsDataSize = bytes[26] | (bytes[27] << 8);

  // invalidate() must stick: growing the record does not resurrect it.
  assert.strictEqual(bytes[24], 0, 'valid byte should still be 0');
  // key index (1) + length (1) + the value itself.
  assert.strictEqual(attrsDataSize, VALUE.length + 2, 'attrs_data_size');
});

console.log('ok');
