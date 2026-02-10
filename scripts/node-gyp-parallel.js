'use strict'

const { cpus, availableParallelism } = require('os')
const { spawnSync } = require('child_process')

function detectParallelism () {
  if (typeof availableParallelism === 'function') {
    return availableParallelism()
  }

  const cores = cpus()
  return Array.isArray(cores) && cores.length > 0 ? cores.length : 1
}

const command = process.platform === 'win32' ? 'node-gyp.cmd' : 'node-gyp'
const jobs = Math.max(detectParallelism(), 1)
const args = [...process.argv.slice(2), '--jobs', String(jobs)]

const child = spawnSync(command, args, { stdio: 'inherit' })

if (child.error) {
  throw child.error
}

process.exit(child.status ?? 1)
