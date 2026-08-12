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

import {strict as assert} from 'assert';
import {fork} from 'node:child_process';
import {join} from 'node:path';

import {isAsyncContextFrameActive} from '../src/async-context-frame';

const CHILD = join(__dirname, 'async-context-frame-child.js');

const major = Number(process.versions.node.split('.')[0]);

interface ChildReport {
  active: boolean;
  execArgv: string[];
}

// Runs the probe in a child process configured the way the test wants, since
// AsyncContextFrame is decided at process start and can't be toggled in-process.
function probeChild(
  options: {execArgv?: string[]; nodeOptions?: string} = {},
): Promise<ChildReport> {
  return new Promise((resolve, reject) => {
    const child = fork(CHILD, [], {
      execArgv: options.execArgv ?? [],
      env: options.nodeOptions
        ? {...process.env, NODE_OPTIONS: options.nodeOptions}
        : {...process.env, NODE_OPTIONS: ''},
      stdio: ['ignore', 'ignore', 'pipe', 'ipc'],
    });
    let report: ChildReport | undefined;
    let stderr = '';
    child.stderr?.on('data', chunk => {
      stderr += chunk;
    });
    child.on('message', message => {
      report = message as ChildReport;
    });
    child.on('error', reject);
    child.on('exit', code => {
      if (report === undefined) {
        reject(
          new Error(
            `child exited with ${code} and no report; stderr: ${stderr}`,
          ),
        );
        return;
      }
      resolve(report);
    });
  });
}

describe('isAsyncContextFrameActive', () => {
  it('gives the same answer on every call', () => {
    const first = isAsyncContextFrameActive();
    assert.equal(typeof first, 'boolean');
    assert.equal(isAsyncContextFrameActive(), first);
  });

  it('reports it active when Node enables it by default', async function () {
    if (major < 24) return this.skip();
    const {active} = await probeChild();
    assert.equal(active, true);
  });

  it('reports it inactive when Node has no support for it', async function () {
    if (major >= 22) return this.skip();
    const {active} = await probeChild();
    assert.equal(active, false);
  });

  it('reports it inactive when the command line turns it off', async function () {
    // The flag only exists from Node 24, where ACF is the default.
    if (major < 24) return this.skip();
    const {active} = await probeChild({
      execArgv: ['--no-async-context-frame'],
    });
    assert.equal(active, false);
  });

  it('reports it inactive when NODE_OPTIONS turns it off', async function () {
    // The regression this detection exists for: Node 24 accepts the flag in
    // NODE_OPTIONS, where it does not reach execArgv, so inferring from execArgv
    // concludes ACF is on. It is off, the CPED slot is never written, and a
    // caller that trusted the inference would emit records nothing updates.
    if (major < 24) return this.skip();
    const {active, execArgv} = await probeChild({
      nodeOptions: '--no-async-context-frame',
    });
    assert.deepEqual(execArgv, []);
    assert.equal(active, false);
  });

  it('reports it active when NODE_OPTIONS turns it on', async function () {
    // The mirror image, on the other Node line: 22 and 23 accept the flag in
    // NODE_OPTIONS (24 rejects it outright), again without it reaching execArgv,
    // so inferring from execArgv concludes ACF is off when it is on — and the
    // caller refuses to run in a process that would have worked.
    if (major < 22 || major >= 24) return this.skip();
    const {active, execArgv} = await probeChild({
      nodeOptions: '--experimental-async-context-frame',
    });
    assert.deepEqual(execArgv, []);
    assert.equal(active, true);
  });
});
