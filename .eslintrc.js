'use strict';

module.exports = {
  extends: ['./node_modules/gts'],
  ignorePatterns: [
    '**/node_modules',
    '**/coverage',
    'build/**',
    'proto/**',
    'out/**',
    'benchmark/**',
    'scripts/**',
    'system-test/**',
    'test.ts',
  ],
};
