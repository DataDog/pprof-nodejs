import * as assert from 'assert';
import * as fs from 'fs';
import * as path from 'path';

describe('package manifest', () => {
  it('declares no npm build lifecycle scripts (Yarn Berry YN0007)', () => {
    const manifest = path.join(__dirname, '..', '..', 'package.json');
    const pkg = JSON.parse(fs.readFileSync(manifest, 'utf8'));
    const scripts = pkg.scripts || {};
    const hooks = ['preinstall', 'install', 'postinstall'];
    const present = hooks.filter(name => scripts[name] !== undefined);
    assert.deepStrictEqual(present, []);
  });
});
