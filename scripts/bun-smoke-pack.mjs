#!/usr/bin/env bun

import {mkdtempSync, rmSync, unlinkSync, writeFileSync} from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {spawnSync} from 'node:child_process';
import {fileURLToPath, pathToFileURL} from 'node:url';

const scriptDir = path.dirname(fileURLToPath(import.meta.url));
const repoRoot = path.resolve(scriptDir, '..');
const tmpDir = mkdtempSync(path.join(os.tmpdir(), 'pprof-bun-pack-'));
const smokeScriptPath = path.join(tmpDir, 'bun-smoke-pack-check.mjs');
const smokeRunnerPath = path.join(scriptDir, 'bun-smoke-runner.mjs');
const npmCacheDir = path.join(tmpDir, 'npm-cache');
const baseEnv = {
  ...process.env,
  NPM_CONFIG_CACHE: process.env.NPM_CONFIG_CACHE ?? npmCacheDir,
};

try {
  const pack = spawnSync('npm', ['pack', '--silent'], {
    cwd: repoRoot,
    encoding: 'utf8',
    env: baseEnv,
    stdio: ['ignore', 'pipe', 'inherit'],
  });
  if (pack.status !== 0) {
    process.exit(pack.status ?? 1);
  }

  const tgzName = pack.stdout.trim().split('\n').at(-1);
  if (!tgzName) {
    throw new Error('npm pack did not produce a tarball name');
  }
  const tgzPath = path.join(repoRoot, tgzName);

  spawnOrThrow('npm', ['init', '-y'], tmpDir, baseEnv);
  spawnOrThrow('npm', ['install', tgzPath], tmpDir, baseEnv);

  writeFileSync(
    smokeScriptPath,
    [
      "import * as pprof from '@datadog/pprof';",
      `import {runSmoke} from '${pathToFileUrl(smokeRunnerPath)}';`,
      "console.log(JSON.stringify(await runSmoke(pprof)));",
      '',
    ].join('\n')
  );

  spawnOrThrow('bun', [smokeScriptPath], tmpDir, process.env);

  unlinkSync(tgzPath);
} finally {
  rmSync(smokeScriptPath, {force: true});
  rmSync(tmpDir, {recursive: true, force: true});
}

function spawnOrThrow(command, args, cwd, env) {
  const result = spawnSync(command, args, {
    cwd,
    stdio: 'inherit',
    env,
  });
  if (result.status !== 0) {
    throw new Error(`${command} ${args.join(' ')} failed with status ${result.status ?? 'null'}`);
  }
}

function pathToFileUrl(filePath) {
  return pathToFileURL(filePath).href;
}
