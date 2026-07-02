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

// Vendored from https://github.com/polarsignals/custom-labels/tree/otel-thread-ctx-wip/js/test/
// (originally js/test/test.js, ported to TypeScript). Kept as a
// near-verbatim copy: edits should ideally land upstream first and be
// ported here, so the two stay in sync. We plan to drop this vendored
// copy once the upstream package is suitable to depend on directly.

// Tests intentionally use array holes to verify the writer's positional
// attribute encoding (where a hole means "no value at this key index").
/* eslint-disable no-sparse-arrays */

import assert from 'assert';
import {strict as strictAssert} from 'assert';
import {spawnSync} from 'node:child_process';
import {existsSync} from 'node:fs';
import {join} from 'node:path';

import {
  ThreadContext,
  getContext,
  clearContext,
  getProcessContextAttributes,
  _currentRecordBytes,
} from '../src/otel-thread-ctx';

// Helpers bridging the old positional-attrs test shape to the new
// ThreadContext-first API.
interface PosOpts {
  traceId: Uint8Array;
  spanId: Uint8Array;
  attributes?: Array<string | null | undefined>;
}
function tcRun<T>(fn: () => T, opts: PosOpts): T {
  return new ThreadContext(opts.traceId, opts.spanId, opts.attributes).run(fn);
}
function tcEnter(opts: PosOpts): void {
  new ThreadContext(opts.traceId, opts.spanId, opts.attributes).enter();
}
function tcAppend(
  attributes: Array<string | null | undefined> | undefined,
): void {
  getContext()!.appendAttributes(attributes);
}
function tcIsTruncated(): boolean {
  return getContext()?.isTruncated() ?? false;
}

const isLinux = process.platform === 'linux';
// AsyncContextFrame (the writer's discovery substrate) is opt-in on Node
// 22/23 (via --experimental-async-context-frame) and on by default in
// Node 24+ (disable-able via --no-async-context-frame). The TS layer
// refuses to install the hook when ACF isn't available, so the entire
// describe block is skipped in that case. Mirrors the source-side
// asyncContextFrameError logic.
const isAsyncContextFrameAvailable = (() => {
  if (process.execArgv.includes('--no-async-context-frame')) return false;
  const major = Number(process.versions.node.split('.')[0]);
  if (major >= 24) return true;
  if (major >= 22) {
    return process.execArgv.includes('--experimental-async-context-frame');
  }
  return false;
})();

// Returns a plain Uint8Array (not a Buffer) so assert.deepStrictEqual against
// other Uint8Arrays — including the one the addon returns — succeeds.
function bytesFromHex(hex: string): Uint8Array {
  return Uint8Array.from(Buffer.from(hex, 'hex'));
}

const TRACE_ID_BYTES = bytesFromHex('0102030405060708090a0b0c0d0e0f10');
const SPAN_ID_BYTES = bytesFromHex('1112131415161718');

interface Header {
  traceId: Uint8Array;
  spanId: Uint8Array;
  valid: number;
  reserved: number;
  attrsDataSize: number;
}

function decodeHeader(bytes: Uint8Array): Header {
  strictAssert.ok(
    bytes.length >= 28,
    `record must be at least 28 bytes, got ${bytes.length}`,
  );
  const attrsDataSize = bytes[26] | (bytes[27] << 8);
  strictAssert.equal(
    bytes.length,
    28 + attrsDataSize,
    `record length (${bytes.length}) must equal 28 + attrs_data_size (${attrsDataSize})`,
  );
  return {
    traceId: bytes.slice(0, 16),
    spanId: bytes.slice(16, 24),
    valid: bytes[24],
    reserved: bytes[25],
    attrsDataSize,
  };
}

// Returns the attribute payload as a positional sparse array, mirroring the
// writer's input shape: index N is the value for uint8 key index N on the
// wire; unset slots are array holes.
function decodeAttrs(bytes: Uint8Array): Array<string | undefined> {
  const hdr = decodeHeader(bytes);
  const out: Array<string | undefined> = [];
  let i = 28;
  const end = i + hdr.attrsDataSize;
  while (i < end) {
    const idx = bytes[i++];
    const len = bytes[i++];
    out[idx] = Buffer.from(bytes.slice(i, i + len)).toString('utf8');
    i += len;
  }
  strictAssert.equal(
    i,
    end,
    'attrs payload must be exactly attrsDataSize bytes',
  );
  return out;
}

function captureBytes(opts: {
  traceId: Uint8Array;
  spanId: Uint8Array;
  attributes?: Array<string | null | undefined>;
}): Uint8Array {
  let bytes: Uint8Array | undefined;
  tcRun(() => {
    bytes = _currentRecordBytes();
  }, opts);
  return bytes as Uint8Array;
}

(isLinux && isAsyncContextFrameAvailable ? describe : describe.skip)(
  'OTEP-4947 thread context (Linux-only)',
  () => {
    describe('ThreadContext construction', () => {
      it('accepts Uint8Array trace and span IDs', () => {
        const bytes = captureBytes({
          traceId: TRACE_ID_BYTES,
          spanId: SPAN_ID_BYTES,
        });
        const hdr = decodeHeader(bytes);
        strictAssert.deepEqual(hdr.traceId, TRACE_ID_BYTES);
        strictAssert.deepEqual(hdr.spanId, SPAN_ID_BYTES);
        strictAssert.equal(hdr.valid, 1);
        strictAssert.equal(hdr.reserved, 0);
        strictAssert.equal(hdr.attrsDataSize, 0);
      });

      it('accepts Buffer (Uint8Array subclass) trace and span IDs', () => {
        const bytes = captureBytes({
          traceId: Buffer.from(TRACE_ID_BYTES),
          spanId: Buffer.from(SPAN_ID_BYTES),
        });
        const hdr = decodeHeader(bytes);
        strictAssert.deepEqual(hdr.traceId, TRACE_ID_BYTES);
        strictAssert.deepEqual(hdr.spanId, SPAN_ID_BYTES);
      });

      it('rejects wrong-length traceId', () => {
        strictAssert.throws(
          () =>
            captureBytes({traceId: new Uint8Array(8), spanId: SPAN_ID_BYTES}),
          /traceId must be/,
        );
      });

      it('rejects wrong-length spanId', () => {
        strictAssert.throws(
          () =>
            captureBytes({traceId: TRACE_ID_BYTES, spanId: new Uint8Array(4)}),
          /spanId must be/,
        );
      });

      it('rejects non-Uint8Array traceId', () => {
        strictAssert.throws(
          () =>
            captureBytes({
              traceId: 'a'.repeat(32) as unknown as Uint8Array,
              spanId: SPAN_ID_BYTES,
            }),
          /traceId must be/,
        );
      });
    });

    describe('attribute encoding', () => {
      it('leaves attrs_data empty when no attributes are provided', () => {
        const bytes = captureBytes({
          traceId: TRACE_ID_BYTES,
          spanId: SPAN_ID_BYTES,
        });
        strictAssert.equal(decodeHeader(bytes).attrsDataSize, 0);
      });

      it('encodes attributes by position', () => {
        const bytes = captureBytes({
          traceId: TRACE_ID_BYTES,
          spanId: SPAN_ID_BYTES,
          attributes: ['GET', '/api/v1/widgets'],
        });
        strictAssert.deepEqual(decodeAttrs(bytes), ['GET', '/api/v1/widgets']);
      });

      it('skips null and undefined slots', () => {
        const bytes = captureBytes({
          traceId: TRACE_ID_BYTES,
          spanId: SPAN_ID_BYTES,
          attributes: ['zero', null, undefined, 'three'],
        });
        strictAssert.deepEqual(decodeAttrs(bytes), ['zero', , , 'three']);
      });

      it('skips trailing array holes', () => {
        const attributes: Array<string | undefined> = [];
        attributes[5] = 'five';
        const bytes = captureBytes({
          traceId: TRACE_ID_BYTES,
          spanId: SPAN_ID_BYTES,
          attributes,
        });
        strictAssert.deepEqual(decodeAttrs(bytes), [, , , , , 'five']);
      });

      it('coerces non-string values via toString', () => {
        const bytes = captureBytes({
          traceId: TRACE_ID_BYTES,
          spanId: SPAN_ID_BYTES,
          attributes: [42 as unknown as string, true as unknown as string],
        });
        strictAssert.deepEqual(decodeAttrs(bytes), ['42', 'true']);
      });

      it('truncates values longer than 255 bytes to 255', () => {
        const long = 'x'.repeat(300);
        const bytes = captureBytes({
          traceId: TRACE_ID_BYTES,
          spanId: SPAN_ID_BYTES,
          attributes: [long],
        });
        strictAssert.deepEqual(decodeAttrs(bytes), ['x'.repeat(255)]);
      });

      it('does not split a multibyte UTF-8 codepoint at the truncation boundary', () => {
        const euro = '€';
        const bytes = captureBytes({
          traceId: TRACE_ID_BYTES,
          spanId: SPAN_ID_BYTES,
          attributes: [euro.repeat(86)],
        });
        strictAssert.deepEqual(decodeAttrs(bytes), [euro.repeat(85)]);
        strictAssert.equal(decodeHeader(bytes).attrsDataSize, 2 + 255);

        const bytes2 = captureBytes({
          traceId: TRACE_ID_BYTES,
          spanId: SPAN_ID_BYTES,
          attributes: [euro.repeat(84) + 'éé'],
        });
        strictAssert.deepEqual(decodeAttrs(bytes2), [euro.repeat(84) + 'é']);
        strictAssert.equal(decodeHeader(bytes2).attrsDataSize, 2 + 254);
      });

      it('right-sizes an empty record to 28 bytes', () => {
        const bytes = captureBytes({
          traceId: TRACE_ID_BYTES,
          spanId: SPAN_ID_BYTES,
        });
        strictAssert.equal(bytes.length, 28);
      });

      it('right-sizes a one-short-attribute record to 28 + 2 + len bytes', () => {
        const bytes = captureBytes({
          traceId: TRACE_ID_BYTES,
          spanId: SPAN_ID_BYTES,
          attributes: ['GET'],
        });
        strictAssert.equal(bytes.length, 28 + 2 + 3);
      });

      it('skip-and-continue truncates past the 612-byte cap', () => {
        const a = 'a'.repeat(255);
        const b = 'b'.repeat(255);
        const c = 'c'.repeat(255);
        const d = 'd'.repeat(30);
        let bytes: Uint8Array | undefined;
        let truncated = false;
        tcRun(
          () => {
            bytes = _currentRecordBytes();
            truncated = tcIsTruncated();
          },
          {
            traceId: TRACE_ID_BYTES,
            spanId: SPAN_ID_BYTES,
            attributes: [a, b, c, d],
          },
        );
        strictAssert.deepEqual(decodeAttrs(bytes!), [a, b, , d]);
        strictAssert.equal(decodeHeader(bytes!).attrsDataSize, 514 + 32);
        strictAssert.equal(truncated, true);
      });

      it('rejects attributes array longer than 256', () => {
        const tooLong: Array<string | undefined> = new Array(257);
        strictAssert.throws(
          () =>
            captureBytes({
              traceId: TRACE_ID_BYTES,
              spanId: SPAN_ID_BYTES,
              attributes: tooLong,
            }),
          /must not exceed 256/,
        );
      });

      it('rejects non-array attributes argument', () => {
        strictAssert.throws(
          () =>
            captureBytes({
              traceId: TRACE_ID_BYTES,
              spanId: SPAN_ID_BYTES,
              attributes: {not: 'an array'} as unknown as Array<string>,
            }),
          /attributes must be an array/,
        );
      });
    });

    describe('runWithContext lifecycle', () => {
      it('returns the callback result', () => {
        const result = tcRun(() => 'ok', {
          traceId: TRACE_ID_BYTES,
          spanId: SPAN_ID_BYTES,
        });
        strictAssert.equal(result, 'ok');
      });

      it('has no active record outside the call', () => {
        strictAssert.equal(_currentRecordBytes(), undefined);
      });

      it('has no active record after the call returns', () => {
        tcRun(() => undefined, {
          traceId: TRACE_ID_BYTES,
          spanId: SPAN_ID_BYTES,
        });
        strictAssert.equal(_currentRecordBytes(), undefined);
      });

      it('restores the parent context after a nested call returns', () => {
        const outerOpts = {traceId: TRACE_ID_BYTES, spanId: SPAN_ID_BYTES};
        const innerSpanBytes = bytesFromHex('aabbccddeeff0011');
        const innerOpts = {traceId: TRACE_ID_BYTES, spanId: innerSpanBytes};

        tcRun(() => {
          const outerBefore = decodeHeader(_currentRecordBytes()!).spanId;
          tcRun(() => {
            const inner = decodeHeader(_currentRecordBytes()!).spanId;
            strictAssert.deepEqual(inner, innerSpanBytes);
          }, innerOpts);
          const outerAfter = decodeHeader(_currentRecordBytes()!).spanId;
          strictAssert.deepEqual(outerBefore, outerAfter);
          strictAssert.deepEqual(outerAfter, SPAN_ID_BYTES);
        }, outerOpts);
      });

      it('keeps the same record after awaits', async () => {
        await tcRun(
          async () => {
            const before = decodeHeader(_currentRecordBytes()!).spanId;
            await Promise.resolve();
            const afterMicro = decodeHeader(_currentRecordBytes()!).spanId;
            await new Promise(setImmediate);
            const afterMacro = decodeHeader(_currentRecordBytes()!).spanId;
            strictAssert.deepEqual(before, SPAN_ID_BYTES);
            strictAssert.deepEqual(afterMicro, SPAN_ID_BYTES);
            strictAssert.deepEqual(afterMacro, SPAN_ID_BYTES);
          },
          {traceId: TRACE_ID_BYTES, spanId: SPAN_ID_BYTES},
        );
      });

      it('keeps concurrent async calls isolated', async () => {
        const aSpan = bytesFromHex('1111111111111111');
        const bSpan = bytesFromHex('2222222222222222');

        async function run(spanBytes: Uint8Array) {
          return tcRun(
            async () => {
              const observed: Uint8Array[] = [];
              for (let i = 0; i < 4; i++) {
                observed.push(decodeHeader(_currentRecordBytes()!).spanId);
                await Promise.resolve();
              }
              return observed;
            },
            {traceId: TRACE_ID_BYTES, spanId: spanBytes},
          );
        }

        const [aObs, bObs] = await Promise.all([run(aSpan), run(bSpan)]);
        for (const s of aObs) strictAssert.deepEqual(s, aSpan);
        for (const s of bObs) strictAssert.deepEqual(s, bSpan);
      });
    });

    describe('enterWithContext', () => {
      it('attaches the record to the current async scope', () => {
        void tcRun(
          () => {
            strictAssert.deepEqual(
              decodeHeader(_currentRecordBytes()!).spanId,
              SPAN_ID_BYTES,
            );

            const newSpan = bytesFromHex('aabbccddeeff0011');
            tcEnter({traceId: TRACE_ID_BYTES, spanId: newSpan});
            strictAssert.deepEqual(
              decodeHeader(_currentRecordBytes()!).spanId,
              newSpan,
            );

            return Promise.resolve().then(() => {
              strictAssert.deepEqual(
                decodeHeader(_currentRecordBytes()!).spanId,
                newSpan,
              );
            });
          },
          {traceId: TRACE_ID_BYTES, spanId: SPAN_ID_BYTES},
        );

        strictAssert.equal(_currentRecordBytes(), undefined);
      });
    });

    describe('clearContext', () => {
      it('detaches the active record within a scope', () => {
        tcRun(
          () => {
            strictAssert.ok(_currentRecordBytes());
            clearContext();
            strictAssert.equal(_currentRecordBytes(), undefined);
          },
          {traceId: TRACE_ID_BYTES, spanId: SPAN_ID_BYTES},
        );
      });

      it('drops the active record so getContext returns undefined', () => {
        tcRun(
          () => {
            strictAssert.ok(getContext() !== undefined);
            clearContext();
            strictAssert.equal(getContext(), undefined);
            strictAssert.equal(tcIsTruncated(), false);
          },
          {traceId: TRACE_ID_BYTES, spanId: SPAN_ID_BYTES},
        );
      });

      it('is idempotent (calling with no context or twice is a no-op)', () => {
        clearContext();
        strictAssert.equal(_currentRecordBytes(), undefined);
        tcRun(
          () => {
            clearContext();
            clearContext();
            strictAssert.equal(_currentRecordBytes(), undefined);
          },
          {traceId: TRACE_ID_BYTES, spanId: SPAN_ID_BYTES},
        );
      });

      it('lets a nested runWithContext re-establish a record', () => {
        tcRun(
          () => {
            clearContext();
            const innerSpan = bytesFromHex('aabbccddeeff0011');
            tcRun(
              () => {
                strictAssert.deepEqual(
                  decodeHeader(_currentRecordBytes()!).spanId,
                  innerSpan,
                );
              },
              {traceId: TRACE_ID_BYTES, spanId: innerSpan},
            );
            // After the inner runWithContext returns, we're back to the
            // post-clear state in the outer scope.
            strictAssert.equal(_currentRecordBytes(), undefined);
          },
          {traceId: TRACE_ID_BYTES, spanId: SPAN_ID_BYTES},
        );
      });

      it('lets enterWithContext re-establish a record', () => {
        tcRun(
          () => {
            clearContext();
            const newSpan = bytesFromHex('aabbccddeeff0011');
            tcEnter({traceId: TRACE_ID_BYTES, spanId: newSpan});
            strictAssert.deepEqual(
              decodeHeader(_currentRecordBytes()!).spanId,
              newSpan,
            );
          },
          {traceId: TRACE_ID_BYTES, spanId: SPAN_ID_BYTES},
        );
      });
    });

    describe('appendAttributes', () => {
      it('adds entries to the current record', () => {
        tcRun(
          () => {
            strictAssert.deepEqual(decodeAttrs(_currentRecordBytes()!), [
              'GET',
            ]);
            tcAppend([, , '200']);
            strictAssert.deepEqual(decodeAttrs(_currentRecordBytes()!), [
              'GET',
              ,
              '200',
            ]);
          },
          {traceId: TRACE_ID_BYTES, spanId: SPAN_ID_BYTES, attributes: ['GET']},
        );
      });

      it('writes in-place when bytes fit in the slack', () => {
        tcRun(
          () => {
            const before = _currentRecordBytes()!;
            tcAppend([, 'ab']);
            const after = _currentRecordBytes()!;
            strictAssert.deepEqual(decodeAttrs(after), ['xxx', 'ab']);
            strictAssert.equal(after.length, before.length + 2 + 2);
            strictAssert.deepEqual(after.slice(0, 26), before.slice(0, 26));
            strictAssert.deepEqual(after.slice(28, 33), before.slice(28, 33));
            strictAssert.equal(after[24], 1);
          },
          {traceId: TRACE_ID_BYTES, spanId: SPAN_ID_BYTES, attributes: ['xxx']},
        );
      });

      it('grows the record geometrically when slack runs out', () => {
        tcRun(
          () => {
            const v = 'y'.repeat(60);
            for (let i = 0; i < 8; i++) {
              const append: Array<string | undefined> = [];
              append[i] = v;
              tcAppend(append);
            }
            const decoded = decodeAttrs(_currentRecordBytes()!);
            for (let i = 0; i < 8; i++) {
              strictAssert.equal(decoded[i], v, `slot ${i}`);
            }
            strictAssert.equal(
              decodeHeader(_currentRecordBytes()!).attrsDataSize,
              8 * 62,
            );
          },
          {traceId: TRACE_ID_BYTES, spanId: SPAN_ID_BYTES},
        );
      });

      it('is a no-op when given an empty array', () => {
        tcRun(
          () => {
            const before = _currentRecordBytes();
            tcAppend([]);
            const after = _currentRecordBytes();
            strictAssert.deepEqual(after, before);
          },
          {traceId: TRACE_ID_BYTES, spanId: SPAN_ID_BYTES},
        );
      });

      it('is a no-op when all slots are null/undefined', () => {
        tcRun(
          () => {
            const before = _currentRecordBytes();
            tcAppend([null, undefined, , null]);
            const after = _currentRecordBytes();
            strictAssert.deepEqual(after, before);
          },
          {traceId: TRACE_ID_BYTES, spanId: SPAN_ID_BYTES},
        );
      });

      it('silently drops entries past the 612-byte cap and sets the truncated flag', () => {
        const big = 'a'.repeat(255);
        tcRun(
          () => {
            tcAppend([big, big]);
            strictAssert.equal(tcIsTruncated(), false);
            tcAppend([, , big]);
            strictAssert.equal(tcIsTruncated(), true);
            strictAssert.equal(
              decodeHeader(_currentRecordBytes()!).attrsDataSize,
              514,
            );
            const small = 'x'.repeat(30);
            tcAppend([, , , small]);
            const decoded = decodeAttrs(_currentRecordBytes()!);
            strictAssert.equal(decoded[0], big);
            strictAssert.equal(decoded[1], big);
            strictAssert.equal(decoded[2], undefined);
            strictAssert.equal(decoded[3], small);
            strictAssert.equal(tcIsTruncated(), true);
          },
          {traceId: TRACE_ID_BYTES, spanId: SPAN_ID_BYTES},
        );
      });

      it('propagates through async continuations', async () => {
        await tcRun(
          async () => {
            tcAppend([, 'after-await']);
            await Promise.resolve();
            strictAssert.deepEqual(decodeAttrs(_currentRecordBytes()!), [
              'before',
              'after-await',
            ]);
          },
          {
            traceId: TRACE_ID_BYTES,
            spanId: SPAN_ID_BYTES,
            attributes: ['before'],
          },
        );
      });
    });

    describe('isContextTruncated', () => {
      it('returns false outside a context', () => {
        strictAssert.equal(tcIsTruncated(), false);
      });

      it('returns false for a non-truncated record', () => {
        tcRun(
          () => {
            strictAssert.equal(tcIsTruncated(), false);
          },
          {
            traceId: TRACE_ID_BYTES,
            spanId: SPAN_ID_BYTES,
            attributes: ['GET', '/x'],
          },
        );
      });

      it('reflects appended-then-overflowed entries', () => {
        tcRun(
          () => {
            strictAssert.equal(tcIsTruncated(), false);
            tcAppend([
              ,
              ,
              'c'.repeat(255),
              ,
              ,
              'd'.repeat(255),
              ,
              ,
              'e'.repeat(255),
            ]);
            strictAssert.equal(tcIsTruncated(), true);
          },
          {
            traceId: TRACE_ID_BYTES,
            spanId: SPAN_ID_BYTES,
            attributes: ['a', 'b'],
          },
        );
      });
    });

    describe('getProcessContextAttributes', () => {
      it('rejects non-array keys', () => {
        strictAssert.throws(
          () => getProcessContextAttributes({} as unknown as string[]),
          /must be an array/,
        );
      });

      it('rejects more than 256 keys', () => {
        const tooMany = Array.from({length: 257}, (_, i) => `k${i}`);
        strictAssert.throws(
          () => getProcessContextAttributes(tooMany),
          /exceeds 256/,
        );
      });

      it('rejects duplicate names', () => {
        strictAssert.throws(
          () => getProcessContextAttributes(['x', 'y', 'x']),
          /duplicate key name/,
        );
      });

      it('rejects non-string entries', () => {
        strictAssert.throws(
          () => getProcessContextAttributes(['ok', 42 as unknown as string]),
          /must be a string/,
        );
      });

      it('returns the expected shape', () => {
        const keys = ['http.method', 'http.route', 'user.id'];
        const pca = getProcessContextAttributes(keys);
        strictAssert.equal(pca['threadlocal.schema_version'], 'nodejs_v1_dev');
        strictAssert.deepEqual(pca['threadlocal.attribute_key_map'], keys);
        strictAssert.equal(pca['threadlocal.wrapped_object_offset'], 24);
        strictAssert.equal(pca['threadlocal.tagged_size'], 8);
        strictAssert.deepEqual(Object.keys(pca).sort(), [
          'threadlocal.attribute_key_map',
          'threadlocal.schema_version',
          'threadlocal.tagged_size',
          'threadlocal.wrapped_object_offset',
        ]);
      });

      it('is frozen and a defensive copy', () => {
        const keys = ['http.method', 'http.route'];
        const pca = getProcessContextAttributes(keys);
        strictAssert.ok(Object.isFrozen(pca));
        strictAssert.ok(Object.isFrozen(pca['threadlocal.attribute_key_map']));
        keys.push('mutated.after');
        strictAssert.deepEqual(pca['threadlocal.attribute_key_map'], [
          'http.method',
          'http.route',
        ]);
        strictAssert.throws(() => {
          (pca as unknown as Record<string, string>)[
            'threadlocal.schema_version'
          ] = 'tampered';
        }, /read-only|read only|TypeError/i);
      });
    });

    describe('discovery contract', () => {
      it('exports otel_thread_ctx_nodejs_v1 as a TLS dynsym', function () {
        const addon = join(
          __dirname,
          '..',
          '..',
          'build',
          'Release',
          'dd_pprof.node',
        );
        // The prebuild-install / node-gyp-build CI matrix runs against a
        // prebuilt binary that lives outside build/Release; only the
        // build-from-source path produces this exact file.
        if (!existsSync(addon)) {
          this.skip();
        }
        const r = spawnSync('readelf', ['--dyn-syms', '--wide', addon], {
          encoding: 'utf8',
        });
        if (r.error && (r.error as NodeJS.ErrnoException).code === 'ENOENT') {
          this.skip();
        }
        strictAssert.equal(r.status, 0, `readelf failed: ${r.stderr}`);
        const line = r.stdout
          .split('\n')
          .find(l => /\sotel_thread_ctx_nodejs_v1$/.test(l));
        assert.ok(
          line,
          'otel_thread_ctx_nodejs_v1 not present in dynamic symbol table',
        );
        assert.match(
          line!,
          /\bTLS\b/,
          `expected TLS type, got: ${line!.trim()}`,
        );
        assert.match(
          line!,
          /\bGLOBAL\b/,
          `expected GLOBAL binding, got: ${line!.trim()}`,
        );
        assert.match(
          line!,
          /\bDEFAULT\b/,
          `expected DEFAULT visibility, got: ${line!.trim()}`,
        );
      });
    });
  },
);
