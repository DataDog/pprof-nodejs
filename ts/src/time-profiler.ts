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
import {
  TimeProfiler,
  constants as profilerConstants,
} from './time-profiler-bindings';
import {LabelSet} from './v8-types';

const {kSampleCount} = profilerConstants;

const DEFAULT_INTERVAL_MICROS: Microseconds = 1000;
const DEFAULT_DURATION_MILLIS: Milliseconds = 60000;

type Microseconds = number;
type Milliseconds = number;

let gProfiler: InstanceType<typeof TimeProfiler> | undefined;
let gSourceMapper: SourceMapper | undefined;
let gIntervalMicros: Microseconds;
let gV8ProfilerStuckEventLoopDetected = false;

/** Make sure to stop profiler before node shuts down, otherwise profiling
 * signal might cause a crash if it occurs during shutdown */
process.once('exit', () => {
  if (isStarted()) stop();
});

export interface TimeProfilerOptions {
  /** time in milliseconds for which to collect profile. */
  durationMillis?: Milliseconds;
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
  withContexts?: boolean;
}

export async function profile({
  intervalMicros = DEFAULT_INTERVAL_MICROS,
  durationMillis = DEFAULT_DURATION_MILLIS,
  sourceMapper,
  lineNumbers = false,
  withContexts = false,
}: TimeProfilerOptions) {
  start({
    intervalMicros,
    durationMillis,
    sourceMapper,
    lineNumbers,
    withContexts,
  });
  await delay(durationMillis);
  return stop();
}

// Temporarily retained for backwards compatibility with older tracer
export function start({
  intervalMicros = DEFAULT_INTERVAL_MICROS,
  durationMillis = DEFAULT_DURATION_MILLIS,
  sourceMapper,
  lineNumbers = false,
  withContexts = false,
}: TimeProfilerOptions) {
  if (gProfiler) {
    throw new Error('Wall profiler is already started');
  }

  gProfiler = new TimeProfiler(
    intervalMicros,
    durationMillis * 1000,
    lineNumbers,
    withContexts
  );
  gSourceMapper = sourceMapper;
  gIntervalMicros = intervalMicros;
  gV8ProfilerStuckEventLoopDetected = false;
  gProfiler.start();
}

export function stop(
  restart = false,
  generateLabels?: (context: object) => LabelSet
) {
  if (!gProfiler) {
    throw new Error('Wall profiler is not started');
  }

  const profile = gProfiler.stop(restart);
  if (restart && gProfiler.v8ProfilerStuckEventLoopDetected()) {
    // Workaround for v8 bug, where profiler event processor thread is stuck in
    // a loop eating 100% CPU, leading to empty profiles.
    // Fully stop and restart the profiler to reset the profile to a valid state.
    gProfiler.stop(false);
    gProfiler.start();
    gV8ProfilerStuckEventLoopDetected = true;
  } else {
    gV8ProfilerStuckEventLoopDetected = false;
  }

  const serialized_profile = serializeTimeProfile(
    profile,
    gIntervalMicros,
    gSourceMapper,
    true,
    generateLabels
  );
  if (!restart) {
    gProfiler = undefined;
    gSourceMapper = undefined;
  }
  return serialized_profile;
}

export function getState() {
  if (!gProfiler) {
    throw new Error('Wall profiler is not started');
  }
  return gProfiler.state;
}

export function setContext(context?: object) {
  if (!gProfiler) {
    throw new Error('Wall profiler is not started');
  }
  gProfiler.context = context;
}

export function isStarted() {
  return !!gProfiler;
}

export function v8ProfilerStuckEventLoopDetected() {
  return gV8ProfilerStuckEventLoopDetected;
}

export const constants = {kSampleCount};
export {LabelSet};
