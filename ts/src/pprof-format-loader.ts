/**
 * Copyright 2026 Datadog
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 */

import {join} from 'path';

import type * as PprofFormat from 'pprof-format';

import {runtime} from './runtime';

let cachedModule: typeof PprofFormat | undefined;

function loadFromPackageRoot(): typeof PprofFormat {
  const packageJsonPath = require.resolve('pprof-format/package.json');
  const packageRoot = packageJsonPath.slice(
    0,
    packageJsonPath.length - 'package.json'.length
  );
  return require(join(packageRoot, 'dist/commonjs/index.js'));
}

export function loadPprofFormat(): typeof PprofFormat {
  if (cachedModule) {
    return cachedModule;
  }

  try {
    const loaded = require('pprof-format') as typeof PprofFormat;
    cachedModule = loaded;
    return loaded;
  } catch (error) {
    if (runtime !== 'bun') {
      throw error;
    }
  }

  cachedModule = loadFromPackageRoot();
  return cachedModule;
}
