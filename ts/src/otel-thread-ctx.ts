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
 * Inputs to {@link runWithContext} and {@link enterWithContext}.
 *
 * `traceId` and `spanId` are passed as raw bytes (a `Uint8Array` of length
 * 16 and 8 respectively; `Buffer` is acceptable as a subclass).
 *
 * `attributes`, if present, is positional: index N in the array is the value
 * for uint8 key index N on the wire. Slots that are `null`, `undefined`, or
 * absent (array holes) are skipped. Non-string values are coerced via
 * `toString`. Values longer than 255 UTF-8 bytes are silently truncated and
 * attributes that would overflow the 612-byte payload budget are silently
 * dropped — see {@link isContextTruncated} for how to detect that. Array
 * length must not exceed 256.
 */
export interface ContextOptions {
  traceId: Uint8Array;
  spanId: Uint8Array;
  attributes?: Array<string | null | undefined>;
}

/**
 * Inputs to the methods returned by {@link makeNamedContext}. Same as
 * {@link ContextOptions} but attributes are addressed by name; names are
 * resolved to uint8 key indexes using the array passed to
 * {@link makeNamedContext}.
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
 * Object returned by {@link makeNamedContext}.
 */
export interface NamedContext {
  runWithContext<T>(fn: () => T, opts: NamedContextOptions): T;
  enterWithContext(opts: NamedContextOptions): void;
  clearContext(): void;
  appendAttributes(
    namedAttributes:
      | Record<string, unknown>
      | Map<string, unknown>
      | Array<[string, unknown]>,
  ): void;
  isContextTruncated(): boolean;
  readonly processContextAttributes: ProcessContextAttributes;
}

interface CtxWrap {
  bytes(): Uint8Array;
  append(attributes: Array<string | null | undefined> | undefined): void;
  isTruncated(): boolean;
}

interface Addon {
  otelThreadCtxWrap: new (
    traceId: Uint8Array,
    spanId: Uint8Array,
    attributes: Array<string | null | undefined> | undefined,
  ) => CtxWrap;
  otelThreadCtxStoreAls(als: AsyncLocalStorage<CtxWrap>): void;
  otelThreadCtxGetStoredAlsHash(): number;
  otelThreadCtxWrappedObjectOffset: number;
  otelThreadCtxTaggedSize: number;
}

const SCHEMA_VERSION = 'nodejs_v1';

// V8 layout constants the addon captured from the V8 headers Node bundles.
// On non-Linux these fall back to values matching Node's standard build
// (no V8 pointer compression, no sandbox); the reader is Linux-only per
// the OTEP anyway, so the fallbacks just keep processContextAttributes
// consistent in shape.
let WRAPPED_OBJECT_OFFSET = 24;
let TAGGED_SIZE = 8;

export let runWithContext: <T>(fn: () => T, opts: ContextOptions) => T;
export let enterWithContext: (opts: ContextOptions) => void;
/**
 * Detach any thread-context record from the current asynchronous scope.
 * Subsequent reads in the same scope (until a new
 * {@link runWithContext}/{@link enterWithContext} attaches one) see no
 * active context. Idempotent. On non-Linux platforms this is a no-op.
 */
export let clearContext: () => void;
export let appendAttributes: (
  attributes: Array<string | null | undefined>,
) => void;
export let isContextTruncated: () => boolean;

// Debug accessor (not part of the stable API; for tests / reader dev).
export let _currentRecordBytes: () => Uint8Array | undefined = () => undefined;

if (process.platform === 'linux') {
  // eslint-disable-next-line @typescript-eslint/no-require-imports
  const findBinding = require('node-gyp-build');
  const addon: Addon = findBinding(join(__dirname, '..', '..'));
  WRAPPED_OBJECT_OFFSET = addon.otelThreadCtxWrappedObjectOffset;
  TAGGED_SIZE = addon.otelThreadCtxTaggedSize;

  let als: AsyncLocalStorage<CtxWrap> | undefined;

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

  function ensureHook(): AsyncLocalStorage<CtxWrap> {
    if (als) return als;
    const err = asyncContextFrameError();
    if (err) {
      throw new Error(
        `otel thread-ctx writer requires async_context_frame support, which is unavailable: ${err}.`,
      );
    }
    als = new AsyncLocalStorage<CtxWrap>();
    addon.otelThreadCtxStoreAls(als);
    return als;
  }

  function buildWrap(opts: ContextOptions): CtxWrap {
    if (!opts || typeof opts !== 'object') {
      throw new TypeError('options object required');
    }
    ensureHook();
    return new addon.otelThreadCtxWrap(
      opts.traceId,
      opts.spanId,
      opts.attributes,
    );
  }

  runWithContext = function <T>(fn: () => T, opts: ContextOptions): T {
    const wrap = buildWrap(opts);
    return ensureHook().run(wrap, fn);
  };

  enterWithContext = function (opts: ContextOptions): void {
    const wrap = buildWrap(opts);
    ensureHook().enterWith(wrap);
  };

  clearContext = function (): void {
    // Idempotent: clearing when no hook has been installed yet (and
    // therefore no context can be active) is a no-op.
    if (!als) return;
    als.enterWith(undefined as unknown as CtxWrap);
  };

  appendAttributes = function (
    attributes: Array<string | null | undefined>,
  ): void {
    if (!als) {
      throw new Error(
        'no active thread context; call runWithContext or enterWithContext first',
      );
    }
    const wrap = als.getStore();
    if (!wrap) {
      throw new Error(
        'no active thread context; call runWithContext or enterWithContext first',
      );
    }
    wrap.append(attributes);
  };

  isContextTruncated = function (): boolean {
    if (!als) return false;
    const wrap = als.getStore();
    if (!wrap) return false;
    return wrap.isTruncated();
  };

  _currentRecordBytes = function (): Uint8Array | undefined {
    if (!als) return undefined;
    const wrap = als.getStore();
    if (!wrap) return undefined;
    return wrap.bytes();
  };
} else {
  runWithContext = function <T>(fn: () => T, _opts: ContextOptions): T {
    return fn();
  };
  enterWithContext = function (_opts: ContextOptions): void {};
  clearContext = function (): void {};
  appendAttributes = function (
    _attributes: Array<string | null | undefined>,
  ): void {};
  isContextTruncated = function (): boolean {
    return false;
  };
}

/**
 * Build name-addressed wrappers around {@link runWithContext},
 * {@link enterWithContext}, and {@link appendAttributes}. The supplied
 * `keys` array is the same string list the caller publishes (or has
 * published) as the `threadlocal.attribute_key_map` resource attribute in
 * the OTEP-4719 process context: index N in this array is the uint8 key
 * index N in the on-the-wire record. The mapping is captured once at
 * factory time.
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
    if (named == null) return undefined;
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

  function toBaseOpts(opts: NamedContextOptions): ContextOptions {
    if (!opts || typeof opts !== 'object') {
      throw new TypeError('options object required');
    }
    return {
      traceId: opts.traceId,
      spanId: opts.spanId,
      attributes: resolveAttributes(opts.namedAttributes),
    };
  }

  const processContextAttributes = Object.freeze({
    'threadlocal.schema_version': SCHEMA_VERSION,
    'threadlocal.attribute_key_map': Object.freeze(keys.slice()),
    'threadlocal.nodejs_v1.wrapped_object_offset': WRAPPED_OBJECT_OFFSET,
    'threadlocal.nodejs_v1.tagged_size': TAGGED_SIZE,
  }) as ProcessContextAttributes;

  return {
    runWithContext<T>(fn: () => T, opts: NamedContextOptions): T {
      return runWithContext(fn, toBaseOpts(opts));
    },
    enterWithContext(opts: NamedContextOptions): void {
      enterWithContext(toBaseOpts(opts));
    },
    clearContext(): void {
      clearContext();
    },
    appendAttributes(
      namedAttributes:
        | Record<string, unknown>
        | Map<string, unknown>
        | Array<[string, unknown]>,
    ): void {
      appendAttributes(resolveAttributes(namedAttributes)!);
    },
    isContextTruncated(): boolean {
      return isContextTruncated();
    },
    processContextAttributes,
  };
}
