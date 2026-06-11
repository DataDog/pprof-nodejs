/*
 * Copyright 2026 Datadog, Inc
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// Node.js writer for the OTEP-4947 Thread Local Context Record, adapted for
// the Node.js asynchronous context model. The record is wrapped in a JS
// object (CtxWrap) and stored in an AsyncLocalStorage instance; an
// out-of-process reader discovers it by walking the V8 isolate's
// ContinuationPreservedEmbedderData to the AsyncContextFrame (a JS Map),
// looking up the ALS instance as the key, reading the resulting CtxWrap,
// and finally the record it owns.

#include "otel-thread-ctx.hh"

#include <node.h>
#include <node_object_wrap.h>
#include <v8-internal.h>

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <atomic>
#include <memory>
#include <vector>

// Single thread-local read from outside the process via TLSDESC. It
// identifies, for the current V8 isolate's thread:
//
//  - the address of the isolate's ContinuationPreservedEmbedderData slot
//    (`cped_slot`), whose value V8 swaps as it switches between
//    continuations. Reading `*cped_slot` yields the active
//    AsyncContextFrame; no V8 internal symbol lookup is required on the
//    reader side.
//  - the AsyncLocalStorage instance the reader must look up inside that
//    AsyncContextFrame map (`als_handle`),
//  - that instance's JS identity hash (`als_identity_hash`), so the
//    reader can restrict the lookup to a single hash bucket.
//  - the (per-isolate) tagged address of the `undefined` singleton
//    (`undefined_addr`). After looking up the value for our ALS key in
//    the ACF map, the reader can compare against this to skip the
//    JSObject / internal-field-0 dereference when no CtxWrap is
//    currently attached.
//
// Layout is part of the reader ABI: see static_asserts below.
extern "C" {
using v8::Global;
using v8::Object;

struct otel_thread_ctx_nodejs_v1_t {
  v8::internal::Address *cped_slot;      // offset 0
  Global<Object> als_handle;             // offset sizeof(void*); 1 V8 ptr
  int als_identity_hash;                 // offset 2 * sizeof(void*); 4 + 4 pad
  v8::internal::Address undefined_addr;  // offset 3 * sizeof(void*); tagged
};

__attribute__((visibility("default")))
thread_local otel_thread_ctx_nodejs_v1_t otel_thread_ctx_nodejs_v1;
}

static_assert(sizeof(v8::Global<v8::Object>) == sizeof(void *),
              "Global<Object> must be exactly one pointer wide");
static_assert(offsetof(otel_thread_ctx_nodejs_v1_t, cped_slot) == 0,
              "cped_slot must be at offset 0");
static_assert(offsetof(otel_thread_ctx_nodejs_v1_t, als_handle) ==
                  sizeof(void *),
              "als_handle must immediately follow cped_slot");
static_assert(offsetof(otel_thread_ctx_nodejs_v1_t, als_identity_hash) ==
                  2 * sizeof(void *),
              "als_identity_hash must immediately follow als_handle");
static_assert(offsetof(otel_thread_ctx_nodejs_v1_t, undefined_addr) ==
                  3 * sizeof(void *),
              "undefined_addr must follow als_identity_hash + padding");

namespace dd {
namespace {

using node::ObjectWrap;
using v8::Array;
using v8::Context;
using v8::Function;
using v8::FunctionCallbackInfo;
using v8::FunctionTemplate;
using v8::Global;
using v8::Integer;
using v8::Isolate;
using v8::Local;
using v8::Object;
using v8::String;
using v8::Uint8Array;
using v8::Value;

// OTEP-4947 record. The trailing `attrs_data` is a C99 flexible array
// member: the writer allocates one contiguous block of size
// `sizeof(OtelThreadCtxRecord) + attrs_data_size`, and the FAM gives the
// reader of this struct definition the right intuition — "there's
// variable-length data after the header" — while sizeof / offsetof still
// see only the 28-byte header. Field offsets are statically verified.
struct OtelThreadCtxRecord {
  uint8_t trace_id[16];      // offset 0
  uint8_t span_id[8];        // offset 16
  uint8_t valid;             // offset 24
  uint8_t reserved;          // offset 25
  uint16_t attrs_data_size;  // offset 26
  uint8_t attrs_data[];      // offset 28; length is attrs_data_size
};
static_assert(sizeof(OtelThreadCtxRecord) == 28,
              "OTEP thread-ctx header must be exactly 28 bytes");
static_assert(offsetof(OtelThreadCtxRecord, trace_id) == 0, "trace_id offset");
static_assert(offsetof(OtelThreadCtxRecord, span_id) == 16, "span_id offset");
static_assert(offsetof(OtelThreadCtxRecord, valid) == 24, "valid offset");
static_assert(offsetof(OtelThreadCtxRecord, reserved) == 25, "reserved offset");
static_assert(offsetof(OtelThreadCtxRecord, attrs_data_size) == 26,
              "attrs_data_size offset");
static_assert(offsetof(OtelThreadCtxRecord, attrs_data) == 28,
              "attrs_data offset");

struct OtelThreadCtxRecordDeleter {
  void operator()(OtelThreadCtxRecord *p) const noexcept { free(p); }
};
using OwnedRecord =
    std::unique_ptr<OtelThreadCtxRecord, OtelThreadCtxRecordDeleter>;

// Floor on the attrs_data capacity of a freshly allocated record. Sized so
// the total allocation is one 64-byte cache line — matching the OTEP-4947
// "frugal writer" guidance — and giving small records some slack so the
// first few appends (if any) can be in-place.
constexpr size_t MIN_INITIAL_CAPACITY = 64 - sizeof(OtelThreadCtxRecord);

// Upper bound on the attribute payload. Sized so the total record stays
// under the OTEP-4947 recommended 640 bytes, which is the read-buffer
// ceiling for typical eBPF readers. Attributes that would push past this
// are silently dropped (with `truncated_` set on the wrapper) rather than
// the writer throwing — the OTEP treats the cap as best-effort.
constexpr size_t MAX_ATTRS_DATA_SIZE = 640 - sizeof(OtelThreadCtxRecord);

// Wraps a heap-allocated OtelThreadCtxRecord. Lifetime is managed by V8
// GC: when no JS code (or AsyncLocalStorage entry) holds a reference, the
// record is freed.
//
// Layout note for the reader: `record_` is private to C++ but its byte
// position within CtxWrap is part of the reader contract. It is the first
// field after the node::ObjectWrap base subobject. `capacity_` and
// `truncated_` sit after `record_` purely for the writer's own
// bookkeeping — the reader never touches them.
class CtxWrap : public ObjectWrap {
 public:
  ~CtxWrap() override;
  static void Init(Local<Object> exports);

  CtxWrap(const CtxWrap &) = delete;
  CtxWrap &operator=(const CtxWrap &) = delete;
  CtxWrap(CtxWrap &&) = delete;
  CtxWrap &operator=(CtxWrap &&) noexcept = delete;

 private:
  static void New(const FunctionCallbackInfo<Value> &args);
  static void DebugBytes(const FunctionCallbackInfo<Value> &args);
  static void Append(const FunctionCallbackInfo<Value> &args);
  static void IsTruncated(const FunctionCallbackInfo<Value> &args);

  static bool EncodeAttrs(Isolate *isolate, Local<Context> context,
                          Local<Value> attrs_val, size_t existing_size,
                          std::vector<uint8_t> *out, bool *out_truncated);

  CtxWrap(OtelThreadCtxRecord *record, size_t capacity, bool truncated);

  // The three fields are kept in one access section because C++ leaves
  // the relative layout of fields in different access controls
  // implementation-defined. `record_` must come first — its offset
  // within CtxWrap is part of the reader contract (see the
  // static_assert below) — and is therefore `public`. The bookkeeping
  // fields after it would normally be private, but the access change
  // would let a conforming compiler reorder them in front of `record_`;
  // exposing them publicly keeps everything in one ordering-stable
  // block. Readers never touch them.
 public:
  OtelThreadCtxRecord *record_;
  // attrs_data capacity in bytes of the record_ allocation. The total
  // allocation is `sizeof(OtelThreadCtxRecord) + capacity_`. Always
  // `record_->attrs_data_size <= capacity_ <= MAX_ATTRS_DATA_SIZE`.
  size_t capacity_;
  // Set to true (once, never cleared) if at any point in this record's
  // lifetime — during New() or any subsequent Append() — at least one
  // attribute had to be dropped because it would have pushed attrs_data
  // past MAX_ATTRS_DATA_SIZE.
  bool truncated_;
};

// Pin the offset of `record_` — the field the reader walks to from the
// JSObject's internal field 0. We document it as "the first field after
// the node::ObjectWrap base subobject", so equality with
// sizeof(node::ObjectWrap) is the invariant. `offsetof` on a non-
// standard-layout type (CtxWrap has private fields and inherits from
// ObjectWrap) is conditionally supported per the standard but accepted
// by every compiler this addon targets; suppress -Winvalid-offsetof so
// the static_assert compiles cleanly under strict warning flags.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
static_assert(offsetof(CtxWrap, record_) == sizeof(node::ObjectWrap),
              "record_ must be the first field after the ObjectWrap base "
              "subobject");
#pragma GCC diagnostic pop

CtxWrap::~CtxWrap() { free(record_); }

CtxWrap::CtxWrap(OtelThreadCtxRecord *record, size_t capacity, bool truncated)
    : record_(record), capacity_(capacity), truncated_(truncated) {}

// Copy exactly `expected_bytes` bytes out of a JS Uint8Array (or subclass
// such as Buffer) into `out`. Returns false if the value isn't a
// Uint8Array or its length doesn't match.
bool CopyBytes(Local<Value> value, size_t expected_bytes, uint8_t *out) {
  if (!value->IsUint8Array()) return false;
  Local<Uint8Array> arr = value.As<Uint8Array>();
  if (arr->ByteLength() != expected_bytes) return false;
  uint8_t *base =
      static_cast<uint8_t *>(arr->Buffer()->GetBackingStore()->Data()) +
      arr->ByteOffset();
  memcpy(out, base, expected_bytes);
  return true;
}

bool CtxWrap::EncodeAttrs(Isolate *isolate, Local<Context> context,
                          Local<Value> attrs_val, size_t existing_size,
                          std::vector<uint8_t> *out, bool *out_truncated) {
  if (attrs_val->IsUndefined() || attrs_val->IsNull()) return true;
  if (!attrs_val->IsArray()) {
    isolate->ThrowError(
        "attributes must be an array indexed by key, or undefined");
    return false;
  }
  Local<Array> attrs = attrs_val.As<Array>();
  uint32_t n = attrs->Length();
  if (n > 256) {
    isolate->ThrowError("attributes array length must not exceed 256");
    return false;
  }
  out->reserve(out->size() + n * 4);
  for (uint32_t i = 0; i < n; ++i) {
    Local<Value> val_val;
    if (!attrs->Get(context, i).ToLocal(&val_val)) return false;
    if (val_val->IsUndefined() || val_val->IsNull()) continue;

    Local<String> v;
    if (!val_val->ToString(context).ToLocal(&v)) return false;
    int v_utf8_len = v->Utf8Length(isolate);
    int v_budget = v_utf8_len > 255 ? 255 : v_utf8_len;

    const size_t needed = 2u + static_cast<size_t>(v_budget);
    if (existing_size + out->size() + needed > MAX_ATTRS_DATA_SIZE) {
      *out_truncated = true;
      continue;
    }

    const size_t entry_off = out->size();
    out->resize(entry_off + needed);
    (*out)[entry_off] = static_cast<uint8_t>(i);
    int v_written = v->WriteUtf8(
        isolate, reinterpret_cast<char *>(&(*out)[entry_off + 2]), v_budget,
        nullptr, String::NO_NULL_TERMINATION);
    (*out)[entry_off + 1] = static_cast<uint8_t>(v_written);
    if (v_written < v_budget) {
      out->resize(entry_off + 2u + static_cast<size_t>(v_written));
    }
  }
  return true;
}

void CtxWrap::New(const FunctionCallbackInfo<Value> &args) {
  Isolate *isolate = args.GetIsolate();
  Local<Context> context = isolate->GetCurrentContext();

  if (!args.IsConstructCall()) [[unlikely]] {
    isolate->ThrowError("OtelThreadCtxWrap must be called with `new`");
    return;
  }
  if (args.Length() != 3) {
    isolate->ThrowError(
        "OtelThreadCtxWrap expects 3 arguments: traceId, spanId, attributes");
    return;
  }

  uint8_t trace_id[16];
  uint8_t span_id[8];
  if (!CopyBytes(args[0], 16, trace_id)) {
    isolate->ThrowError("traceId must be a 16-byte Uint8Array");
    return;
  }
  if (!CopyBytes(args[1], 8, span_id)) {
    isolate->ThrowError("spanId must be an 8-byte Uint8Array");
    return;
  }

  std::vector<uint8_t> attrs_buf;
  bool truncated = false;
  if (!EncodeAttrs(isolate, context, args[2], 0, &attrs_buf, &truncated)) {
    return;
  }

  size_t capacity = std::max(attrs_buf.size(), MIN_INITIAL_CAPACITY);
  const size_t total = sizeof(OtelThreadCtxRecord) + capacity;
  OwnedRecord record(static_cast<OtelThreadCtxRecord *>(calloc(1, total)));
  if (!record) {
    isolate->ThrowError("allocation failed");
    return;
  }
  memcpy(record->trace_id, trace_id, sizeof(trace_id));
  memcpy(record->span_id, span_id, sizeof(span_id));
  record->attrs_data_size = static_cast<uint16_t>(attrs_buf.size());
  if (!attrs_buf.empty()) {
    memcpy(record->attrs_data, attrs_buf.data(), attrs_buf.size());
  }

  // OTEP-4947 publication protocol: order the `valid = 1` store after every
  // other field write, with an atomic_signal_fence + volatile store.
  std::atomic_signal_fence(std::memory_order_release);
  *reinterpret_cast<volatile uint8_t *>(&record->valid) = 1;

  CtxWrap *self = new CtxWrap(record.release(), capacity, truncated);
  self->Wrap(args.This());
  args.GetReturnValue().Set(args.This());
}

// Append entries to the active record. Either modifies the record in
// place (if the appended bytes fit in the current allocation's slack) or
// reallocates to a larger one (geometrically), keeping the invariant
// `record_->attrs_data_size <= capacity_`.
void CtxWrap::Append(const FunctionCallbackInfo<Value> &args) {
  Isolate *isolate = args.GetIsolate();
  Local<Context> context = isolate->GetCurrentContext();

  CtxWrap *self = ObjectWrap::Unwrap<CtxWrap>(args.This());
  if (!self) {
    isolate->ThrowError("not an OtelThreadCtxWrap");
    return;
  }
  if (args.Length() != 1) {
    isolate->ThrowError("append expects 1 argument: attributes");
    return;
  }

  const size_t current_used = self->record_->attrs_data_size;
  std::vector<uint8_t> appended;
  bool truncated = false;
  if (!EncodeAttrs(isolate, context, args[0], current_used, &appended,
                   &truncated)) {
    return;
  }
  if (truncated) self->truncated_ = true;

  if (appended.empty()) return;

  const size_t new_used = current_used + appended.size();

  if (new_used <= self->capacity_) {
    // In-place: write the new entries past the current attrs_data_size,
    // then bump attrs_data_size with a release fence + volatile store.
    // attrs_data_size is the publication boundary — bytes past it are
    // not observable by the reader, so a reader firing mid-append sees
    // either the old or new size, never a torn state.
    memcpy(&self->record_->attrs_data[current_used], appended.data(),
           appended.size());
    std::atomic_signal_fence(std::memory_order_release);
    *reinterpret_cast<volatile uint16_t *>(&self->record_->attrs_data_size) =
        static_cast<uint16_t>(new_used);
    return;
  }

  // Doesn't fit. Reallocate with geometric growth, capped.
  size_t new_cap = std::min(std::max(self->capacity_ * 2, new_used), MAX_ATTRS_DATA_SIZE);

  const size_t total = sizeof(OtelThreadCtxRecord) + new_cap;
  OwnedRecord new_rec(static_cast<OtelThreadCtxRecord *>(calloc(1, total)));
  if (!new_rec) {
    isolate->ThrowError("allocation failed");
    return;
  }
  memcpy(new_rec.get(), self->record_,
         sizeof(OtelThreadCtxRecord) + current_used);
  memcpy(&new_rec->attrs_data[current_used], appended.data(), appended.size());
  new_rec->attrs_data_size = static_cast<uint16_t>(new_used);
  // The copy should've preserved valid=1 from the source record.
  assert(new_rec->valid == 1);

  // Publish: the pointer swap is the atomic boundary the reader sees. The
  // first fence keeps the new_rec content writes ordered before the pointer
  // store from the compiler's perspective. The second fence prevents free()
  // from being hoisted above the pointer swap — without it, a reader stopped
  // between a reordered free() and the not-yet-completed swap would follow
  // self->record_ into freed memory. OTEP signal-handler semantics (the
  // writer is stopped during reads) take care of CPU-side ordering and make
  // immediate freeing of the old record safe.
  std::atomic_signal_fence(std::memory_order_release);
  OtelThreadCtxRecord *old_rec = self->record_;
  self->record_ = new_rec.release();
  self->capacity_ = new_cap;
  std::atomic_signal_fence(std::memory_order_acq_rel);
  free(old_rec);
}

void CtxWrap::IsTruncated(const FunctionCallbackInfo<Value> &args) {
  CtxWrap *self = ObjectWrap::Unwrap<CtxWrap>(args.This());
  if (!self) {
    args.GetIsolate()->ThrowError("not an OtelThreadCtxWrap");
    return;
  }
  args.GetReturnValue().Set(self->truncated_);
}

void CtxWrap::DebugBytes(const FunctionCallbackInfo<Value> &args) {
  Isolate *isolate = args.GetIsolate();
  CtxWrap *self = ObjectWrap::Unwrap<CtxWrap>(args.This());
  if (!self) {
    isolate->ThrowError("not an OtelThreadCtxWrap");
    return;
  }
  const size_t total =
      sizeof(OtelThreadCtxRecord) + self->record_->attrs_data_size;
  Local<v8::ArrayBuffer> buf = v8::ArrayBuffer::New(isolate, total);
  memcpy(buf->GetBackingStore()->Data(), self->record_, total);
  args.GetReturnValue().Set(Uint8Array::New(buf, 0, total));
}

void CtxWrap::Init(Local<Object> exports) {
#if NODE_MAJOR_VERSION >= 26
  Isolate *isolate = Isolate::GetCurrent();
#else
  Isolate *isolate = exports->GetIsolate();
#endif
  Local<Context> context = isolate->GetCurrentContext();

  Local<FunctionTemplate> tpl = FunctionTemplate::New(isolate, New);
  tpl->SetClassName(String::NewFromUtf8Literal(isolate, "OtelThreadCtxWrap"));
  tpl->InstanceTemplate()->SetInternalFieldCount(1);

  tpl->PrototypeTemplate()->Set(
      String::NewFromUtf8Literal(isolate, "debugBytes"),
      FunctionTemplate::New(isolate, DebugBytes));
  tpl->PrototypeTemplate()->Set(
      String::NewFromUtf8Literal(isolate, "append"),
      FunctionTemplate::New(isolate, Append));
  tpl->PrototypeTemplate()->Set(
      String::NewFromUtf8Literal(isolate, "isTruncated"),
      FunctionTemplate::New(isolate, IsTruncated));

  Local<Function> constructor = tpl->GetFunction(context).ToLocalChecked();
  exports
      ->Set(context, String::NewFromUtf8Literal(isolate, "otelThreadCtxWrap"),
            constructor)
      .FromJust();
}

// Reset the Global<Object> and the cped_slot pointer before the isolate
// is torn down. The Global lives in thread-local storage and its
// destructor only runs at thread exit, which on the main thread happens
// after the isolate is already gone — causing a segfault. Registering
// this as a per-isolate cleanup hook the first time StoreAls is called
// keeps the handle safely scoped to the isolate.
void ResetDiscoveryStruct(void * /*arg*/) {
  otel_thread_ctx_nodejs_v1.cped_slot = nullptr;
  otel_thread_ctx_nodejs_v1.als_handle.Reset();
  otel_thread_ctx_nodejs_v1.als_identity_hash = 0;
  otel_thread_ctx_nodejs_v1.undefined_addr = 0;
}

void StoreAls(const FunctionCallbackInfo<Value> &args) {
  static thread_local bool cleanup_registered = false;

  Isolate *isolate = args.GetIsolate();
  if (!args[0]->IsObject()) {
    isolate->ThrowError("First argument must be the AsyncLocalStorage object.");
    return;
  }
  Local<Object> obj = args[0].As<Object>();
  otel_thread_ctx_nodejs_v1.als_identity_hash = obj->GetIdentityHash();
  otel_thread_ctx_nodejs_v1.als_handle = Global<Object>(isolate, obj);
#if NODE_MAJOR_VERSION >= 22
  otel_thread_ctx_nodejs_v1.cped_slot = reinterpret_cast<v8::internal::Address *>(
      reinterpret_cast<char *>(isolate) +
      v8::internal::Internals::kContinuationPreservedEmbedderDataOffset);
#else
  // Node < 22 lacks ContinuationPreservedEmbedderData entirely (and the
  // associated V8 internal offset). The TS layer refuses to install the
  // hook on these versions via asyncContextFrameError, so StoreAls is
  // never called from JS — this null assignment is just here so the
  // addon compiles on the older Node versions the package supports.
  otel_thread_ctx_nodejs_v1.cped_slot = nullptr;
#endif
  // Cache the per-isolate undefined singleton's tagged address. Undefined
  // is a read-only-roots heap object, never moves, so a cached numeric
  // address is fine — no Global<> tracking needed.
  otel_thread_ctx_nodejs_v1.undefined_addr =
      reinterpret_cast<v8::internal::Address>(*v8::Undefined(isolate));
  if (!cleanup_registered) {
    node::AddEnvironmentCleanupHook(isolate, ResetDiscoveryStruct, nullptr);
    cleanup_registered = true;
  }
}

// Without a function that explicitly reads the TLS variable, on x86 the
// linker may strip the symbol from the dynamic symbol table even though
// `nm` still reports it, breaking out-of-process discovery.
void GetStoredAlsHash(const FunctionCallbackInfo<Value> &args) {
  Isolate *isolate = args.GetIsolate();
  args.GetReturnValue().Set(
      Integer::New(isolate, otel_thread_ctx_nodejs_v1.als_identity_hash));
}

// V8 layout constants captured at addon-compile time from the same V8
// headers Node bundles. Published via the discovery contract so an
// out-of-process reader can decode our wrapper / V8's internal hashmap
// layout without doing its own V8-internal-symbol lookups for the
// pointer-compression / sandbox state.
#if NODE_MAJOR_VERSION >= 22
constexpr int WRAPPED_OBJECT_OFFSET =
    v8::internal::Internals::kJSObjectHeaderSize +
    v8::internal::Internals::kEmbedderDataSlotExternalPointerOffset;
#else
// Node < 22 lacks kEmbedderDataSlotExternalPointerOffset. The discovery
// contract isn't usable on these versions (no ContinuationPreservedEmbedderData
// either — see StoreAls), so this value is published only to keep the
// addon's exported surface consistent across Node majors. A would-be
// reader cannot reach a live record through it.
constexpr int WRAPPED_OBJECT_OFFSET = 0;
#endif
constexpr int TAGGED_SIZE = v8::internal::kApiTaggedSize;

}  // namespace

void OtelThreadCtx::Init(Local<Object> exports) {
  CtxWrap::Init(exports);
  NODE_SET_METHOD(exports, "otelThreadCtxStoreAls", StoreAls);
  NODE_SET_METHOD(exports, "otelThreadCtxGetStoredAlsHash", GetStoredAlsHash);

  Isolate *isolate = exports->GetIsolate();
  Local<Context> ctx = isolate->GetCurrentContext();
  exports
      ->Set(ctx,
            String::NewFromUtf8Literal(isolate, "otelThreadCtxWrappedObjectOffset"),
            Integer::New(isolate, WRAPPED_OBJECT_OFFSET))
      .FromJust();
  exports
      ->Set(ctx,
            String::NewFromUtf8Literal(isolate, "otelThreadCtxTaggedSize"),
            Integer::New(isolate, TAGGED_SIZE))
      .FromJust();
}

}  // namespace dd
