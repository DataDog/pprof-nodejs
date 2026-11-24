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

import {
  CapturedProfile,
  Frame,
  HeapProfileNormalizerOptions,
  NormalizedSample,
  ProfileMetadata,
  TimeProfileNormalizerOptions,
  VisitResult,
  visitHeapProfile,
  visitTimeProfile,
} from '../src/profile-normalizer';
import {
  AllocationProfileNode,
  AllocationProfileNodeWithStats,
  TimeProfile,
} from '../src/v8-types';

/** A visit of a profile, collected into arrays, for tests to assert on. */
export interface NormalizedProfile extends ProfileMetadata, VisitResult {
  frames: Frame[];
  /** Stacks, leaf first, as indices into {@link frames}. */
  stacks: number[][];
  /** Samples' stack IDs are indices into {@link stacks}. */
  samples: NormalizedSample[];
}

/** Visits a profile, and collects the visit. */
export function collect(profile: CapturedProfile): NormalizedProfile {
  let metadata: ProfileMetadata | undefined;
  const frames: Frame[] = [];
  const stacks: number[][] = [];
  const samples: NormalizedSample[] = [];
  const {hasMissingMapFiles} = profile.visit({
    metadata: m => {
      metadata = m;
    },
    frame: f => frames.push(f) - 1,
    stack: s => stacks.push(s) - 1,
    sample: s => {
      samples.push(s);
    },
  });
  return {...metadata!, frames, stacks, samples, hasMissingMapFiles};
}

export function normalizeTimeProfile(
  prof: TimeProfile,
  options: TimeProfileNormalizerOptions,
): NormalizedProfile {
  return collect({visit: visitor => visitTimeProfile(prof, visitor, options)});
}

export function normalizeHeapProfile(
  prof: AllocationProfileNode | AllocationProfileNodeWithStats,
  options: HeapProfileNormalizerOptions,
): NormalizedProfile {
  return collect({visit: visitor => visitHeapProfile(prof, visitor, options)});
}
