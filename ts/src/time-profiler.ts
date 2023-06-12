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

import {serializeTimeProfile} from './profile-serializer';
import {SourceMapper} from './sourcemapper/sourcemapper';
import {TimeProfiler} from './time-profiler-bindings';
import {LabelSet} from './v8-types';

const DEFAULT_INTERVAL_MICROS: Microseconds = 1000;
const DEFAULT_DURATION_MICROS: Microseconds = 60000000;

const majorVersion = process.version.slice(1).split('.').map(Number)[0];

type Microseconds = number;
type Milliseconds = number;

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
}

export async function profile(options: TimeProfilerOptions) {
  const stop = start(
    options.intervalMicros || DEFAULT_INTERVAL_MICROS,
    options.sourceMapper,
    options.lineNumbers
  );
  await delay(options.durationMillis);
  return stop();
}

// Temporarily retained for backwards compatibility with older tracer
export function start(
  intervalMicros: Microseconds = DEFAULT_INTERVAL_MICROS,
  sourceMapper?: SourceMapper,
  lineNumbers = true
) {
  const {stop} = startInternal(
    intervalMicros,
    // Duration must be at least intervalMicros; not used anyway when
    // not collecting extra info (CPU time, labels) with samples.
    intervalMicros,
    sourceMapper,
    lineNumbers,
    false
  );
  return stop;
}

export function startWithLabels(
  intervalMicros: Microseconds = DEFAULT_INTERVAL_MICROS,
  durationMicros: Microseconds = DEFAULT_DURATION_MICROS,
  sourceMapper?: SourceMapper,
  lineNumbers = true
) {
  return startInternal(
    intervalMicros,
    durationMicros,
    sourceMapper,
    lineNumbers,
    true
  );
}

// NOTE: refreshing doesn't work if giving a profile name.
function startInternal(
  intervalMicros: Microseconds,
  durationMicros: Microseconds,
  sourceMapper?: SourceMapper,
  lineNumbers?: boolean,
  withLabels?: boolean
) {
  const profiler = new TimeProfiler(
    intervalMicros,
    durationMicros,
    !!lineNumbers,
    !!withLabels
  );
  start();

  return {
    stop: majorVersion < 16 ? stopOld : stop,
    setLabels,
  };

  function start() {
    profiler.start();
  }

  // Node.js versions prior to v16 leak memory if not disposed and recreated
  // between each profile. As disposing deletes current profile data too,
  // we must stop then dispose then start.
  function stopOld(restart = false) {
    const result = profiler.stop(false);
    if (restart) {
      start();
    }
    return serializeTimeProfile(result, intervalMicros, sourceMapper, true);
  }

  function stop(restart = false) {
    const result = profiler.stop(restart);
    return serializeTimeProfile(result, intervalMicros, sourceMapper, true);
  }

  function setLabels(labels?: LabelSet) {
    profiler.labels = labels;
  }
}
