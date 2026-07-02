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

// Vendored from https://github.com/polarsignals/custom-labels/tree/otel-thread-ctx-wip/js/
// (originally js/index.js + js/index.d.ts, merged into TypeScript). Kept
// as a near-verbatim copy: edits should ideally land upstream first and
// be ported here, so the two stay in sync. We plan to drop this vendored
// copy once the upstream package is suitable to depend on directly.

// Node.js writer for the OpenTelemetry Thread Local Context Record
// (OTEP-4947), discoverable from an out-of-process reader via the
// `otel_thread_ctx_nodejs_v1` thread-local symbol exported by
// `dd_pprof.node`.
//
// Linux only; on other platforms the exported functions degrade to no-ops.

import {join} from 'path';
import {AsyncLocalStorage} from 'node:async_hooks';

/**
 * OTEP-4719 process-context attributes corresponding to a particular
 * key list. Spread this into whatever attribute map the application
 * hands to its OTEP-4719 process-context publisher.
 */
export interface ProcessContextAttributes {
  readonly 'threadlocal.schema_version': 'nodejs_v1_dev';
  readonly 'threadlocal.attribute_key_map': readonly string[];
  readonly 'threadlocal.wrapped_object_offset': number;
  readonly 'threadlocal.tagged_size': number;
}

/**
 * A thread-context record. Construct with `new ThreadContext(...)`; install
 * via the {@link enter} or {@link run} instance methods. The underlying
 * native record is GC-owned: when no JS or async-context-frame reference
 * survives, it's freed.
 *
 * `appendAttributes` mutates the context's record in place. Because every
 * async-context frame that holds the same `ThreadContext` reference observes
 * the same native record buffer, an append is visible across all those
 * frames even when the reallocate path runs (the context's internal
 * pointer is updated, the JS object is not replaced).
 */
export interface ThreadContext {
  appendAttributes(
    attributes: Array<string | null | undefined> | undefined,
  ): void;
  isTruncated(): boolean;
  /** Debug-only: returns the on-the-wire record bytes. Not stable. */
  debugBytes(): Uint8Array;

  /**
   * Attach this context to the current async-context frame (and every
   * frame derived from it until the frame ends or {@link clearContext}
   * detaches it). Re-installing the same context reference is cheap (no
   * allocation); per-span caching of the context on the caller side is
   * the intended usage pattern.
   *
   * On non-Linux platforms this is a no-op.
   */
  enter(): void;

  /**
   * Attach this context for the duration of `fn`. Equivalent to
   * `als.run(this, fn)` — after `fn` returns, the previous context is
   * restored. Returns whatever `fn` returns; if `fn` returns a Promise,
   * the same Promise is propagated. On non-Linux platforms simply
   * invokes `fn`.
   */
  run<T>(fn: () => T): T;
}

/**
 * Constructor for {@link ThreadContext}. On non-Linux platforms, returns a
 * no-op instance whose methods do nothing — the OTEP-4947 reader
 * contract is ELF-TLSDESC, only meaningful on Linux.
 */
export interface ThreadContextCtor {
  new (
    traceId: Uint8Array,
    spanId: Uint8Array,
    attributes?: Array<string | null | undefined>,
  ): ThreadContext;
  readonly prototype: ThreadContext;
}

interface Addon {
  threadContext: ThreadContextCtor;
  otelThreadCtxStoreAls(als: AsyncLocalStorage<ThreadContext>): void;
  otelThreadCtxGetStoredAlsHash(): number;
  otelThreadCtxWrappedObjectOffset: number;
  otelThreadCtxTaggedSize: number;
}

const SCHEMA_VERSION = 'nodejs_v1_dev';

// V8 layout constants the addon captured from the V8 headers Node bundles.
// On non-Linux these fall back to values matching Node's standard build
// (no V8 pointer compression, no sandbox); the reader is Linux-only per
// the OTEP anyway, so the fallbacks just keep processContextAttributes
// consistent in shape.
let WRAPPED_OBJECT_OFFSET = 24;
let TAGGED_SIZE = 8;

/** {@inheritDoc ThreadContextCtor} */
export let ThreadContext: ThreadContextCtor;

/**
 * Returns the {@link ThreadContext} currently attached to the active
 * async-context frame, or `undefined` if none is.
 */
export let getContext: () => ThreadContext | undefined;

/**
 * Detach any {@link ThreadContext} from the current async-context frame.
 * Idempotent when no context is attached. On non-Linux platforms this is
 * a no-op.
 */
export let clearContext: () => void;

// Debug accessor (not part of the stable API; for tests / reader dev).
export let _currentRecordBytes: () => Uint8Array | undefined = () => undefined;

if (process.platform === 'linux') {
  // eslint-disable-next-line @typescript-eslint/no-require-imports
  const findBinding = require('node-gyp-build');
  const addon: Addon = findBinding(join(__dirname, '..', '..'));
  WRAPPED_OBJECT_OFFSET = addon.otelThreadCtxWrappedObjectOffset;
  TAGGED_SIZE = addon.otelThreadCtxTaggedSize;

  ThreadContext = addon.threadContext;

  let als: AsyncLocalStorage<ThreadContext> | undefined;

  function asyncContextFrameError(): string | undefined {
    const [major] = process.versions.node.split('.').map(Number);
    if (process.execArgv.includes('--no-async-context-frame')) {
      return 'Node explicitly launched with --no-async-context-frame';
    }
    if (major >= 24) return undefined;
    if (process.execArgv.includes('--experimental-async-context-frame')) {
      return undefined;
    }
    if (major >= 22) {
      return 'Node versions prior to v24 must be launched with --experimental-async-context-frame';
    }
    return 'Node major versions prior to v22 do not support the feature at all';
  }

  function ensureHook(): AsyncLocalStorage<ThreadContext> {
    if (als) return als;
    const err = asyncContextFrameError();
    if (err) {
      throw new Error(
        `otel thread-ctx writer requires async_context_frame support, which is unavailable: ${err}.`,
      );
    }
    als = new AsyncLocalStorage<ThreadContext>();
    addon.otelThreadCtxStoreAls(als);
    return als;
  }

  getContext = function (): ThreadContext | undefined {
    return als ? als.getStore() : undefined;
  };

  // Idempotent: clearing when the hook hasn't been installed (no prior
  // enter / run on a ThreadContext) is a no-op.
  clearContext = function (): void {
    if (!als) return;
    als.enterWith(undefined as unknown as ThreadContext);
  };

  // Install the active-context channel on the ThreadContext prototype so
  // the only way to push a ThreadContext into our AsyncLocalStorage is
  // via the context itself — callers can't poison the ALS with an
  // arbitrary object.
  ThreadContext.prototype.enter = function (this: ThreadContext): void {
    ensureHook().enterWith(this);
  };
  ThreadContext.prototype.run = function <T>(
    this: ThreadContext,
    fn: () => T,
  ): T {
    return ensureHook().run(this, fn);
  };

  _currentRecordBytes = function (): Uint8Array | undefined {
    if (!als) return undefined;
    const context = als.getStore();
    return context ? context.debugBytes() : undefined;
  };
} else {
  // Non-Linux degradation. The writer's reader contract is ELF-TLSDESC,
  // meaningful only on Linux; on other platforms we still want the API
  // to be callable so consumers don't have to gate every call site —
  // construction succeeds but produces an inert context, and the
  // enter/run/clearContext entry points don't wire anything into
  // AsyncLocalStorage.
  class NoopThreadContext implements ThreadContext {
    appendAttributes(): void {}
    isTruncated(): boolean {
      return false;
    }
    debugBytes(): Uint8Array {
      return new Uint8Array(0);
    }
    enter(): void {}
    run<T>(fn: () => T): T {
      return fn();
    }
  }
  ThreadContext = NoopThreadContext as ThreadContextCtor;
  getContext = function (): undefined {
    return undefined;
  };
  clearContext = function (): void {};
}

/**
 * Returns the OTEP-4719 process-context attributes the caller should
 * publish so an out-of-process reader can decode the on-the-wire uint8
 * key indexes back to attribute names. The supplied `keys` array is the
 * same string list the caller writes into the positional `attributes`
 * argument of {@link ThreadContext}: index N here is the uint8 key index
 * N in each record.
 *
 * `keys` is validated: must be a string array of length ≤ 256 with no
 * duplicates.
 */
export function getProcessContextAttributes(
  keys: string[],
): ProcessContextAttributes {
  if (!Array.isArray(keys)) {
    throw new TypeError('keys must be an array of attribute names');
  }
  if (keys.length > 256) {
    throw new RangeError('keys array exceeds 256 entries');
  }
  const seen = new Set<string>();
  for (let i = 0; i < keys.length; ++i) {
    const name = keys[i];
    if (typeof name !== 'string') {
      throw new TypeError('every key must be a string');
    }
    if (seen.has(name)) {
      throw new Error(`duplicate key name at index ${i}: ${name}`);
    }
    seen.add(name);
  }
  return Object.freeze({
    'threadlocal.schema_version': SCHEMA_VERSION,
    'threadlocal.attribute_key_map': Object.freeze(keys.slice()),
    'threadlocal.wrapped_object_offset': WRAPPED_OBJECT_OFFSET,
    'threadlocal.tagged_size': TAGGED_SIZE,
  }) as ProcessContextAttributes;
}
