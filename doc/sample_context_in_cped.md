# Storing Sample Context in V8 Continuation-Preserved Embedder Data

## What is the Sample Context?
Datadog's Node.js profiler has the ability to store a custom object that it will
then associate with collected CPU samples. We refer to this object as the
"sample context." A higher-level embedding (typically, dd-trace-js) will then
update the sample context to keep it current with changes in the execution. A
typical piece of data sample context stores is the tracing span ID, so whenever
it changes, the sample context needs to be updated.

## How is the Sample Context stored and updated?
Before Node 23, the sample context would be stored in a
`std::shared_ptr<v8::Global<v8::Value>>` field on the C++ `WallProfiler`
instance. (In fact, due to the need for ensuring atomic updates and shared
pointers not being effectively updateable atomically it's actually a pair of
fields with an atomic pointer-to-shared-pointer switching between them, but I
digress.) Due to it being a single piece of instance state, it had to be updated
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

Starting with Node.js 23, CPED is used to implement data storage behind Node.js
`AsyncLocalStorage` API. This dovetails nicely with our needs as all the
span-related data we set on the sample context is normally managed in an async
local storage (ALS) by the tracer. An application can create any number of
ALSes, and each ALS manages a single value per async context. This value is
somewhat confusingly called the "store" of the async local storage, making it
important to not confuse the terms "storage" (an identity with multiple values,
one per async context) and "store", which is a value of a storage within a
particular async context.

The new implementation for storing ALS stores introduces an internal Node.js
class named `AsyncContextFrame` (ACF) which is a map that uses ALSes as keys,
and their stores as the map values, essentially providing a mapping from an ALS
to its store in the current async context. (This implementation is very similar
to how e.g. Java implements `ThreadLocal`, which is a close analogue to ALS in
Node.js.) ACF instances are then stored in CPED.

## Storing the Sample Context in CPED, take one
Node.js – as the embedder of V8 – commandeers the CPED to store instances of
ACF in it. This means that our profiler can't directly store our sample context
in the CPED, because then we'd overwrite the ACF reference already in there and
break Node.js. Our first attempt at solving this was to –- since ACF is "just"
an ordinary JavaScript object -- to define a new property on it, and store our
sample context in it! JavaScript properties can have strings, numbers, or
symbols as their keys, with symbols being the recommended practice to define
properties that are hidden from unrelated code as symbols are private to their
creator and only compare equal to themselves. Thus we created a private symbol in
the profiler instance for our property key, and our logic for storing the sample
context thus becomes:
* get the CPED from the V8 isolate
* if it is not an object, do nothing (we can't set the sample context)
* otherwise set the sample context as a value in the object with our property
  key.

Unfortunately, this approach is not signal safe. When we want to read the value
in the signal handler, it now needs to retrieve the CPED, which creates a V8
`Local<Value>`, and then it needs to read a property on it, which creates
another `Local`. It also needs to retrieve the current context, and a `Local`
for the symbol used as a key – four `Local`s in total. V8 tracks the object
addresses pointed to by locals so that GC doesn't touch them. It tracks them in
a series of arrays, and if the current array fills up, it needs to allocate a
new one. As we know, allocation is unsafe in a signal handler, hence our
problem. We were thinking of a solution where we check if there is at least 4
slots free in the current array, but then our profiler's operation would be at
mercy of V8 internal state.

## Storing the Sample Context in CPED, take two

Next we thought of replacing the `AsyncContextFrame` object in CPED with one we
created with an internal field – we can store and retrieve an arbitrary `void *`
in it with `{Get|Set}AlignedPointerInInternalField` methods. The initial idea
was to leverage JavaScript's property of being a prototype-based language and
set the original CPED object as the prototype of our replacement, so that all
its methods would keep being invoked. This unfortunately didn't work because
the `AsyncContextFrame` is a `Map` and our replacement object doesn't have the
internal structure of V8's implementation of a map. The final solution turned
out to be the one where we store the original ACF as a property in our
replacement object (now effectively, a proxy to the ACF), and define all the
`Map` methods and properties on the proxy so that they are invoked on the ACF.
Even though the proxy does not pass an `instanceof Map` check, it is duck-typed
as a map. We even encapsulated this behavior in a special prototype object, so
the operations to set the context are:
* retrieve the ACF from CPED
* create a new object (the proxy) with one internal field
* set the ACF as a special property in the proxy
* set the prototype of the proxy to our prototype that defines all the proxied
methods and properties to forward through the proxy-referenced ACF.
* store our sample context in the internal field of the proxy
* set the proxy object as the CPED.

Now, a keen eyed reader will notice that in the signal handler we still need to
call `Isolate::GetContinuationPreservedEmbedderData` which still creates a
`Local`. That would be true, except that we can import the `v8-internals.h`
header and directly read the address of the object by reading into the isolate
at the offset `kContinuationPreservedEmbedderDataOffset` declared in it.


The chain of data now looks something like this:
```
v8::Isolate (from Isolate::GetCurrent())
 +-> current continuation (internally managed by V8)
   +-- our proxy object
     +-- node::AsyncContextFrame (in proxy's private property, for forwarding method calls)
     +-- prototype: declares functions and properties that forward to the AsyncContextFrame
     +-- dd:PersistentContextPtr* (in proxy's internal field)
       +-> std::shared_ptr<v8::Global<v8::Value>> (in PersistentContextPtr's context field)
         +-> v8::Global (in shared_ptr)
          +-> v8::Value (the actual sample context object)

```
The last 3 steps are the same as when CPED is not being used, except `context`
is directly represented in the `WallProfiler`, so then it looks like this:
```
dd::WallProfiler
 +-> std::shared_ptr<v8::Global<v8::Value>> (in either WallProfiler::ptr1 or ptr2)
  +-> v8::Global (in shared_ptr)
   +-> v8::Value (the actual sample context object)
```

### Memory allocations and garbage collection
We need to allocate a `PersistentContextPtr` (PCP) instance for every proxy we
create. The PCP has two concerns: it both has a shared pointer to the V8 global
that carries the sample context, and it also has a V8 weak reference to the
proxy object it is encapsulated within. This allows us to detect (since weak
references allow for GC callbacks) when the proxy object gets garbage collected,
and at that time the PCP itself can be either deleted or reused. We have an
optimization where we don't delete PCPs -- the assumption is that the number of
live ACFs (and thus proxies, and thus PCPs) will be constant for a server
application under load, so instead of doing a high amount of small new/delete
operations that can fragment the native heap, we keep the ones we'd delete in a
dequeue instead and reuse them.

## Odds and ends
And that's mostly it! There are few more small odds and ends to make it work
safely. We still need to guard reading the value in the signal handler while
it's being written. We guard by introducing an atomic boolean and proper signal
fencing.

The signal handler code also needs to be prevented from trying to access the
data while GC is in progress. For this reason, we register GC prologue and
epilogue callbacks with the V8 isolate so we can know when GCs are ongoing and
the signal handler will refrain from reading the CPED field during them. We'll
however grab the current sample context from the CPED and store it in a profiler
instance field in the GC prologue and use it for any samples taken during GC.

## Changes in dd-trace-js
For completeness, we'll describe the changes in dd-trace-js here as well. The
main change is that with Node 24, we no longer require async hooks. The
instrumentation points for `AsyncLocalStorage.enterWith` and
`AsyncLocalStorage.run` remain in place – they are the only ones that are needed
now.

There are some small performance optimizations that no longer apply with the new
approach, though. For one, with the old approach we did some data conversions
(span IDs to bigint, a tag array to endpoint string) in a sample when a sample
was captured. With the new approach, we do these conversions for all sample
contexts during profile serialization. Doing them after each sample capture
amortized their cost, possibly reducing the latency induced at serialization
time. With the old approach we also called `SetContext` only once per sampling –
we'd install a sample context to be used for the next sample, and then kept
updating a `ref` field in it with a reference to the actual data from pure
JavaScript code. Since we no longer have a single sample context (but one per
continuation) we can not do this anymore, and we need to call `SetContext` on
every ACF change. The cost of this (basically, going into a native call from
JavaScript) are still well offset by not having to use async hooks and do work
on every async context change. We could arguably simplify the code by removing
those small optimizations.
