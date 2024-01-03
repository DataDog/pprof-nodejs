const fs = require('fs')
const { execSync } = require('child_process')

execSync('npm run compile', { stdio: 'inherit' })

// if prebuilds already exist, do not compile native addon
if (fs.existsSync('./prebuilds')) {
  process.exit(0)
}

if (process.env.PREBUILD === '1') {
  execSync('npm run prebuild', { stdio: 'inherit' })
} else {
  execSync('npm run rebuild', { stdio: 'inherit' })
}
