'use strict'

const { default: CpuProfiler } = require('../../../out/src/cpu-profiler')

let profiler
if (process.env.ENABLE_PROFILER) {
  profiler = new CpuProfiler()
  profiler.start(999)
}

setTimeout(() => {
  if (profiler) {
    const profile = profiler.profile()
    profiler.stop()
  }
}, 1000)
