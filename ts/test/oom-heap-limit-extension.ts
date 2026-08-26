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

import * as v8 from 'v8';

import {heap, HeapLimitExtensionSize} from '../src/index';

const MB = 1024 * 1024;
const CHUNK_SIZE = 4 * MB;
const MAX_CHUNKS = 64;
const heapLimitExtensionSize: HeapLimitExtensionSize =
  process.argv[2] === 'auto' ? 'auto' : Number(process.argv[2] || 0);

function heapLimit() {
  return v8.getHeapStatistics().heap_size_limit;
}

heap.start(MB, 64);
heap.monitorOutOfMemory(heapLimitExtensionSize, 1, false);

const initialLimit = heapLimit();
const retained: number[][] = [];
let chunks = 0;

// Report every heap limit the process observes so the parent can tell whether
// a top-level extension was granted even if v8 aborts us mid-leak. A near-heap
// limit event is not necessarily fatal - v8 may free enough and carry on - so
// the limit is the only reliable signal here, not survival.
console.log(`limit ${initialLimit}`);

function leak() {
  const limit = heapLimit();
  if (limit !== initialLimit) {
    console.log(`limit ${limit}`);
    process.exit(0);
  }
  if (chunks >= MAX_CHUNKS) {
    process.exit(0);
  }

  const chunk = new Array<number>(CHUNK_SIZE / 8);
  for (let i = 0; i < chunk.length; i++) {
    chunk[i] = i + 0.1;
  }
  retained.push(chunk);
  chunks++;
  setTimeout(leak, 5);
}

leak();
