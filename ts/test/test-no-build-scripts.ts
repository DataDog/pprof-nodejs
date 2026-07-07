import * as assert from 'assert';
import * as fs from 'fs';
import * as path from 'path';

describe('package manifest', () => {
  const manifest = path.join(__dirname, '..', '..', 'package.json');
  const pkg = JSON.parse(fs.readFileSync(manifest, 'utf8'));

  it('declares no npm build lifecycle scripts (Yarn Berry YN0007)', () => {
    const scripts = pkg.scripts || {};
    const hooks = ['preinstall', 'install', 'postinstall'];
    const present = hooks.filter(name => scripts[name] !== undefined);
    assert.deepStrictEqual(present, []);
  });

  it('opts out of node-gyp on install via gypfile:false (npm implicit rebuild)', () => {
    // binding.gyp lives in the dev tree (excluded from the published tarball).
    // Without gypfile:false, npm's publish-time normalization synthesizes
    // "gypfile": true and "install": "node-gyp rebuild" into the registry
    // manifest, so consumers run node-gyp rebuild against a missing
    // binding.gyp and the install fails. Pinning gypfile:false suppresses
    // that hook while keeping the manifest free of lifecycle scripts.
    assert.strictEqual(pkg.gypfile, false);
  });
});
