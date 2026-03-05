/**
 * Copyright 2016 Google Inc. All Rights Reserved.
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

// Originally copied from cloud-debug-nodejs's sourcemapper.ts from
// https://github.com/googleapis/cloud-debug-nodejs/blob/7bdc2f1f62a3b45b7b53ea79f9444c8ed50e138b/src/agent/io/sourcemapper.ts
// Modified to map from generated code to source code, rather than from source
// code to generated code.

import * as fs from 'fs';
import * as path from 'path';
import * as sourceMap from 'source-map';
import {logger} from '../logger';

const readFile = fs.promises.readFile;

const CONCURRENCY = 10;

function createLimiter(concurrency: number) {
  let active = 0;
  const queue: Array<() => void> = [];
  return function limit<T>(fn: () => Promise<T>): Promise<T> {
    return new Promise<T>((resolve, reject) => {
      const run = () => {
        active++;
        fn()
          .then(resolve, reject)
          .finally(() => {
            active--;
            if (queue.length > 0) queue.shift()!();
          });
      };
      if (active < concurrency) run();
      else queue.push(run);
    });
  };
}
const MAP_EXT = '.map';

// Per TC39 ECMA-426 §11.1.2.1 JavaScriptExtractSourceMapURL (without parsing):
// https://tc39.es/ecma426/#sec-linking-inline
//
// Split on these line terminators (ECMA-262 LineTerminatorSequence):
const LINE_SPLIT_RE = /\r\n|\n|\r|\u2028|\u2029/;
// Bytes to read from the end of a JS file when scanning for the annotation.
// The annotation must be on the last non-empty line, which is always short
// for external URLs. If no line terminator appears in the tail we fall back
// to a full file read (handles very large inline data: maps).
export const ANNOTATION_TAIL_BYTES = 4 * 1024;
// Quote code points that invalidate the annotation (U+0022, U+0027, U+0060):
const QUOTE_CHARS_RE = /["'`]/;
// MatchSourceMapURL pattern applied to the comment text that follows "//":
const MATCH_SOURCE_MAP_URL_RE = /^[@#]\s*sourceMappingURL=(\S*?)\s*$/;

/**
 * Extracts a sourceMappingURL from JS source per ECMA-426 §11.1.2.1
 * (without-parsing variant).
 *
 * Scans lines from the end, skipping empty/whitespace-only lines.
 * Returns null as soon as the first non-empty line is found that does not
 * carry a valid annotation — the URL must be on the last non-empty line.
 */
export function extractSourceMappingURL(content: string): string | undefined {
  const lines = content.split(LINE_SPLIT_RE);
  for (let i = lines.length - 1; i >= 0; i--) {
    const line = lines[i];
    if (line.trim() === '') continue; // skip empty / whitespace-only lines

    // This is the last non-empty line; it must carry the annotation or we stop.
    const commentStart = line.indexOf('//');
    if (commentStart === -1) return undefined;

    const comment = line.slice(commentStart + 2);
    if (QUOTE_CHARS_RE.test(comment)) return undefined;

    const match = MATCH_SOURCE_MAP_URL_RE.exec(comment);
    return match ? match[1] || undefined : undefined;
  }
  return undefined;
}

/**
 * Reads the sourceMappingURL from a JS file efficiently by only reading a
 * small tail of the file.
 *
 * The annotation must be on the last non-empty line (ECMA-426), so as long as
 * the tail contains at least one line terminator the last line is fully
 * captured. If no line terminator appears in the tail the entire tail is part
 * of one very long inline data: line; we fall back to a full file read in
 * that case.
 */
export async function readSourceMappingURL(
  filePath: string,
): Promise<string | undefined> {
  const fd = await fs.promises.open(filePath, 'r');
  try {
    const {size} = await fd.stat();
    if (size === 0) return undefined;

    const tailSize = Math.min(ANNOTATION_TAIL_BYTES, size);
    const buf = Buffer.allocUnsafe(tailSize);
    await fd.read(buf, 0, tailSize, size - tailSize);
    const tail = buf.toString('utf8');

    // The last non-empty line is fully captured in the tail if and only if a
    // line terminator that precedes it also falls within the tail — i.e. the
    // last non-empty segment is not the very first element of the split result.
    //
    // Counter-example: a large inline map followed by trailing empty lines.
    // The tail might be "<end of base64>\n\n", which contains line terminators
    // but whose last non-empty content ("<end of base64>") is the first
    // segment — it extends before the window. Checking LINE_TERM_RE alone
    // would incorrectly accept this tail.
    const lines = tail.split(LINE_SPLIT_RE);
    let lastNonEmptyIdx = lines.length - 1;
    while (lastNonEmptyIdx > 0 && lines[lastNonEmptyIdx].trim() === '') {
      lastNonEmptyIdx--;
    }
    if (tailSize === size || lastNonEmptyIdx > 0) {
      return extractSourceMappingURL(tail);
    }

    const fullContent = await readFile(filePath, 'utf8');
    return extractSourceMappingURL(fullContent);
  } finally {
    await fd.close();
  }
}

function error(msg: string) {
  logger.debug(`Error: ${msg}`);
  return new Error(msg);
}

export interface MapInfoCompiled {
  mapFileDir: string;
  mapConsumer: sourceMap.RawSourceMap;
}

export interface GeneratedLocation {
  file: string;
  name?: string;
  line: number;
  column: number;
}

export interface SourceLocation {
  file?: string;
  name?: string;
  line?: number;
  column?: number;
}

/**
 * @param {!Map} infoMap The map that maps input source files to
 *  SourceMapConsumer objects that are used to calculate mapping information
 * @param {string} mapPath The path to the source map file to process.  The
 *  path should be relative to the process's current working directory
 * @private
 */
async function processSourceMap(
  infoMap: Map<string, MapInfoCompiled>,
  mapPath: string,
  debug: boolean,
): Promise<void> {
  // this handles the case when the path is undefined, null, or
  // the empty string
  if (!mapPath || !mapPath.endsWith(MAP_EXT)) {
    throw error(`The path "${mapPath}" does not specify a source map file`);
  }
  mapPath = path.normalize(mapPath);

  let contents;
  try {
    contents = await readFile(mapPath, 'utf8');
  } catch (e) {
    throw error('Could not read source map file ' + mapPath + ': ' + e);
  }

  /* If the source map file defines a "file" attribute, use it as
   * the output file where the path is relative to the directory
   * containing the map file.  Otherwise, use the name of the output
   * file (with the .map extension removed) as the output file.

   * With nextjs/webpack, when there are subdirectories in `pages` directory,
   * the generated source maps do not reference correctly the generated files
   * in their `file` property.
   * For example if the generated file / source maps have paths:
   * <root>/pages/sub/foo.js(.map)
   * foo.js.map will have ../pages/sub/foo.js as `file` property instead of
   * ../../pages/sub/foo.js
   * To workaround this, check first if file referenced in `file` property
   * exists and if it does not, check if generated file exists alongside the
   * source map file.
   */
  const dir = path.dirname(mapPath);

  // Parse the JSON lightly to get the `file` property before creating the
  // full SourceMapConsumer, so we can bail out early if the generated file
  // is already loaded (e.g. via a sourceMappingURL annotation).
  let rawFile: string | undefined;
  try {
    rawFile = (JSON.parse(contents) as {file?: string}).file;
  } catch {
    // Will fail again below when creating SourceMapConsumer; let that throw.
  }

  const generatedPathCandidates: string[] = [];
  if (rawFile) {
    generatedPathCandidates.push(path.resolve(dir, rawFile));
  }
  const samePath = path.resolve(dir, path.basename(mapPath, MAP_EXT));
  if (
    generatedPathCandidates.length === 0 ||
    generatedPathCandidates[0] !== samePath
  ) {
    generatedPathCandidates.push(samePath);
  }

  // Find the first candidate that exists and hasn't been loaded already.
  let targetPath: string | undefined;
  for (const candidate of generatedPathCandidates) {
    if (infoMap.has(candidate)) {
      // Already loaded via sourceMappingURL annotation; skip this map file.
      if (debug) {
        logger.debug(
          `Skipping ${mapPath}: ${candidate} already loaded via sourceMappingURL`,
        );
      }
      return;
    }
    try {
      await fs.promises.access(candidate, fs.constants.F_OK);
      targetPath = candidate;
      break;
    } catch {
      if (debug) {
        logger.debug(`Generated path ${candidate} does not exist`);
      }
    }
  }

  if (!targetPath) {
    if (debug) {
      logger.debug(`Unable to find generated file for ${mapPath}`);
    }
    return;
  }

  let consumer: sourceMap.RawSourceMap;
  try {
    // TODO: Determine how to reconsile the type conflict where `consumer`
    //       is constructed as a SourceMapConsumer but is used as a
    //       RawSourceMap.
    // TODO: Resolve the cast of `contents as any` (This is needed because the
    //       type is expected to be of `RawSourceMap` but the existing
    //       working code uses a string.)
    consumer = (await new sourceMap.SourceMapConsumer(
      contents as {} as sourceMap.RawSourceMap,
    )) as {} as sourceMap.RawSourceMap;
  } catch (e) {
    throw error(
      'An error occurred while reading the ' +
        'sourceMap file ' +
        mapPath +
        ': ' +
        e,
    );
  }

  infoMap.set(targetPath, {mapFileDir: dir, mapConsumer: consumer});
  if (debug) {
    logger.debug(`Loaded source map for ${targetPath} => ${mapPath}`);
  }
}

export class SourceMapper {
  infoMap: Map<string, MapInfoCompiled>;
  debug: boolean;

  static async create(
    searchDirs: string[],
    debug = false,
  ): Promise<SourceMapper> {
    const mapper = new SourceMapper(debug);
    for (const dir of searchDirs) {
      await mapper.loadDirectory(dir);
    }
    return mapper;
  }

  constructor(debug = false) {
    this.infoMap = new Map();
    this.debug = debug;
  }

  /**
   * Scans `searchDir` recursively for JS files and source map files, loading
   * source maps for all JS files found.
   *
   * Priority for each JS file:
   *  1. A map pointed to by a `sourceMappingURL` annotation in the JS file
   *     (inline `data:` URL or external file path, only if the file exists).
   *  2. A `.map` file found in the directory scan that claims to belong to
   *     that JS file (via its `file` property or naming convention).
   *
   * Safe to call multiple times; already-loaded files are skipped.
   */
  async loadDirectory(searchDir: string): Promise<void> {
    if (this.debug) {
      logger.debug(`Loading source maps from directory: ${searchDir}`);
    }

    const jsFiles: string[] = [];
    const mapFiles: string[] = [];

    for await (const entry of walk(
      searchDir,
      filename =>
        /\.[cm]?js$/.test(filename) || /\.[cm]?js\.map$/.test(filename),
      (root, dirname) =>
        root !== '/proc' && dirname !== '.git' && dirname !== 'node_modules',
    )) {
      if (entry.endsWith(MAP_EXT)) {
        mapFiles.push(entry);
      } else {
        jsFiles.push(entry);
      }
    }

    if (this.debug) {
      logger.debug(
        `Found ${jsFiles.length} JS files and ${mapFiles.length} map files in ${searchDir}`,
      );
    }

    const limit = createLimiter(CONCURRENCY);

    // Phase 1: Check sourceMappingURL annotations in JS files (higher priority).
    await Promise.all(
      jsFiles.map(jsPath =>
        limit(async () => {
          if (this.infoMap.has(jsPath)) return;

          let url: string | undefined;
          try {
            url = await readSourceMappingURL(jsPath);
          } catch {
            return;
          }
          if (!url) return;

          const INLINE_PREFIX = 'data:application/json;base64,';
          if (url.startsWith(INLINE_PREFIX)) {
            const mapContent = Buffer.from(
              url.slice(INLINE_PREFIX.length),
              'base64',
            ).toString();
            await this.loadMapContent(jsPath, mapContent, path.dirname(jsPath));
          } else {
            const mapPath = path.resolve(path.dirname(jsPath), url);
            try {
              const mapContent = await readFile(mapPath, 'utf8');
              await this.loadMapContent(
                jsPath,
                mapContent,
                path.dirname(mapPath),
              );
            } catch {
              // Map file doesn't exist or is unreadable; fall through to Phase 2.
            }
          }
        }),
      ),
    );

    // Phase 2: Process .map files for any JS files not yet resolved.
    await Promise.all(
      mapFiles.map(mapPath =>
        limit(() => processSourceMap(this.infoMap, mapPath, this.debug)),
      ),
    );
  }

  private async loadMapContent(
    jsPath: string,
    mapContent: string,
    mapDir: string,
  ): Promise<void> {
    try {
      const consumer = (await new sourceMap.SourceMapConsumer(
        mapContent as {} as sourceMap.RawSourceMap,
      )) as {} as sourceMap.RawSourceMap;
      this.infoMap.set(jsPath, {mapFileDir: mapDir, mapConsumer: consumer});
      if (this.debug) {
        logger.debug(`Loaded source map for ${jsPath} via sourceMappingURL`);
      }
    } catch (e) {
      logger.debug(`Failed to parse source map for ${jsPath}: ${e}`);
    }
  }

  /**
   * Used to get the information about the transpiled file from a given input
   * source file provided there isn't any ambiguity with associating the input
   * path to exactly one output transpiled file.
   *
   * @param inputPath The (possibly relative) path to the original source file.
   * @return The `MapInfoCompiled` object that describes the transpiled file
   *  associated with the specified input path.  `null` is returned if either
   *  zero files are associated with the input path or if more than one file
   *  could possibly be associated with the given input path.
   */
  private getMappingInfo(inputPath: string): MapInfoCompiled | null {
    const normalizedPath = path.normalize(inputPath);
    if (this.infoMap.has(normalizedPath)) {
      return this.infoMap.get(normalizedPath) as MapInfoCompiled;
    }
    return null;
  }

  /**
   * Used to determine if the source file specified by the given path has
   * a .map file and an output file associated with it.
   *
   * If there is no such mapping, it could be because the input file is not
   * the input to a transpilation process or it is the input to a transpilation
   * process but its corresponding .map file was not given to the constructor
   * of this mapper.
   *
   * @param {string} inputPath The path to an input file that could
   *  possibly be the input to a transpilation process.  The path should be
   *  relative to the process's current working directory.
   */
  hasMappingInfo(inputPath: string): boolean {
    return this.getMappingInfo(inputPath) !== null;
  }

  /**
   * @param {string} inputPath The path to an input file that could possibly
   *  be the input to a transpilation process.  The path should be relative to
   *  the process's current working directory
   * @param {number} The line number in the input file where the line number is
   *   zero-based.
   * @param {number} (Optional) The column number in the line of the file
   *   specified where the column number is zero-based.
   * @return {Object} The object returned has a "file" attribute for the
   *   path of the output file associated with the given input file (where the
   *   path is relative to the process's current working directory),
   *   a "line" attribute of the line number in the output file associated with
   *   the given line number for the input file, and an optional "column" number
   *   of the column number of the output file associated with the given file
   *   and line information.
   *
   *   If the given input file does not have mapping information associated
   *   with it then the input location is returned.
   */
  mappingInfo(location: GeneratedLocation): SourceLocation {
    const inputPath = path.normalize(location.file);
    const entry = this.getMappingInfo(inputPath);
    if (entry === null) {
      if (this.debug) {
        logger.debug(
          `Source map lookup failed: no map found for ${location.file} (normalized: ${inputPath})`,
        );
      }
      return location;
    }

    const generatedPos = {
      line: location.line,
      column: location.column > 0 ? location.column - 1 : 0, // SourceMapConsumer expects column to be 0-based
    };

    // TODO: Determine how to remove the explicit cast here.
    const consumer: sourceMap.SourceMapConsumer =
      entry.mapConsumer as {} as sourceMap.SourceMapConsumer;

    // When column is 0, we don't have real column info (e.g., from V8's LineTick
    // which only provides line numbers). Use LEAST_UPPER_BOUND to find the first
    // mapping on this line instead of failing because there's nothing at column 0.
    const bias =
      generatedPos.column === 0
        ? sourceMap.SourceMapConsumer.LEAST_UPPER_BOUND
        : sourceMap.SourceMapConsumer.GREATEST_LOWER_BOUND;

    const pos = consumer.originalPositionFor({...generatedPos, bias});
    if (pos.source === null) {
      if (this.debug) {
        logger.debug(
          `Source map lookup failed for ${location.name}(${location.file}:${location.line}:${location.column})`,
        );
      }
      return location;
    }

    const loc = {
      file: path.resolve(entry.mapFileDir, pos.source),
      line: pos.line || undefined,
      name: pos.name || location.name,
      column: pos.column === null ? undefined : pos.column + 1, // convert column back to 1-based
    };

    if (this.debug) {
      logger.debug(
        `Source map lookup succeeded for ${location.name}(${location.file}:${location.line}:${location.column}) => ${loc.name}(${loc.file}:${loc.line}:${loc.column})`,
      );
    }
    return loc;
  }
}

function isErrnoException(e: unknown): e is NodeJS.ErrnoException {
  return e instanceof Error && 'code' in e;
}

function isNonFatalError(error: unknown) {
  const nonFatalErrors = ['ENOENT', 'EPERM', 'EACCES', 'ELOOP'];

  return (
    isErrnoException(error) && error.code && nonFatalErrors.includes(error.code)
  );
}

async function* walk(
  dir: string,
  // eslint-disable-next-line @typescript-eslint/no-unused-vars
  fileFilter = (filename: string) => true,
  // eslint-disable-next-line @typescript-eslint/no-unused-vars
  directoryFilter = (root: string, dirname: string) => true,
): AsyncIterable<string> {
  async function* walkRecursive(dir: string): AsyncIterable<string> {
    try {
      for await (const d of await fs.promises.opendir(dir)) {
        const entry = path.join(dir, d.name);
        if (d.isDirectory() && directoryFilter(dir, d.name)) {
          yield* walkRecursive(entry);
        } else if (d.isFile() && fileFilter(d.name)) {
          // check that the file is readable
          await fs.promises.access(entry, fs.constants.R_OK);
          yield entry;
        }
      }
    } catch (error) {
      if (!isNonFatalError(error)) {
        throw error;
      } else {
        logger.debug(() => `Non fatal error: ${error}`);
      }
    }
  }

  yield* walkRecursive(dir);
}
