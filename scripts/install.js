'use strict';

const {spawnSync} = require('child_process');
const {existsSync} = require('fs');
const {join} = require('path');

const projectRoot = join(__dirname, '..');
const builtEntryPoint = join(projectRoot, 'out', 'src', 'index.js');

if (existsSync(builtEntryPoint)) {
  process.exit(0);
}

console.log(
  '[pprof] out/src is missing; compiling TypeScript and rebuilding native addon'
);

runStep('compile');
runStep('rebuild');

function runStep(scriptName) {
  const result = spawnSync(
    'npm',
    ['run', scriptName, '--silent'],
    {
      cwd: projectRoot,
      stdio: 'inherit',
      shell: process.platform === 'win32',
      env: process.env,
    }
  );

  if (result.status !== 0) {
    process.exit(result.status ?? 1);
  }
}
