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
import * as assert from 'assert';
import * as fs from 'fs';
import * as path from 'path';
import * as tmp from 'tmp';

import {
  ANNOTATION_TAIL_BYTES,
  extractSourceMappingURL,
  readSourceMappingURL,
} from '../src/sourcemapper/sourcemapper';

describe('extractSourceMappingURL', () => {
  it('returns URL from a standard annotation', () => {
    assert.strictEqual(
      extractSourceMappingURL('//# sourceMappingURL=foo.js.map\n'),
      'foo.js.map',
    );
  });

  it('accepts legacy //@ prefix', () => {
    assert.strictEqual(
      extractSourceMappingURL('//@ sourceMappingURL=foo.js.map\n'),
      'foo.js.map',
    );
  });

  it('skips trailing empty and whitespace-only lines', () => {
    assert.strictEqual(
      extractSourceMappingURL('//# sourceMappingURL=foo.js.map\n\n   \n'),
      'foo.js.map',
    );
  });

  it('allows leading whitespace before //', () => {
    assert.strictEqual(
      extractSourceMappingURL('   //# sourceMappingURL=foo.js.map\n'),
      'foo.js.map',
    );
  });

  it('returns undefined when last non-empty line has no // comment', () => {
    assert.strictEqual(extractSourceMappingURL('const x = 1;\n'), undefined);
  });

  it('returns undefined when // comment does not match annotation pattern', () => {
    assert.strictEqual(
      extractSourceMappingURL('// some other comment\n'),
      undefined,
    );
  });

  it('returns undefined (early exit) when last non-empty line is not an annotation, even if earlier lines are', () => {
    // The annotation must be on the last non-empty line; earlier ones are ignored.
    assert.strictEqual(
      extractSourceMappingURL(
        '//# sourceMappingURL=foo.js.map\nconst x = 1;\n',
      ),
      undefined,
    );
  });

  it('returns undefined when comment contains a double-quote', () => {
    assert.strictEqual(
      extractSourceMappingURL('//# sourceMappingURL="foo.js.map"\n'),
      undefined,
    );
  });

  it('returns undefined when comment contains a single-quote', () => {
    assert.strictEqual(
      extractSourceMappingURL("//# sourceMappingURL='foo.js.map'\n"),
      undefined,
    );
  });

  it('returns undefined when comment contains a backtick', () => {
    assert.strictEqual(
      extractSourceMappingURL('//# sourceMappingURL=`foo.js.map`\n'),
      undefined,
    );
  });

  it('returns undefined for empty content', () => {
    assert.strictEqual(extractSourceMappingURL(''), undefined);
  });

  it('returns undefined for whitespace-only content', () => {
    assert.strictEqual(extractSourceMappingURL('   \n\n   \n'), undefined);
  });

  it('handles all line terminator variants', () => {
    assert.strictEqual(
      extractSourceMappingURL('x\r//# sourceMappingURL=a.map'),
      'a.map',
    );
    assert.strictEqual(
      extractSourceMappingURL('x\r\n//# sourceMappingURL=b.map'),
      'b.map',
    );
    assert.strictEqual(
      extractSourceMappingURL('x\u2028//# sourceMappingURL=c.map'),
      'c.map',
    );
    assert.strictEqual(
      extractSourceMappingURL('x\u2029//# sourceMappingURL=d.map'),
      'd.map',
    );
  });

  it('returns a data: URL for inline source maps', () => {
    const map = Buffer.from('{"mappings":""}').toString('base64');
    const url = `data:application/json;base64,${map}`;
    assert.strictEqual(
      extractSourceMappingURL(`//# sourceMappingURL=${url}\n`),
      url,
    );
  });
});

describe('readSourceMappingURL', () => {
  let tmpDir: string;

  before(() => {
    tmp.setGracefulCleanup();
    tmpDir = tmp.dirSync().name;
  });

  function write(name: string, content: string): string {
    const p = path.join(tmpDir, name);
    fs.writeFileSync(p, content, 'utf8');
    return p;
  }

  // Build a fake base64 payload larger than ANNOTATION_TAIL_BYTES to force
  // the "last non-empty line extends before the tail window" scenario.
  const LARGE_BASE64 = 'A'.repeat(ANNOTATION_TAIL_BYTES + 128);
  const LARGE_ANNOTATION = `//# sourceMappingURL=data:application/json;base64,${LARGE_BASE64}`;

  it('reads external URL from a small file (fits entirely in tail)', async () => {
    const p = write('ext-small.js', '//# sourceMappingURL=ext-small.js.map\n');
    assert.strictEqual(await readSourceMappingURL(p), 'ext-small.js.map');
  });

  it('reads inline data: URL from a small file (fits entirely in tail)', async () => {
    const map = Buffer.from('{"mappings":""}').toString('base64');
    const url = `data:application/json;base64,${map}`;
    const p = write('inline-small.js', `//# sourceMappingURL=${url}\n`);
    assert.strictEqual(await readSourceMappingURL(p), url);
  });

  it('returns undefined for a small file with no annotation', async () => {
    const p = write('no-annotation.js', 'const x = 1;\n');
    assert.strictEqual(await readSourceMappingURL(p), undefined);
  });

  it('reads external URL from a large file (last line short, captured in tail)', async () => {
    // Pad the file so the total size exceeds ANNOTATION_TAIL_BYTES, but keep
    // the annotation line itself short so it fits within the tail.
    const padding = '//' + ' '.repeat(ANNOTATION_TAIL_BYTES) + '\n';
    const p = write(
      'ext-large.js',
      padding + '//# sourceMappingURL=ext-large.js.map\n',
    );
    assert.strictEqual(await readSourceMappingURL(p), 'ext-large.js.map');
  });

  it('reads large inline data: URL — no trailing newline (full-file fallback)', async () => {
    // The annotation line is longer than ANNOTATION_TAIL_BYTES with no
    // trailing newline, so the tail contains no line terminator → fallback.
    const p = write('inline-large-no-nl.js', LARGE_ANNOTATION);
    assert.strictEqual(
      await readSourceMappingURL(p),
      `data:application/json;base64,${LARGE_BASE64}`,
    );
  });

  it('reads large inline data: URL — single trailing newline (full-file fallback)', async () => {
    // tail = "<end of base64>\n" → lastNonEmptyIdx === 0 → fallback.
    const p = write('inline-large-one-nl.js', LARGE_ANNOTATION + '\n');
    assert.strictEqual(
      await readSourceMappingURL(p),
      `data:application/json;base64,${LARGE_BASE64}`,
    );
  });

  it('reads large inline data: URL — multiple trailing empty lines (full-file fallback)', async () => {
    // The bug case: tail = "<end of base64>\n\n" has line terminators but
    // lastNonEmptyIdx === 0, so we must not use the tail alone.
    const p = write('inline-large-multi-nl.js', LARGE_ANNOTATION + '\n\n\n');
    assert.strictEqual(
      await readSourceMappingURL(p),
      `data:application/json;base64,${LARGE_BASE64}`,
    );
  });

  it('returns undefined for a large file with no annotation', async () => {
    const padding = 'x'.repeat(ANNOTATION_TAIL_BYTES + 1) + '\n';
    const p = write('large-no-annotation.js', padding + 'const x = 1;\n');
    assert.strictEqual(await readSourceMappingURL(p), undefined);
  });

  it('returns undefined for an empty file', async () => {
    const p = write('empty.js', '');
    assert.strictEqual(await readSourceMappingURL(p), undefined);
  });
});
