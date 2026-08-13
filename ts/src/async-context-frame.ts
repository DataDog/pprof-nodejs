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

import {AsyncLocalStorage} from 'node:async_hooks';

let active: boolean | undefined;

/**
 * Whether this process's `AsyncLocalStorage` is backed by AsyncContextFrame,
 * which is what puts the active value in the isolate's
 * ContinuationPreservedEmbedderData slot that this addon reads.
 *
 * Feature-detected rather than inferred from the Node version plus
 * `process.execArgv`, because the two disagree in both directions and each
 * combination is reachable today:
 *
 * - `NODE_OPTIONS=--experimental-async-context-frame` is accepted on Node 22
 *   and 23 and turns ACF on without appearing in `execArgv`. Inferring "off"
 *   there makes callers refuse to run in a process that would have worked.
 * - `NODE_OPTIONS=--no-async-context-frame` is accepted on Node 24 and turns
 *   ACF off without appearing in `execArgv`. Inferring "on" there is the worse
 *   error: the CPED slot is never written, so a writer that starts anyway keeps
 *   looking healthy from JS — `getStore()` still works — while every
 *   out-of-process reader sees a record that nothing ever updates.
 * - A worker thread created with an explicit `execArgv` doesn't inherit the
 *   main thread's command line either, and tooling sometimes rewrites
 *   `process.execArgv` outright.
 *
 * With ACF, `run()` is implemented in terms of `enterWith()`; without it, it
 * isn't. Memoized: the answer is fixed for the life of the thread.
 */
export function isAsyncContextFrameActive(): boolean {
  if (active === undefined) {
    const probe = new AsyncLocalStorage<number>();
    let delegated = false;
    probe.enterWith = () => {
      delegated = true;
    };
    probe.run(0, () => {});
    probe.disable();
    active = delegated;
  }
  return active;
}

/**
 * How to turn AsyncContextFrame on, for the error message of whatever declined
 * to run without it.
 *
 * Advisory text only — never decide availability from this. That is what
 * {@link isAsyncContextFrameActive} is for.
 */
export function asyncContextFrameHint(): string {
  const version = process.versions.node;
  const major = Number(version.split('.')[0]);
  if (major < 22) {
    return `Node ${version} does not support it at all; Node 24 and later enable it by default`;
  }
  if (major < 24) {
    return `Node ${version} needs --experimental-async-context-frame, on the command line or in NODE_OPTIONS; Node 24 and later enable it by default`;
  }
  return `Node ${version} enables it by default, so something turned it off — look for --no-async-context-frame on the command line, in NODE_OPTIONS, or in this worker's execArgv`;
}
