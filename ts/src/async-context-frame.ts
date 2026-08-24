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
import {join} from 'path';

interface Addon {
  cpedMapContains(key: unknown, value: unknown): boolean;
}

let addon: Addon | undefined;

// Required lazily so importing this module doesn't force the addon to load;
// memoized by isAsyncContextFrameActive, so this runs at most once per thread.
function bindings(): Addon {
  if (!addon) {
    const findBinding = require('node-gyp-build');
    addon = findBinding(join(__dirname, '..', '..')) as Addon;
  }
  return addon;
}

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
 * - `NODE_OPTIONS=--experimental-async-context-frame` is accepted from Node
 *   22.7.0 through 23 and turns ACF on without appearing in `execArgv`.
 *   Inferring "off" there makes callers refuse to run in a process that would
 *   have worked.
 * - `NODE_OPTIONS=--no-async-context-frame` is accepted on Node 24 and turns
 *   ACF off without appearing in `execArgv`. Inferring "on" there is the worse
 *   error: the CPED slot is never written, so a writer that starts anyway keeps
 *   looking healthy from JS — `getStore()` still works — while every
 *   out-of-process reader sees a record that nothing ever updates.
 * - A worker thread created with an explicit `execArgv` doesn't inherit the
 *   main thread's command line either, and tooling sometimes rewrites
 *   `process.execArgv` outright.
 *
 * Detected by asking the addon what is in the CPED slot during a `run()`. With
 * ACF, Node installs an AsyncContextFrame — a JS Map keyed by the
 * `AsyncLocalStorage` instance, valued by its store — as the running
 * continuation's CPED; without it, nothing writes the slot. So a probe storage
 * whose own store is visible there is direct evidence, and it is evidence about
 * the exact slot both consumers read: `WallProfiler::SetContext` requires that
 * Map, and the thread-ctx reader looks this very key up by the identity hash
 * published as `als_identity_hash`.
 *
 * Observing whether `run()` delegates to `enterWith()` would be an indirect
 * proxy for the same thing: it holds today, but it depends on `run()`
 * dispatching through the instance property, which is unspecified and which
 * anything patching `AsyncLocalStorage` can break — and the failure would be
 * silent and in the dangerous direction.
 *
 * Memoized: the answer is fixed for the life of the thread.
 */
export function isAsyncContextFrameActive(): boolean {
  if (active === undefined) {
    const probe = new AsyncLocalStorage<object>();
    // Object identity, so a stray equal-valued binding can't answer for us.
    const sentinel = {};
    let bound = false;
    probe.run(sentinel, () => {
      bound = bindings().cpedMapContains(probe, sentinel);
    });
    probe.disable();
    active = bound;
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
  const [major, minor] = version.split('.').map(Number);
  // Hand-rolled rather than semver.satisfies: semver is a devDependency, and
  // this module ships.
  if (major < 22 || (major === 22 && minor < 7)) {
    return `Node ${version} does not support it at all; Node 24 and later enable it by default`;
  }
  if (major < 24) {
    return `Node ${version} needs --experimental-async-context-frame, on the command line or in NODE_OPTIONS; Node 24 and later enable it by default`;
  }
  return `Node ${version} enables it by default, so something turned it off — look for --no-async-context-frame on the command line, in NODE_OPTIONS, or in this worker's execArgv`;
}
