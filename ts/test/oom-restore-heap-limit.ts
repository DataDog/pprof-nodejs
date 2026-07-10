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

import {heap} from '../src/index';

const MB = 1024 * 1024;
const LIMIT_TOLERANCE = 16 * MB;
const CHUNK_SIZE = 4 * MB;
const gc = (global as typeof globalThis & {gc?: () => void}).gc;

function heapLimit() {
  return v8.getHeapStatistics().heap_size_limit;
}

function allocateChunk() {
  const chunk = new Array<number>(CHUNK_SIZE / 8);
  for (let i = 0; i < chunk.length; i++) {
    chunk[i] = i + 0.1;
  }
  return chunk;
}

async function main() {
  if (!gc) {
    throw new Error('This test must be run with --expose-gc.');
  }

  heap.start(MB, 64);
  try {
    heap.monitorOutOfMemory(64 * MB, 1, false);

    const initialLimit = heapLimit();
    const retained: number[][] = [];

    while (
      heapLimit() <= initialLimit + LIMIT_TOLERANCE &&
      retained.length < 64
    ) {
      retained.push(allocateChunk());
    }

    const increasedLimit = heapLimit();
    if (increasedLimit <= initialLimit + LIMIT_TOLERANCE) {
      throw new Error(`Heap limit did not increase from ${initialLimit}.`);
    }

    retained.length = 0;
    for (let i = 0; i < 100; i++) {
      gc();
      if (heapLimit() <= initialLimit + LIMIT_TOLERANCE) {
        return;
      }
      await new Promise(resolve => setTimeout(resolve, 20));
    }

    throw new Error(
      `Heap limit increased to ${increasedLimit} but did not restore to ` +
        `${initialLimit}.`,
    );
  } finally {
    heap.stop();
  }
}

main().catch(err => {
  console.error(err.stack || err.message);
  process.exitCode = 1;
});
