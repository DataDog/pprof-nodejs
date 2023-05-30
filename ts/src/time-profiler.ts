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
const DEFAULT_DURATION_MILLIS: Milliseconds = 60000;

const majorVersion = process.version.slice(1).split('.').map(Number)[0];

type Microseconds = number;
type Milliseconds = number;

export interface TimeProfilerOptions {
  /** time in milliseconds for which to collect profile. */
  durationMillis: Milliseconds;
  /** average time in microseconds between samples */
  intervalMicros?: Microseconds;
  sourceMapper?: SourceMapper;
  name?: string;

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
    options.name,
    options.sourceMapper,
    options.lineNumbers
  );
  await delay(options.durationMillis);
  return stop();
}

function ensureRunName(name?: string) {
  return name || `pprof-${Date.now()}-${Math.random()}`;
}

// Temporarily retained for backwards compatibility with older tracer
export function start(
  intervalMicros: Microseconds = DEFAULT_INTERVAL_MICROS,
  name?: string,
  sourceMapper?: SourceMapper,
  lineNumbers = true
) {
  const {stop} = startInternal(
    intervalMicros,
    // Duration must be at least intervalMicros; not used anyway when
    // not collecting extra info (CPU time, labels) with samples.
    intervalMicros,
    name,
    sourceMapper,
    lineNumbers,
    false
  );
  return stop;
}

export function startWithLabels(
  intervalMicros: Microseconds = DEFAULT_INTERVAL_MICROS,
  durationMillis: Milliseconds = DEFAULT_DURATION_MILLIS,
  name?: string,
  sourceMapper?: SourceMapper,
  lineNumbers = true
) {
  return startInternal(
    intervalMicros,
    durationMillis * 1000,
    name,
    sourceMapper,
    lineNumbers,
    true
  );
}

// NOTE: refreshing doesn't work if giving a profile name.
function startInternal(
  intervalMicros: Microseconds,
  durationMicros: Microseconds,
  name?: string,
  sourceMapper?: SourceMapper,
  lineNumbers?: boolean,
  withLabels?: boolean
) {
  const profiler = new TimeProfiler(intervalMicros, durationMicros);
  let runName = start();
  return {
    stop: majorVersion < 16 ? stopOld : stop,
    setLabels,
    labelsCaptured,
  };

  function start() {
    const runName = ensureRunName(name);
    profiler.start(runName, lineNumbers, withLabels);
    return runName;
  }

  // Node.js versions prior to v16 leak memory if not disposed and recreated
  // between each profile. As disposing deletes current profile data too,
  // we must stop then dispose then start.
  function stopOld(restart = false) {
    const result = profiler.stop(runName, lineNumbers);
    profiler.dispose();
    if (restart) {
      runName = start();
    }
    return serializeTimeProfile(result, intervalMicros, sourceMapper, true);
  }

  // For Node.js v16+, we want to start the next profile before we stop the
  // current one as otherwise the active profile count could reach zero which
  // means V8 might tear down the symbolizer thread and need to start it again.
  function stop(restart = false) {
    let nextRunName;
    if (restart) {
      nextRunName = start();
    }
    const result = profiler.stop(runName, lineNumbers);
    if (nextRunName) {
      runName = nextRunName;
    }
    if (!restart) profiler.dispose();
    return serializeTimeProfile(result, intervalMicros, sourceMapper, true);
  }

  function setLabels(labels: LabelSet) {
    profiler.labels = labels;
  }

  function labelsCaptured() {
    return profiler.labelsCaptured;
  }
}
