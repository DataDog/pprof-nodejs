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

## Storing the Sample Context in CPED
Node.js – as the embedder of V8 – commandeers the CPED to store instances of
ACF in it. This means that our profiler can't directly store our sample context
in the CPED, because then we'd overwrite the ACF reference already in there and
break Node.js. Fortunately, since ACF is "just" an ordinary JavaScript object,
we can define a new property on it, and store our sample context in it!
JavaScript properties can have strings, numbers, or symbols as their keys, with
symbols being the recommended practice to define properties that are hidden from
unrelated code as symbols are private to their creator and only compare equal to
themselves. Thus we create a private symbol in the profiler instance for our
property key, and our logic for storing the sample context thus becomes:
* get the CPED from the V8 isolate
* if it is not an object, do nothing (we can't set the sample context)
* otherwise set the sample context as a value in the object with our property
  key.

The reality is a bit thornier, though. Imagine what happens if while we're
setting the property, we get interrupted by a PROF signal and the signal handler
tries to read the property value? It could easily observe an inconsistent state
and crash. But even if it reads a property value, which one did it read? Still
the old one, already the new one, or maybe a torn value between the two?

Fortunately, we had the exact same problem with our previous approach where we
only stored one sample context in the profiler instances, and the solution is
the same. We encapsulate the pair of shared pointers to a V8 `Global` and an
atomic pointer-to-pointer in a class named `AtomicContextPtr`, which looks like
this:
```
using ContextPtr = std::shared_ptr<v8::Global<v8::Value>>;

class AtomicContextPtr {
  ContextPtr ptr1;
  ContextPtr ptr2;
  std::atomic<ContextPtr*> currentPtr = &ptr1;
  ...
```
A `Set` method on this class will first store the newly passed sample context in
either `ptr1` or `ptr2` – whichever `currentPtr` is _not_ pointing to at the
moment. Subsequently it atomically updates `currentPtr` to now point to it.

Instead of storing the current sample context in the ACF property directly,
we want to store an `AtomicContextPtr` (ACP.) The only problem? This is a C++
class, and properties of JavaScript objects can only be JavaScript values.
Fortunately, V8 gives us a solution for this as well: the `v8::External` type is
a V8 value type that wraps a `void *`.
So now the algorithm for setting a sample context is:
* get the CPED from the V8 isolate
* if it is not an object, do nothing (we can't set the sample context)
* Retrieve the property value. If there is one, it's the `External` wrapping the
  pointer to the ACP we use.
* If there is none, allocate a new ACP on C++ heap, create a `v8::External` to
  hold its pointer, and store it as a property in the ACF.
* Set the sample context as a value on the either retrieved or created ACP.

The chain of data now looks something like this:
```
v8::Isolate (from Isolate::GetCurrent())
 +-> current continuation (internally managed by V8)
   +-> node::AsyncContextFrame (in continuation's CPED field)
    +-> v8::External (in AsyncContextFrame's private property)
     +-> dd::AsyncContextPtr (in External's data field)
      +-> std::shared_ptr<v8::Global<v8::Value>> (in either AsyncContextPtr::ptr1 or ptr2)
       +-> v8::Global (in shared_ptr)
        +-> v8::Value (the actual sample context object)
```
The last 3-4 steps were the same in the previous code version as well, except
`ptr1` and `ptr2` were directly represented in the `WallProfiler`, so then it
looked like this:
```
dd::WallProfiler
 +-> std::shared_ptr<v8::Global<v8::Value>> (in either WallProfiler::ptr1 or ptr2)
  +-> v8::Global (in shared_ptr)
   +-> v8::Value (the actual sample context object)
```
The difference between the two diagrams shows how we encapsulated the
`(ptr1, ptr2, currentPtr)` tuple into a separate class and moved it out from
being an instance state of `WallProfiler` to being a property of every ACF we
encounter.

## Odds and ends
And that's mostly it! There are few more small odds and ends to make it work
safely. We still need to guard writing the property value to the ACF against
concurrent access by the signal handler, but now it happens only once for every
ACF, when we create its ACP. We guard by introducing an atomic boolean and
proper signal fencing.

The signal handler code also needs to be prevented from trying to access the
data while a GC is in progress. With this new model, the signal handler
unfortunately needs to do a small number of V8 API invocations. It needs to
retrieve the current V8 `Context`, it needs to obtain a `Local` for the property
key, and finally it needs to use both in an `Object::Get` call on the CPED.
Calling a property getter on an object is reentrancy into V8, which is advised
against, but this being an ordinary property it ends up being a single dependent
load, which turns out to work safely… unless there's GC happening. For this
reason, we register GC prologue and epilogue callbacks with the V8 isolate so we
can know when GCs are ongoing and the signal handler will refrain from touching
CPED during them. We'll however grab the current sample context from the CPED
and store it in a profiler instance field in the GC prologue and use it for any
samples taken during GC.

Speaking of GC, we can now have an unbounded number of ACPs – one for each live
ACF. Each ACP is allocated on the C++ heap, and needs to be deleted eventually.
The profiler tracks every ACP it creates in an internal set of live ACPs and
deletes them all when it itself gets disposed. This would still allow for
unbounded growth so we additionally register a V8 GC finalization callback for
every ACF. When V8 collects an ACF instance its finalization callback will put
that ACF's ACP into the profiler's internal vector of ready-to-delete ACPs and
the profiler processes that vector (both deletes the ACP and removes it from the
live set) on each call to `SetContext`.

## Changes in dd-trace-js
For completeness, we'll describe the changes in dd-trace-js here as well. The
main change is that with Node 24, we no longer require async hooks. The
instrumentation points for `AsyncLocalStorage.enterWith` and
`AsyncLocalStorage.run` remain in place – they are the only ones that are needed
now.

There are some small performance optimizations that no longer apply with the new
approach, though. For one, with the old approach we did some data conversions
(span IDs to string, a tag array to endpoint string) in a sample when a sample
was captured. With the new approach, we do these conversions for all sample
contexts during profile serialization. Doing them after each sample capture
amortized their cost possibly minimally reducing the latency induced at
serialization time. With the old approach we also called `SetContext` only once
per sampling – we'd install a sample context to be used for the next sample, and
then kept updating a `ref` field in it with a reference to the actual data.
Since we no longer have a single sample context (but one per continuation) we
can not do this anymore, and we need to call `SetContext` on every ACF change.
The cost of this (basically, going into a native call from JavaScript) are still
well offset by not having to use async hooks and do work on every async context
change. We could arguably simplify the code by removing those small
optimizations.
