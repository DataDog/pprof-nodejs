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

// Node.js writer for the OpenTelemetry Thread Local Context Record
// (OTEP-4947), discoverable from an out-of-process reader via the
// `otel_thread_ctx_nodejs_v1` thread-local symbol exported by
// `dd_pprof.node`.
//
// Linux only; on other platforms the exported functions degrade to no-ops.

import {join} from 'path';
import {AsyncLocalStorage} from 'node:async_hooks';

/**
 * Inputs to {@link NamedContext.buildContext} (and the convenience
 * methods that delegate to it).
 *
 * `traceId` and `spanId` are passed as raw bytes (a `Uint8Array` of length
 * 16 and 8 respectively; `Buffer` is acceptable as a subclass).
 *
 * `namedAttributes` are resolved to positional uint8 key indexes via the
 * `keys` array passed to {@link makeNamedContext}. Values are coerced to
 * strings via `toString`. Values longer than 255 UTF-8 bytes are silently
 * truncated, and attributes that would overflow the 612-byte payload cap
 * are silently dropped (see {@link ThreadContext.isTruncated}). Names that
 * aren't in the key map throw.
 */
export interface NamedContextOptions {
  traceId: Uint8Array;
  spanId: Uint8Array;
  namedAttributes?:
    | Record<string, unknown>
    | Map<string, unknown>
    | Array<[string, unknown]>;
}

/**
 * OTEP-4719 process-context attributes corresponding to a particular
 * {@link NamedContext}. Spread this into whatever attribute map the
 * application hands to its OTEP-4719 process-context publisher.
 */
export interface ProcessContextAttributes {
  readonly 'threadlocal.schema_version': 'nodejs_v1';
  readonly 'threadlocal.attribute_key_map': readonly string[];
  readonly 'threadlocal.nodejs_v1.wrapped_object_offset': number;
  readonly 'threadlocal.nodejs_v1.tagged_size': number;
}

/**
 * A thread-context record. Construct with `new ThreadContext(...)`; install
 * with {@link setContext} or {@link runWithContext}. The underlying
 * native record is GC-owned: when no JS or async-context-frame
 * reference survives, it's freed.
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
}

interface Addon {
  threadContext: ThreadContextCtor;
  otelThreadCtxStoreAls(als: AsyncLocalStorage<ThreadContext>): void;
  otelThreadCtxGetStoredAlsHash(): number;
  otelThreadCtxWrappedObjectOffset: number;
  otelThreadCtxTaggedSize: number;
}

/**
 * Object returned by {@link makeNamedContext}. Resolves the
 * `namedAttributes` map to a positional array against the key list
 * captured at factory time and builds a {@link ThreadContext}; convenience
 * methods compose with {@link setContext} / {@link runWithContext}.
 */
export interface NamedContext {
  /** Allocate a ThreadContext with attributes resolved positionally by name. */
  buildContext(opts: NamedContextOptions): ThreadContext;
  /** Sugar: `setContext(buildContext(opts))`. */
  enterWithContext(opts: NamedContextOptions): void;
  /** Sugar: `runWithContext(buildContext(opts), fn)`. */
  runWithContext<T>(fn: () => T, opts: NamedContextOptions): T;
  /** Sugar: `setContext(undefined)`. */
  clearContext(): void;
  readonly processContextAttributes: ProcessContextAttributes;
}

const SCHEMA_VERSION = 'nodejs_v1';

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
 * Attach a {@link ThreadContext} (or `undefined` to detach) to the current
 * async-context frame. Idempotent for `setContext(undefined)` when no
 * frame has been installed. Re-installing the same context reference is
 * cheap (no allocation); per-span caching of the context on the caller
 * side is the intended usage pattern.
 */
export let setContext: (context: ThreadContext | undefined) => void;

/**
 * As {@link setContext}, but scoped to the callback's execution. After
 * `fn` returns, the previous context is restored.
 */
export let runWithContext: <T>(
  context: ThreadContext | undefined,
  fn: () => T,
) => T;

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

  setContext = function (context: ThreadContext | undefined): void {
    if (context === undefined) {
      // Idempotent: clearing when the hook hasn't been installed (no
      // prior setContext call) is a no-op.
      if (!als) return;
      als.enterWith(undefined as unknown as ThreadContext);
      return;
    }
    ensureHook().enterWith(context);
  };

  runWithContext = function <T>(
    context: ThreadContext | undefined,
    fn: () => T,
  ): T {
    if (context === undefined) {
      if (!als) return fn();
      return als.run(undefined as unknown as ThreadContext, fn);
    }
    return ensureHook().run(context, fn);
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
  // construction succeeds but produces an inert context, and setContext /
  // runWithContext don't actually wire anything into AsyncLocalStorage.
  class NoopThreadContext implements ThreadContext {
    appendAttributes(): void {}
    isTruncated(): boolean {
      return false;
    }
    debugBytes(): Uint8Array {
      return new Uint8Array(0);
    }
  }
  ThreadContext = NoopThreadContext as ThreadContextCtor;
  getContext = function (): undefined {
    return undefined;
  };
  setContext = function (): void {};
  runWithContext = function <T>(
    _context: ThreadContext | undefined,
    fn: () => T,
  ): T {
    return fn();
  };
}

/**
 * Build name-addressed wrappers around {@link ThreadContext},
 * {@link setContext}, and {@link runWithContext}. The supplied
 * `keys` array is the same string list the caller publishes (or has
 * published) as the `threadlocal.attribute_key_map` resource attribute
 * in the OTEP-4719 process context: index N in this array is the uint8
 * key index N in the on-the-wire record. The mapping is captured once
 * at factory time.
 */
export function makeNamedContext(keys: string[]): NamedContext {
  if (!Array.isArray(keys)) {
    throw new TypeError('keys must be an array of attribute names');
  }
  if (keys.length > 256) {
    throw new RangeError('keys array exceeds 256 entries');
  }
  const indexByName = new Map<string, number>();
  keys.forEach((name, i) => {
    if (typeof name !== 'string') {
      throw new TypeError('every key must be a string');
    }
    if (indexByName.has(name)) {
      throw new Error(
        `duplicate key name at indexes ${indexByName.get(name)} and ${i}: ${name}`,
      );
    }
    indexByName.set(name, i);
  });

  function resolveAttributes(
    named:
      | Record<string, unknown>
      | Map<string, unknown>
      | Array<[string, unknown]>
      | undefined,
  ): Array<string | null | undefined> | undefined {
    if (named === null || named === undefined) return undefined;
    const attributes: Array<string | undefined> = [];
    const set = (name: string, value: unknown) => {
      const idx = indexByName.get(name);
      if (idx === undefined) {
        throw new Error(`unknown attribute name: ${name}`);
      }
      attributes[idx] = String(value);
    };
    if (Array.isArray(named)) {
      for (const [n, v] of named) set(n, v);
    } else if (named instanceof Map) {
      for (const [n, v] of named) set(n, v);
    } else if (typeof named === 'object') {
      for (const n of Object.keys(named))
        set(n, (named as Record<string, unknown>)[n]);
    } else {
      throw new TypeError(
        'namedAttributes must be an object, Map, or array of pairs',
      );
    }
    return attributes;
  }

  function buildContext(opts: NamedContextOptions): ThreadContext {
    if (!opts || typeof opts !== 'object') {
      throw new TypeError('options object required');
    }
    return new ThreadContext(
      opts.traceId,
      opts.spanId,
      resolveAttributes(opts.namedAttributes),
    );
  }

  const processContextAttributes = Object.freeze({
    'threadlocal.schema_version': SCHEMA_VERSION,
    'threadlocal.attribute_key_map': Object.freeze(keys.slice()),
    'threadlocal.nodejs_v1.wrapped_object_offset': WRAPPED_OBJECT_OFFSET,
    'threadlocal.nodejs_v1.tagged_size': TAGGED_SIZE,
  }) as ProcessContextAttributes;

  return {
    buildContext,
    enterWithContext(opts: NamedContextOptions): void {
      setContext(buildContext(opts));
    },
    runWithContext<T>(fn: () => T, opts: NamedContextOptions): T {
      return runWithContext(buildContext(opts), fn);
    },
    clearContext(): void {
      setContext(undefined);
    },
    processContextAttributes,
  };
}
