import fs from 'fs';

// monitorOutOfMemory appends the profile path after the export command.
function check(): string {
  const profile = fs.readFileSync(process.argv[2], 'utf8');
  JSON.parse(profile);
  const counts = [...profile.matchAll(/"count":(\d+)/g)];
  return counts.some(match => Number(match[1]) > 0)
    ? 'ok'
    : 'ko: exported profile carried no allocations';
}

let result: string;
try {
  result = check();
} catch (err) {
  result = `ko: ${err}`;
}
fs.writeFileSync('oom_check.log', result);
