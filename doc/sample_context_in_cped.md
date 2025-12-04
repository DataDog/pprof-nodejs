# Storing Sample Context in V8 Continuation-Preserved Embedder Data

## What is the Sample Context?
Datadog's Node.js profiler has the ability to store a custom object that it will
then associate with collected CPU samples. We refer to this object as the
"sample context." A higher-level embedding (typically, dd-trace-js) will then
update the sample context to keep it current with changes in the execution. A
typical piece of data sample context stores is the tracing span ID, so whenever
it changes, the sample context needs to be updated.

## How is the Sample Context stored and updated?
Before Node 22.7, the sample context would be stored in a
`std::shared_ptr<v8::Global<v8::Value>>` field on the C++ `WallProfiler`
instance. Due to it being a single piece of instance state, it had to be updated
every time the active span changed, possibly on every invocation of
`AsyncLocalStorage.enterWith` and `.run`, but even more importantly on every
async context change, and for that we needed to register a "before" callback
with `async_hooks.createHook`. This meant that we needed to both update the
sample context on every async context change, but more importantly it also meant
we needed to use `async_hooks.createHook` which is getting deprecated in Node.
Current documentation for it is not exactly a shining endorsement:
> Please migrate away from this API, if you can. We do not recommend using the
> createHook, AsyncHook, and executionAsyncResource APIs as they have usability
> issues, safety risks, and performance implications.

Fortunately, first the V8 engine and then Node.js gave us building blocks for a
better solution.

## V8 Continuation-Preserved Embedder Data and Node.js Async Context Frame
In the V8 engine starting from version 12 (the one shipping with Node 22)
`v8::Isolate` exposes an API to set and get embedder-specific data on it so that
it is preserved across executions that are logical continuations of each other
(essentially: across promise chains; this includes await expressions.) Even
though the APIs are exposed on the isolate, the data is stored on a
per-continuation basis and the engine takes care to return the right one when
`Isolate::GetContinuationPreservedEmbedderData()` method is invoked. We will
refer to continuation-preserved embedder data as "CPED" from now on.

Starting with Node.js 22.7, CPED is used to implement data storage behind
Node.js `AsyncLocalStorage` API. This dovetails nicely with our needs as all the
span-related data we set on the sample context is normally managed in an async
local storage (ALS) by the tracer. An application can create any number of
ALSes, and each ALS manages a single value per async context. This value is
somewhat confusingly called the "store" of the async local storage, making it
important to not confuse the terms "storage" (an identity with multiple values,
one per async context) and "store", which is a value of a storage within a
particular async context.

The new implementation for storing ALS stores introduces an internal Node.js
class named `AsyncContextFrame` (ACF) which is a subclass of JavaScript Map
class that uses ALSes as keys and their stores as the map values, essentially
providing a mapping from an ALS to its store in the current async context. (This
implementation is very similar to how e.g. Java implements `ThreadLocal`, which
is a close analogue to ALS in Node.js.) ACF instances are then stored in CPED.

## Storing the Sample Context in CPED
Node.js – as the embedder of V8 – commandeers the CPED to store instances of
ACF in it. This means that our profiler can't directly store our sample context
in the CPED, because then we'd overwrite the ACF reference already in there and
break Node.js. Fortunately, since ACF is "just" an ordinary JavaScript Map,
we can store our sample context in it as a key-value pair! When a new ACF is
created (normally, through `AsyncLocalStorage.enterWith`), all key-value pairs
are copied into the new map, so our sample context is nicely propagated.
Our logic for storing the sample context thus becomes:
* get the CPED from the V8 isolate
* if it is not a Map, do nothing (we can't set the sample context)
* otherwise set the sample context as a value in the map with our key.

It's worth noting that our key is just an ordinary empty JavaScript object
created internally by the profiler. We could've also passed it an externally
created `AsyncLocalStorage` instance, thus preserving the invariant that all
keys in an ACF are ALS instances, but this doesn't seem necessary.

We use a mutex implemented as an atomic boolean to guard our writes to the map.
The JavaScript code for AsyncContextFrame/AsyncLocalStorage treats the maps as

Internally, we hold on to the sample context value with a shared pointer to a
V8 `Global`:
```
using ContextPtr = std::shared_ptr<v8::Global<v8::Value>>;
```

The values we store in ACF need to be JavaScript values. We use Node.js
`WrapObject` class for this purpose – it allows defining C++ classes that have
a JavaScript "mirror" object, carry a pointer to their C++ object in an internal
field, and when the JS object is garbage collected, the C++ object is destroyed.
Our `WrapObject` subclass in named `PersistentContextPtr` (PCP) because it has
only one field – the above introduced `ContextPtr`, and it is "persistent"
because its lifecycle is bound to that of its representative JavaScript object.

So the more detailed algorithm for setting a sample context is:
* get the CPED from the V8 isolate
* if it is not a Map, do nothing (we can't set the sample context)
* if sample context is undefined, delete the key (if it exists) from the map
* if sample context is a different value, create a new `PersistentContextPtr`
  wrapped in a JS object, and set the JS object as the value with the key in the
  map.

The chain of data now looks something like this:
```
v8::Isolate (from Isolate::GetCurrent())
 +-> current continuation (internally managed by V8)
   +-> node::AsyncContextFrame (in continuation's CPED field)
    +-> Object (the PersistentContextPtr wrapper, associated with our key)
     +-> dd::PersistentContextPtr (pointed in Object's internal field)
      +-> ContextPtr (in `context` field)
       +-> v8::Global (in shared_ptr)
        +-> v8::Value (the actual sample context object)
```
The last 3-4 steps were the same in the previous code version as well, except
there we used a field directly in the `WallProfiler`:
```
dd::WallProfiler
 +-> ContextPtr (in `curContext_` field)
  +-> v8::Global (in shared_ptr)
   +-> v8::Value (the actual sample context object)
```
The difference between the two diagrams shows how we moved the ContextPtr from
being a single instance state of `WallProfiler` to being an element in ACF maps.

## Looking up values in a signal handler
The signal handler unfortunately can't directly call any V8 APIs, so in order to
traverse the chain of data above, it needs to rely on pointer arithmetic. Every
`Global` and `Local` have one field, and `Address*`. Thus, to dereference the
actual memory location of a JS object represented by a global reference `ref`,
we use `**<reinterpret_cast>(Address**)(&ref)`. These addresses are _tagged_,
meaning their LSB is set to 1, and need to be masked to obtain the actual memory
address. We can safely get the current Isolate pointer, but then we need to
interpret as an address the memory location at an internal offset where it keeps
the current CPED. If it's a JS Map, then we need to do more dead-reckoning into
it and retrieve a pointer to its OrderedHashMap, and then know its memory layout
to find the right hash bucket and traverse the linked list until we find a
key-value pair where the key address is our key object's current address (this
can be moved around by the GC, so that's why our Global is an `Address*`, for
a sufficient number of indirections to keep up with the moves.) The algorithm
for executing an equivalent of a `Map.get()` purely using pointer arithmetic
with knowledge of the V8 object memory layouts is encapsualted in `map-get.cc`.

## Odds and ends
And that's mostly it! There are few more small odds and ends to make it work
safely. As we mentioned above, we're preventing the signal handler from reading
if we're just writing the value using an atomic boolean. We also register GC
prologue and epilogue callbacks with the V8 isolate so we can know when GCs are
ongoing and the signal handler will also refrain from touching memory while a GC
runs. We'll however grab the current sample context from the CPED
and store it in a profiler instance field in the GC prologue and use it for any
samples taken during GC.

Speaking of GC, we can now have an unbounded number of PersistentContextPtr
objects – one for each live ACF. Each PCP is allocated on the C++ heap, and
needs to be deleted eventually. The profiler tracks every PCP it creates in an
internal set of live PCPs and deletes them all when it itself gets disposed.
This is combined with `WrapObject` having GC finalization callback for every
PCP. When V8 collects a PCP wrapper its finalization callback will delete the
PCP.

## Changes in dd-trace-js
For completeness, we'll describe the changes in dd-trace-js here as well. The
main change is that with Node 24, we no longer require async hooks. The
instrumentation point for `AsyncLocalStorage.enterWith` is the only one
remaining (`AsyncLocalStorage.run` is implemented in terms of `enterWith`.)
We can further optimize and _not_ set the sample context object if we see it's
the same as the current one (because `enterWith` was run without setting a new
span as the current span.)

There are some small performance optimizations that no longer apply with the new
approach, though. For one, with the old approach we did some data conversions
(span IDs to string, a tag array to endpoint string) in a sample context when a
sample was captured. With the new approach, we do these conversions for all
sample contexts during profile serialization. Doing them after each sample
capture amortized their cost possibly minimally reducing the latency induced at
serialization time. With the old approach we also called `SetContext` only once
per sampling – we'd install a sample context to be used for the next sample, and
then kept updating a `ref` field in it with a reference to the actual data.
Since we no longer have a single sample context (but rather one per
continuation) we can not do this anymore, and we need to call `SetContext`
either every time `enterWith` runs, or only when we notice that the relevant
span data changed.
The cost of this (basically, going into a native call from JavaScript) are still
well offset by not having to use async hooks and do work on every async context
change. We could arguably even simplify the code by removing those small
optimizations.
