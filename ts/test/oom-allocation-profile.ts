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

import fs from 'fs';

import {heap} from '../src/index';

const MB = 1024 * 1024;

function report(message: string) {
  fs.writeSync(1, `${message}\n`);
}

function fail(message: string): never {
  report(`FAIL ${message}`);
  process.exit(1);
}

heap.start(256 * 1024, 64, true);
heap.monitorOutOfMemory(
  'auto',
  1,
  false,
  undefined,
  profile => {
    const sampleTypes = profile.sampleType.map(
      sampleType => profile.stringTable.strings[Number(sampleType.type)],
    );
    const expectedTypes = [
      'inuse_objects',
      'alloc_objects',
      'inuse_space',
      'alloc_space',
    ];
    if (sampleTypes.join() !== expectedTypes.join()) {
      fail(`sample types were ${sampleTypes.join()}`);
    }

    if (profile.sample.length === 0) {
      fail('profile had no samples');
    }
    if (profile.sample.some(sample => sample.value.length !== 4)) {
      fail('a sample did not contain four values');
    }
    if (
      !profile.sample.some(
        sample => Number(sample.value[1]) > Number(sample.value[0]),
      )
    ) {
      fail('profile did not retain collected allocations');
    }
    if (
      profile.sample.some(
        sample =>
          Number(sample.value[1]) < Number(sample.value[0]) ||
          Number(sample.value[3]) < Number(sample.value[2]),
      )
    ) {
      fail('allocated values were smaller than in-use values');
    }

    report('allocationProfileChecked');
    process.exit(0);
  },
  heap.CallbackMode.Async,
);

const retained: number[][] = [];

function allocateChunk(): number[] {
  const chunk = new Array<number>((4 * MB) / 8);
  for (let i = 0; i < chunk.length; i++) {
    chunk[i] = i + 0.1;
  }
  return chunk;
}

function leak() {
  // Give one sampled call site both live and collected allocations.
  retained.push(allocateChunk());
  allocateChunk();
  setTimeout(leak, 5);
}

leak();
