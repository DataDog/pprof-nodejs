#!/usr/bin/env node

/**
 * Copyright 2026 Datadog
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 */

const {spawnSync} = require('child_process');
const {existsSync} = require('fs');
const {join} = require('path');

const packageRoot = join(__dirname, '..');
const sourceEntry = join(packageRoot, 'ts', 'src', 'index.ts');
const compiledEntry = join(packageRoot, 'out', 'src', 'index.js');
const compiledTypes = join(packageRoot, 'out', 'src', 'index.d.ts');

function runNpmScript(scriptName) {
  const npmCmd = process.platform === 'win32' ? 'npm.cmd' : 'npm';
  const result = spawnSync(npmCmd, ['run', scriptName], {
    cwd: packageRoot,
    stdio: 'inherit',
    env: process.env,
  });
  if (result.status !== 0) {
    process.exit(result.status ?? 1);
  }
}

// For source installs (e.g. git dependencies), out/src can be absent.
// Compile once so downstream packages can resolve JS + type artifacts.
if (
  existsSync(sourceEntry) &&
  (!existsSync(compiledEntry) || !existsSync(compiledTypes))
) {
  runNpmScript('compile');
}
