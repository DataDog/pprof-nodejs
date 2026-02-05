/**
 * Regression test for CPED context pointer freelist growth.
 *
 * Runs the actual workload in a separate Node.js process launched with
 * `--expose-gc` so we can force GC deterministically.
 */

import assert from 'assert';
import {spawnSync} from 'child_process';
import path from 'path';
import {satisfies} from 'semver';

describe('CPED freelist trimming (regression)', () => {
  it('should plateau total async context pointers after enough churn', function () {
    this.timeout(120_000);

    if (process.platform !== 'darwin' && process.platform !== 'linux') {
      this.skip();
    }

    const supportsCPED =
      satisfies(process.versions.node, '>=24.0.0') ||
      satisfies(process.versions.node, '>=22.7.0');
    if (!supportsCPED) {
      this.skip();
    }

    const gcCheck = spawnSync(
      process.execPath,
      [
        '--expose-gc',
        '-e',
        "process.exit(typeof global.gc === 'function' ? 0 : 1)",
      ],
      {stdio: 'pipe'}
    );
    if (gcCheck.status !== 0) {
      this.skip();
    }

    const child = path.join(__dirname, 'cped-freelist-regression-child.js');
    const args = ['--expose-gc', '--max-old-space-size=4096'];
    if (
      satisfies(process.versions.node, '>=22.7.0') &&
      satisfies(process.versions.node, '<24.0.0')
    ) {
      args.push('--experimental-async-context-frame');
    }
    args.push(child);
    const res = spawnSync(process.execPath, args, {
      stdio: 'inherit',
    });

    // If the child process exits non-zero, fail with a helpful message.
    assert.strictEqual(res.error, undefined);
    assert.strictEqual(res.status, 0, `child exited with status ${res.status}`);
  });
});
