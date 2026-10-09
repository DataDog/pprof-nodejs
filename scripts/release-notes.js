'use strict'

// Usage: node scripts/release-notes.js [version] [--dry-run]
//
// Publishes the release proposal PR's body as the GitHub release notes for
// that version, expanding bare pull request references into their titles.
//
// The proposal PR is resolved by its head branch, which keeps working after
// the branch is deleted on merge. `--latest` is hardcoded because v5.x is the
// only release line; once a second one ships it has to be decided per branch.

const { execFileSync } = require('child_process')
const { writeFileSync } = require('fs')
const { join } = require('path')
const { tmpdir } = require('os')

const owner = 'DataDog'
const name = 'pprof-nodejs'
const repo = `${owner}/${name}`

const args = process.argv.slice(2)
const dryRun = args.includes('--dry-run')
const version = args.find(arg => !arg.startsWith('--')) || require('../package.json').version
const tag = `v${version}`

function gh (params) {
  return execFileSync('gh', params, { encoding: 'utf8', maxBuffer: 16 * 1024 * 1024 }).trim()
}

function ghSucceeds (params) {
  try {
    gh(params)
    return true
  } catch {
    return false
  }
}

function readTitles (numbers) {
  const titles = new Map()
  if (numbers.length === 0) return titles

  const fields = numbers
    .map(number => `pr${number}: pullRequest(number: ${number}) { number title }`)
    .join(' ')
  const response = JSON.parse(gh([
    'api', 'graphql', '-f',
    `query=query { repository(owner: "${owner}", name: "${name}") { ${fields} } }`
  ]))

  if (response.errors && response.errors.length > 0) {
    throw new Error(`Pull request title lookup failed: ${response.errors[0].message}`)
  }

  for (const pullRequest of Object.values(response.data.repository)) {
    if (pullRequest) titles.set(String(pullRequest.number), pullRequest.title)
  }

  return titles
}

// Only a list item that is nothing but a reference is expanded, so bodies that
// already spell out their titles are left exactly as the author wrote them.
const bareReference = /^([ \t]*[-*][ \t]+)#(\d+)[ \t]*$/gm

const body = gh(['pr', 'view', `${tag}-proposal`, '--repo', repo, '--json', 'body', '--jq', '.body'])
  .replace(/\r\n/g, '\n')
const numbers = [...new Set([...body.matchAll(bareReference)].map(match => match[2]))]
const titles = readTitles(numbers)
const notes = body.replace(bareReference, (line, bullet, number) => {
  const title = titles.get(number)
  return title ? `${bullet}${title} (#${number})` : line
})

if (dryRun) {
  console.log(notes)
  process.exit(0)
}

const file = join(process.env.RUNNER_TEMP || tmpdir(), `${tag}.md`)
writeFileSync(file, notes)

const exists = ghSucceeds(['release', 'view', tag, '--repo', repo, '--json', 'tagName'])
const prerelease = version.includes('-')

// --verify-tag keeps a lagging tag push from silently creating the tag on the
// default branch instead of on the release commit.
const params = [
  'release', exists ? 'edit' : 'create', tag,
  '--repo', repo,
  '--verify-tag',
  '--title', version,
  '--notes-file', file,
  prerelease ? '--latest=false' : '--latest'
]

if (prerelease) params.push('--prerelease')

console.log(gh(params))
