const pprof = require('./out/src');
const {AsyncLocalStorage} = require('async_hooks');
const AsyncContextFrame = require('internal/async_context_frame');

pprof.time.start({
  intervalMicros: 100,
  durationMillis: 60000,
  withContexts: true,
  lineNumbers: false,
  workaroundV8Bug: false,
  collectCpuTime: false,
  collectAsyncId: false,
  useCPED: true,
});

const als = new AsyncLocalStorage();

function bar() {
  console.log(AsyncContextFrame.current());
}

while (true) {
  als.run(123, bar);
}
