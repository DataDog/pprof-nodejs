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

import {Profile} from 'pprof-format';

// Small on purpose: multi-megabyte chunks make the heap all-live, so the
// capture's forced GC frees nothing and v8 aborts on ineffective mark-compacts.
const CHUNK_BYTES = 256 * 1024;
export const SAMPLE_INTERVAL_BYTES = 64 * 1024;

function expect(ok: boolean, message: string) {
  if (!ok) {
    fs.writeSync(1, `FAIL ${message}\n`);
    process.exit(1);
  }
}

function done(marker: string): never {
  fs.writeSync(1, `${marker}\n`);
  process.exit(0);
}

function expectTypes(profile: Profile, expected: string) {
  const actual = profile.sampleType
    .map(sampleType => profile.stringTable.strings[Number(sampleType.type)])
    .join();
  expect(actual === expected, `sample types were ${actual}`);
}

function values(profile: Profile): number[][] {
  return profile.sample.map(sample => sample.value.map(Number));
}

export function checkInuseProfile(profile: Profile) {
  expectTypes(profile, 'inuse_objects,inuse_space');
  // The leak samples many objects per call site; "(external)" is always one.
  expect(
    values(profile).some(([inuseObjects]) => inuseObjects > 1),
    'no sample accounted for the leak',
  );
  done('inuseProfileChecked');
}

const retained: number[][] = [];

function allocateChunk(): number[] {
  const chunk = new Array<number>(CHUNK_BYTES / 8);
  for (let i = 0; i < chunk.length; i++) {
    chunk[i] = i + 0.1;
  }
  return chunk;
}

export function leak() {
  // Give one sampled call site both live and collected allocations.
  retained.push(allocateChunk());
  allocateChunk();
}
