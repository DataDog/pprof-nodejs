/**
 * Copyright 2017 Google Inc. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

import delay from 'delay';

import { serializeTimeProfile } from './profile-serializer';
import { SourceMapper } from './sourcemapper/sourcemapper';
import { TimeProfiler } from './time-profiler-bindings';
import { LabelSet, TimeProfile } from './v8-types';

const DEFAULT_INTERVAL_MICROS: Microseconds = 1000;
const DEFAULT_DURATION_MICROS: Microseconds = 60000000;

type Microseconds = number;
type Milliseconds = number;

let gProfiler: InstanceType<typeof TimeProfiler> | undefined;
let gSourceMapper: SourceMapper | undefined;
let gIntervalMicros: Microseconds;

export interface TimeProfilerOptions {
  /** time in milliseconds for which to collect profile. */
  durationMillis: Milliseconds;
  /** average time in microseconds between samples */
  intervalMicros?: Microseconds;
  sourceMapper?: SourceMapper;

  /**
   * This configuration option is experimental.
   * When set to true, functions will be aggregated at the line level, rather
   * than at the function level.
   * This defaults to false.
   */
  lineNumbers?: boolean;
  customLabels?: boolean;
}

export async function profile(options: TimeProfilerOptions) {
  start(
    options.intervalMicros || DEFAULT_INTERVAL_MICROS,
    options.durationMillis * 1000,
    options.sourceMapper,
    options.lineNumbers,
    options.customLabels
  );
  await delay(options.durationMillis);
  return stop();
}

// Temporarily retained for backwards compatibility with older tracer
export function start(
  intervalMicros: Microseconds = DEFAULT_INTERVAL_MICROS,
  durationMicros: Microseconds = DEFAULT_DURATION_MICROS,
  sourceMapper?: SourceMapper,
  lineNumbers = true,
  customLabels = false
) {
  if (gProfiler) {
    throw new Error(
      'Wall profiler is already started'
    );
  }

  gProfiler = new TimeProfiler(intervalMicros, durationMicros)
  gProfiler.start(lineNumbers, customLabels)
  gSourceMapper = sourceMapper;
  gIntervalMicros = intervalMicros;
}

export function stop(restart = false) {
  if (!gProfiler) {
    throw new Error(
      'Wall profiler is not started');
  }

  const profile = gProfiler.stop(restart)
  const serialized_profile = serializeTimeProfile(profile, gIntervalMicros, gSourceMapper, true);
  if (!restart) {
    gProfiler = undefined;
    gSourceMapper = undefined;
  }
  return serialized_profile
}

export function setLabels(labels?: LabelSet) {
  if (!gProfiler) {
    throw new Error(
      'Wall profiler is not started');
  }
  gProfiler.labels = labels;
}
