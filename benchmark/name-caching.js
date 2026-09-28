'use strict'

const { Script } = require('vm')
const { isMainThread } = require('worker_threads')
const { TimeProfiler } = require('../out/src/time-profiler-bindings')

const scriptCount = Number(process.env.SCRIPT_COUNT || '500')
const functionsPerScript = Number(process.env.FUNCTIONS_PER_SCRIPT || '8')
const rounds = Number(process.env.ROUNDS || '8')
const work = Number(process.env.WORK || '1000')
const lines = Number(process.env.LINES || '32')
const iterations = Number(process.env.ITERATIONS || '9')
const warmup = Number(process.env.WARMUP || '3')

function compileScript (scriptIndex) {
  const handlers = []
  const calls = []

  for (let i = 0; i < functionsPerScript; i++) {
    const name = `handler_${scriptIndex}_${i}`
    const statements = Array.from({ length: lines }, (_, line) =>
      `for (let k = 0; k < ${work}; k++) total += Math.sqrt(k * n + ${i + line})`
    )
    handlers.push(`
      function ${name}(n) {
        let total = 0
        ${statements.join('\n')}
        return total
      }`)
    calls.push(`total += ${name}(n + ${i})`)
  }

  return new Script(`
    (() => {
      ${handlers.join('\n')}
      return function run_${scriptIndex}(n) {
        let total = 0
        ${calls.join('\n')}
        return total
      }
    })()
  `, { filename: `/opt/service/dist/modules/module-${scriptIndex}.js` })
    .runInThisContext()
}

function countNodes (node) {
  let count = 1
  for (const child of node.children) count += countNodes(child)
  return count
}

function median (values) {
  return [...values].sort((a, b) => a - b)[Math.floor(values.length / 2)]
}

const scripts = Array.from({ length: scriptCount }, (_, i) => compileScript(i))
const stopMicros = []
const nanosPerNode = []
const nodeCounts = []

for (let iteration = 0; iteration < warmup + iterations; iteration++) {
  const profiler = new TimeProfiler({
    intervalMicros: 50,
    durationMillis: 60000,
    lineNumbers: true,
    withContexts: false,
    workaroundV8Bug: false,
    collectCpuTime: false,
    collectAsyncId: false,
    isMainThread,
    useCPED: false
  })
  profiler.start()

  let result = 0
  for (let round = 0; round < rounds; round++) {
    for (const run of scripts) result += run(round + 1)
  }

  const start = process.hrtime.bigint()
  const profile = profiler.stop(false)
  const elapsed = Number(process.hrtime.bigint() - start)
  profiler.dispose()

  if (!Number.isFinite(result)) throw new Error('benchmark workload failed')
  if (iteration < warmup) continue

  const nodes = countNodes(profile.topDownRoot)
  stopMicros.push(elapsed / 1000)
  nanosPerNode.push(elapsed / nodes)
  nodeCounts.push(nodes)
}

console.log(JSON.stringify({
  name: 'profile-name-caching',
  scripts: scriptCount,
  functionsPerScript,
  lines,
  iterations,
  medianNodes: median(nodeCounts),
  medianStopMicros: median(stopMicros),
  medianNanosPerNode: median(nanosPerNode)
}))
