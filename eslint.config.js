'use strict';
const gts = require('./node_modules/gts');

module.exports = [
  {
    ignores: [
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
  },
  ...gts,
];
