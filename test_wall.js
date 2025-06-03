const pprof = require('./out/src');
const {AsyncLocalStorage} = require('async_hooks');
const AsyncContextFrame = require('internal/async_context_frame');
const symbol = Symbol.for('dd::WallProfiler::cpedSymbol_');

pprof.time.start({
  intervalMicros: 1,
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
  pprof.time.setContext({foo: 'bar'});

  // pprof.time.setContext({foo: 'bar'});
  // console.log(AsyncContextFrame.current()[symbol]);
  // foo(10);
}

function foo(depth) {
  // console.log(AsyncContextFrame.current());
  const als = new AsyncLocalStorage();
  for (let i = 0; i < 10; i++) {
    const newAls = new AsyncLocalStorage();
    newAls.enterWith({foo: 'bar'});
  }
  if (depth > 0) {
    als.run(123 * depth, foo, depth - 1);
  }
}

als.run(123, () => {
  console.log(AsyncContextFrame.current());

  while (true)
    {
    als.run(456, bar);
    console.log(AsyncContextFrame.current());
  }
});

// // als.enterWith({foo: 'bar'});
// while (true) {
//   pprof.time.setContext({foo: 'bar'});
//   console.log(AsyncContextFrame.current()[symbol]);
// }