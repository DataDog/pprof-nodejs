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
import * as sourceMap from 'source-map';

import {
  ANNOTATION_TAIL_BYTES,
  SourceMapper,
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

describe('SourceMapper.loadDirectory', () => {
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

  // A minimal valid source map for test.js -> test.ts
  const MAP_CONTENT = JSON.stringify({
    version: 3,
    file: 'test.js',
    sources: ['test.ts'],
    names: [],
    mappings: 'AAAA',
  });

  it('falls back to .map file when sourceMappingURL points to a non-existent file', async () => {
    // The annotation references a file that doesn't exist; Phase 2 should
    // find and load the conventional test.js.map instead.
    write('test.js', '//# sourceMappingURL=nonexistent.js.map\n');
    write('test.js.map', MAP_CONTENT);

    const sm = new SourceMapper();
    await sm.loadDirectory(tmpDir);

    assert.ok(
      sm.hasMappingInfo(path.join(tmpDir, 'test.js')),
      'expected mapping to be loaded via .map file fallback',
    );
  });

  it('loads no mapping when sourceMappingURL points to a non-existent file and there is no .map fallback', async () => {
    write('orphan.js', '//# sourceMappingURL=nonexistent.js.map\n');
    // No orphan.js.map written — nothing to fall back to.

    const sm = new SourceMapper();
    await sm.loadDirectory(tmpDir);

    assert.ok(
      !sm.hasMappingInfo(path.join(tmpDir, 'orphan.js')),
      'expected no mapping to be loaded',
    );
  });

  it('sets missingMapFile=true when sourceMappingURL declares a missing map', async () => {
    write('declared-missing.js', '//# sourceMappingURL=nonexistent.js.map\n');
    // No declared-missing.js.map written — nothing to fall back to.

    const sm = new SourceMapper();
    await sm.loadDirectory(tmpDir);

    const jsPath = path.join(tmpDir, 'declared-missing.js');
    const loc = sm.mappingInfo({file: jsPath, line: 1, column: 0, name: 'foo'});
    assert.strictEqual(
      loc.missingMapFile,
      true,
      'expected missingMapFile to be true for a file with a declared but missing map',
    );
  });

  it('does not set missingMapFile when file has no sourceMappingURL', async () => {
    write('plain.js', 'console.log("hello");\n');

    const sm = new SourceMapper();
    await sm.loadDirectory(tmpDir);

    const jsPath = path.join(tmpDir, 'plain.js');
    const loc = sm.mappingInfo({file: jsPath, line: 1, column: 0, name: 'foo'});
    assert.ok(
      !loc.missingMapFile,
      'expected missingMapFile to be falsy for a file with no sourceMappingURL',
    );
  });

  it('does not set missingMapFile for an inline data: URL with charset parameter', async () => {
    // data:application/json;charset=utf-8;base64,... is a valid inline form but
    // does not match the old exact INLINE_PREFIX. It must not be treated as a
    // file path and must not produce a false missingMapFile signal.
    const mapJson = JSON.stringify({
      version: 3,
      file: 'charset.js',
      sources: ['charset.ts'],
      names: [],
      mappings: 'AAAA',
    });
    const b64 = Buffer.from(mapJson).toString('base64');
    const url = `data:application/json;charset=utf-8;base64,${b64}`;
    write('charset.js', `//# sourceMappingURL=${url}\n`);

    const sm = new SourceMapper();
    await sm.loadDirectory(tmpDir);

    const jsPath = path.join(tmpDir, 'charset.js');
    assert.ok(
      sm.hasMappingInfo(jsPath),
      'expected mapping to be loaded from charset data: URL',
    );
    assert.ok(
      !sm.mappingInfo({file: jsPath, line: 1, column: 0, name: 'f'})
        .missingMapFile,
      'expected missingMapFile to be falsy for an inline charset data: URL',
    );
  });

  it('does not set missingMapFile when map was found via .map fallback', async () => {
    // JS with annotation pointing to nonexistent path, but a .map file exists
    // alongside it (Phase 2 fallback).
    const {SourceMapGenerator} = await import('source-map');
    const gen = new SourceMapGenerator({file: 'fallback.js'});
    gen.addMapping({
      source: path.join(tmpDir, 'source.ts'),
      generated: {line: 1, column: 0},
      original: {line: 10, column: 0},
    });
    write('fallback.js', '//# sourceMappingURL=nowhere.js.map\n');
    write('fallback.js.map', gen.toString());

    const sm = new SourceMapper();
    await sm.loadDirectory(tmpDir);

    const jsPath = path.join(tmpDir, 'fallback.js');
    const loc = sm.mappingInfo({file: jsPath, line: 1, column: 0, name: 'foo'});
    assert.ok(
      !loc.missingMapFile,
      'expected missingMapFile to be falsy when map was found via Phase 2 fallback',
    );
  });
});

// Regression test for the webpack source map issue originally surfaced in #81
// and relevant to #248.
//
// Webpack minifies output into a single line, placing multiple functions at
// different columns.  On Node.js < 25, V8's LineTick struct has no column
// field, so the C++ layer always emits column=0 for every sample.  The
// sourcemapper's LEAST_UPPER_BOUND path (triggered when column===0) then finds
// the first mapping on the line for every lookup — attributing all functions in
// the bundle to whichever source function appears first.
//
// On Node.js >= 25, V8 fills in real column numbers, so each function is
// looked up with GREATEST_LOWER_BOUND and resolves correctly.
//
// This test documents both behaviours so a regression would be immediately
// visible.
describe('SourceMapper.mappingInfo — webpack-style single-line bundle', () => {
  const MAP_DIR = path.resolve('app', 'dist');
  const BUNDLE_PATH = path.join(MAP_DIR, 'bundle.js');

  // Build a source map that places three functions on line 1 of bundle.js at
  // columns 10, 30 and 50, each originating from a different source file.
  async function buildMapper(): Promise<SourceMapper> {
    const gen = new sourceMap.SourceMapGenerator({file: 'bundle.js'});

    // funcA  — bundle.js line 1, col 10  →  a.ts line 1, col 0
    gen.addMapping({
      generated: {line: 1, column: 10},
      source: 'a.ts',
      original: {line: 1, column: 0},
      name: 'funcA',
    });
    // funcB  — bundle.js line 1, col 30  →  b.ts line 1, col 0
    gen.addMapping({
      generated: {line: 1, column: 30},
      source: 'b.ts',
      original: {line: 1, column: 0},
      name: 'funcB',
    });
    // funcC  — bundle.js line 1, col 50  →  c.ts line 1, col 0
    gen.addMapping({
      generated: {line: 1, column: 50},
      source: 'c.ts',
      original: {line: 1, column: 0},
      name: 'funcC',
    });

    const consumer = (await new sourceMap.SourceMapConsumer(
      gen.toJSON() as unknown as sourceMap.RawSourceMap,
    )) as unknown as sourceMap.RawSourceMap;

    const mapper = new SourceMapper();
    mapper.infoMap.set(BUNDLE_PATH, {
      mapFileDir: MAP_DIR,
      mapConsumer: consumer,
    });
    return mapper;
  }

  // Helper: look up a location in bundle.js.
  function lookup(mapper: SourceMapper, line: number, column: number) {
    return mapper.mappingInfo({
      file: BUNDLE_PATH,
      line,
      column,
      name: 'unknown',
    });
  }

  it('resolves functions correctly when real column numbers are available (Node.js ≥ 25 behaviour)', async () => {
    // When V8 supplies real 1-based columns (11, 31, 51 for cols 10, 30, 50)
    // GREATEST_LOWER_BOUND is used and each function maps to its own source.
    const mapper = await buildMapper();

    const a = lookup(mapper, 1, 11); // col 11 → adjusted 10 → funcA
    assert.strictEqual(a.name, 'funcA', 'funcA column');
    assert.ok(a.file!.endsWith('a.ts'), `funcA file: ${a.file}`);

    const b = lookup(mapper, 1, 31); // col 31 → adjusted 30 → funcB
    assert.strictEqual(b.name, 'funcB', 'funcB column');
    assert.ok(b.file!.endsWith('b.ts'), `funcB file: ${b.file}`);

    const c = lookup(mapper, 1, 51); // col 51 → adjusted 50 → funcC
    assert.strictEqual(c.name, 'funcC', 'funcC column');
    assert.ok(c.file!.endsWith('c.ts'), `funcC file: ${c.file}`);
  });

  it('resolves column=0 to the first mapping on the line (Node.js < 25 behaviour — known limitation)', async () => {
    // On Node.js < 25, LineTick has no column field so the C++ layer emits
    // column=0 for every sample.  LEAST_UPPER_BOUND is therefore used and all
    // three functions resolve to the *first* mapping on the line (funcA at
    // column 10).  This is a known limitation: distinct functions in a
    // webpack bundle cannot be differentiated on pre-25 Node.js.
    const mapper = await buildMapper();

    // All three functions are reported with column=0 (no real column info).
    const a = lookup(mapper, 1, 0);
    const b = lookup(mapper, 1, 0);
    const c = lookup(mapper, 1, 0);

    // They all resolve to the first mapped function on the line.
    assert.strictEqual(a.name, 'funcA', 'funcA with column=0');
    assert.strictEqual(
      b.name,
      'funcA',
      'funcB with column=0 maps to funcA — known limitation',
    );
    assert.strictEqual(
      c.name,
      'funcA',
      'funcC with column=0 maps to funcA — known limitation',
    );
  });
});
