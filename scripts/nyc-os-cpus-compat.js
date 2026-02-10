'use strict';

const os = require('os');

const originalCpus = os.cpus.bind(os);

/**
 * Some environments report an empty CPU list, which makes nyc derive a
 * zero concurrency and crash. Keep Node's original result when present and
 * synthesize a minimum-safe fallback only for the empty-list case.
 */
os.cpus = function patchedCpus() {
  const cpus = originalCpus();
  if (Array.isArray(cpus) && cpus.length > 0) {
    return cpus;
  }

  const fallbackCount = Math.max(
    typeof os.availableParallelism === 'function'
      ? os.availableParallelism()
      : 1,
    1
  );

  return Array.from({length: fallbackCount}, () => ({
    model: 'unknown',
    speed: 0,
    times: {
      user: 0,
      nice: 0,
      sys: 0,
      idle: 0,
      irq: 0,
    },
  }));
};
