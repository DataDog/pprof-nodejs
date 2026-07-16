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

// Vendored from
// https://github.com/polarsignals/custom-labels/tree/otel-thread-ctx-wip/js/
// (originally js/addon.cpp). Kept as a near-verbatim copy: edits should
// ideally land upstream first and be ported here, so the two stay in
// sync. We plan to drop this vendored copy once the upstream package is
// suitable to depend on directly.

// Node.js writer for the OTEP-4947 Thread Local Context Record, adapted for
// the Node.js asynchronous context model. The record is wrapped in a JS
// object (CtxWrap) and stored in an AsyncLocalStorage instance; an
// out-of-process reader discovers it by walking the V8 isolate's
// ContinuationPreservedEmbedderData to the AsyncContextFrame (a JS Map),
// looking up the ALS instance as the key, reading the resulting CtxWrap,
// and finally the record it owns.

#include "otel-thread-ctx.hh"

#include "defer.hh"

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
//    currently attached; without it, a reader walking through undefined
//    would have to rely on structural validation of the bytes at
//    undefined+wrapped_object_offset to detect the absence.
//
// Layout is part of the reader ABI: see the README "Discovery contract"
// section and the static_asserts below.
extern "C" {
using v8::Global;
using v8::Object;

struct otel_thread_ctx_nodejs_v1_t {
  v8::internal::Address* cped_slot;      // offset 0
  Global<Object> als_handle;             // offset sizeof(void*); 1 V8 ptr
  int als_identity_hash;                 // offset 2 * sizeof(void*); 4 + 4 pad
  v8::internal::Address undefined_addr;  // offset 3 * sizeof(void*); tagged
};

// MSVC doesn't understand __attribute__; visibility is irrelevant on
// Windows anyway since the OTEP-4947 reader contract is ELF-TLSDESC and
// only meaningful on Linux.
#if defined(__GNUC__) || defined(__clang__)
__attribute__((visibility("default")))
#endif
thread_local otel_thread_ctx_nodejs_v1_t otel_thread_ctx_nodejs_v1;
}

static_assert(sizeof(v8::Global<v8::Object>) == sizeof(void*),
              "Global<Object> must be exactly one pointer wide");
static_assert(offsetof(otel_thread_ctx_nodejs_v1_t, cped_slot) == 0,
              "cped_slot must be at offset 0");
static_assert(offsetof(otel_thread_ctx_nodejs_v1_t, als_handle) ==
                  sizeof(void*),
              "als_handle must immediately follow cped_slot");
static_assert(offsetof(otel_thread_ctx_nodejs_v1_t, als_identity_hash) ==
                  2 * sizeof(void*),
              "als_identity_hash must immediately follow als_handle");
static_assert(offsetof(otel_thread_ctx_nodejs_v1_t, undefined_addr) ==
                  3 * sizeof(void*),
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
// see only the 28-byte header. Field offsets are statically verified
// below.
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
  void operator()(OtelThreadCtxRecord* p) const noexcept { free(p); }
};
using OwnedRecord =
    std::unique_ptr<OtelThreadCtxRecord, OtelThreadCtxRecordDeleter>;

// Floor on the attrs_data capacity of a freshly allocated record. Sized so
// the total allocation is one 64-byte cache line — matching the OTEP-4947
// "frugal writer" guidance ("a frugal writer may aim to keep the entire
// record under 64 bytes") — and giving small records some slack so the
// first few appends (if any) can be in-place.
constexpr size_t MIN_INITIAL_CAPACITY = 64 - sizeof(OtelThreadCtxRecord);

// Upper bound on the attribute payload. Sized so the total record (28-byte
// header + attrs_data) stays under the OTEP-4947 recommended 640 bytes,
// which is the read-buffer ceiling for typical eBPF readers. Attributes
// that would push past this are silently dropped (with `truncated_` set on
// the wrapper) rather than the writer throwing — the OTEP treats the cap
// as best-effort.
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

  CtxWrap(const CtxWrap&) = delete;
  CtxWrap& operator=(const CtxWrap&) = delete;
  CtxWrap(CtxWrap&&) = delete;
  CtxWrap& operator=(CtxWrap&&) noexcept = delete;

 private:
  static void New(const FunctionCallbackInfo<Value>& args);
  static void DebugBytes(const FunctionCallbackInfo<Value>& args);
  static void Append(const FunctionCallbackInfo<Value>& args);
  static void IsTruncated(const FunctionCallbackInfo<Value>& args);

  // Encode the JS array at `attrs_val` into `out` as packed (key, len, value)
  // entries. Same shape used by both New() and Append(). On a parse error
  // (non-array, etc.) throws via `isolate` and returns false. On per-entry
  // overflow against the 612-byte attrs_data cap, the entry is dropped,
  // `*out_truncated` is set to true, and processing continues with the
  // next entry (a smaller subsequent entry may still fit).
  static bool EncodeAttrs(Isolate* isolate,
                          Local<Context> context,
                          Local<Value> attrs_val,
                          size_t existing_size,
                          std::vector<uint8_t>* out,
                          bool* out_truncated);

  CtxWrap(OtelThreadCtxRecord* record, size_t capacity, bool truncated);

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
  OtelThreadCtxRecord* record_;
  // attrs_data capacity in bytes of the record_ allocation. The total
  // allocation is `sizeof(OtelThreadCtxRecord) + capacity_`. Always
  // `record_->attrs_data_size <= capacity_ <= MAX_ATTRS_DATA_SIZE`.
  size_t capacity_;
  // Set to true (once, never cleared) if at any point in this record's
  // lifetime — during New() or any subsequent Append() — at least one
  // attribute had to be dropped because it would have pushed attrs_data
  // past MAX_ATTRS_DATA_SIZE.
  bool truncated_;
  // Reentrancy guard for Append(). EncodeAttrs calls ToString on each
  // attribute value, which can execute user JS (e.g. a custom
  // `toString`) that in turn calls `appendAttributes` on the same
  // ThreadContext. A reentrant Append would mutate attrs_data_size out
  // from under the outer call's `current_used` snapshot, causing the
  // outer memcpy to overwrite the reentrant call's bytes and the outer
  // attrs_data_size write to shrink the record. We reject the reentrant
  // call instead. New() doesn't need the guard because a freshly constructed
  // CtxWrap isn't observable to JS until New() returns.
  bool encoding_;
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

CtxWrap::~CtxWrap() {
  free(record_);
}

CtxWrap::CtxWrap(OtelThreadCtxRecord* record, size_t capacity, bool truncated)
    : record_(record),
      capacity_(capacity),
      truncated_(truncated),
      encoding_(false) {}

// Copy exactly `expected_bytes` bytes out of a JS Uint8Array (or subclass
// such as Buffer) into `out`. Returns false if the value isn't a
// Uint8Array or its length doesn't match.
bool CopyBytes(Local<Value> value, size_t expected_bytes, uint8_t* out) {
  if (!value->IsUint8Array()) return false;
  Local<Uint8Array> arr = value.As<Uint8Array>();
  if (arr->ByteLength() != expected_bytes) return false;
  uint8_t* base =
      static_cast<uint8_t*>(arr->Buffer()->GetBackingStore()->Data()) +
      arr->ByteOffset();
  memcpy(out, base, expected_bytes);
  return true;
}

// Encode the JS array `attrs_val` (positional, index N = uint8 key N) into
// `*out` as packed `(key:u8, len:u8, value:u8[len])` entries.
// `existing_size` is the number of bytes already in any pre-existing
// record's attrs_data — used so the cap is enforced across the combined
// result. On a parse error (wrong type, etc.) throws and returns false. An
// entry whose encoding would push the combined size past MAX_ATTRS_DATA_SIZE
// is dropped (not encoded into `*out`), `*out_truncated` is set, and
// processing continues so a smaller subsequent entry may still fit.
bool CtxWrap::EncodeAttrs(Isolate* isolate,
                          Local<Context> context,
                          Local<Value> attrs_val,
                          size_t existing_size,
                          std::vector<uint8_t>* out,
                          bool* out_truncated) {
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

  // Reserve a conservative upper bound; reallocations are cheap but
  // unnecessary for the typical small attribute set.
  out->reserve(out->size() + n * 4);
  for (uint32_t i = 0; i < n; ++i) {
    Local<Value> val_val;
    if (!attrs->Get(context, i).ToLocal(&val_val)) return false;
    // null / undefined / array holes mean "no value at this key index".
    if (val_val->IsUndefined() || val_val->IsNull()) continue;
    Local<String> v;
    if (!val_val->ToString(context).ToLocal(&v)) return false;
#if NODE_MAJOR_VERSION >= 24
    int v_utf8_len = static_cast<int>(v->Utf8LengthV2(isolate));
#else
    int v_utf8_len = v->Utf8Length(isolate);
#endif
    // The on-the-wire val_len prefix is a uint8, so individual values
    // longer than 255 UTF-8 bytes are silently truncated to 255.
    int v_budget = v_utf8_len > 255 ? 255 : v_utf8_len;

    const size_t needed = 2u + static_cast<size_t>(v_budget);
    if (existing_size + out->size() + needed > MAX_ATTRS_DATA_SIZE) {
      // Doesn't fit in the remaining budget; drop this entry and set the
      // truncated flag. Smaller subsequent entries may still fit, so we
      // continue rather than break.
      *out_truncated = true;
      continue;
    }

    const size_t entry_off = out->size();
    out->resize(entry_off + needed);
    (*out)[entry_off] = static_cast<uint8_t>(i);
    // WriteUtf8 returns the actual number of bytes written, which can be
    // less than v_budget when the cap lands inside a multibyte codepoint
    // — WriteUtf8 stops before writing a partial sequence. Use that count
    // as the length prefix, and shrink the buffer back so the next entry
    // starts at exactly the right offset.
#if NODE_MAJOR_VERSION >= 24
    int v_written = static_cast<int>(
        v->WriteUtf8V2(isolate,
                       reinterpret_cast<char*>(&(*out)[entry_off + 2]),
                       static_cast<size_t>(v_budget),
                       String::WriteFlags::kNone));
#else
    int v_written =
        v->WriteUtf8(isolate,
                     reinterpret_cast<char*>(&(*out)[entry_off + 2]),
                     v_budget,
                     nullptr,
                     String::NO_NULL_TERMINATION);
#endif
    (*out)[entry_off + 1] = static_cast<uint8_t>(v_written);
    if (v_written < v_budget) {
      out->resize(entry_off + 2u + static_cast<size_t>(v_written));
    }
  }
  return true;
}

void CtxWrap::New(const FunctionCallbackInfo<Value>& args) {
  Isolate* isolate = args.GetIsolate();
  Local<Context> context = isolate->GetCurrentContext();

  if (!args.IsConstructCall()) [[unlikely]] {
    isolate->ThrowError("ThreadContext must be called with `new`");
    return;
  }
  if (args.Length() < 2 || args.Length() > 3) {
    isolate->ThrowError(
        "ThreadContext expects 2 or 3 arguments: traceId, spanId, "
        "attributes?");
    return;
  }

  // Validate IDs into a scratch header first; we copy into the final
  // allocation once we know how much room the attrs payload needs.
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

  // Encode attributes into a transient buffer first so we can size the
  // record allocation correctly. The 612-byte attrs_data cap mirrors the
  // OTEP-recommended 640-byte total-record ceiling (which exists for
  // eBPF readers that copy the record into a fixed-size kernel buffer);
  // entries that wouldn't fit are silently dropped and recorded via the
  // truncated flag below.
  std::vector<uint8_t> attrs_buf;
  bool truncated = false;
  if (!EncodeAttrs(isolate, context, args[2], 0, &attrs_buf, &truncated)) {
    return;
  }

  // Pick the initial attrs_data capacity. Small records get a 64-byte
  // floor so the first append is likely to fit in-place; larger records
  // are sized exactly to what's needed (the extra slack a doubling
  // strategy would buy is dwarfed by the existing memory footprint and
  // doesn't change the geometric-growth amortized cost of subsequent
  // appends).
  size_t capacity = std::max(attrs_buf.size(), MIN_INITIAL_CAPACITY);
  const size_t total = sizeof(OtelThreadCtxRecord) + capacity;
  OwnedRecord record(static_cast<OtelThreadCtxRecord*>(calloc(1, total)));
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
  // other field write, with an `atomic_signal_fence` to pin that ordering at
  // compile time and a volatile store so the compiler can't fold or hoist
  // the write. The signal fence + volatile store is also the protocol used
  // by Append() in its in-place path.
  std::atomic_signal_fence(std::memory_order_release);
  *reinterpret_cast<volatile uint8_t*>(&record->valid) = 1;

  CtxWrap* self = new CtxWrap(record.release(), capacity, truncated);
  self->Wrap(args.This());
  args.GetReturnValue().Set(args.This());
}

// Append entries to the active record. Either modifies the record in place
// (if the appended bytes fit in the current allocation's slack) or
// reallocates to a larger one (geometrically), keeping invariant
// `record_->attrs_data_size <= capacity_`.
void CtxWrap::Append(const FunctionCallbackInfo<Value>& args) {
  Isolate* isolate = args.GetIsolate();
  Local<Context> context = isolate->GetCurrentContext();

  CtxWrap* self = ObjectWrap::Unwrap<CtxWrap>(args.This());
  if (!self) {
    isolate->ThrowError("not a ThreadContext");
    return;
  }
  if (args.Length() != 1) {
    isolate->ThrowError("append expects 1 argument: attributes");
    return;
  }

  // Reject reentrant Append on the same wrap. EncodeAttrs' `ToString`
  // below can execute user JS, and if that JS calls `appendAttributes`
  // on this same ThreadContext, the reentrant call would grow
  // attrs_data_size out from under the outer call's `current_used`
  // snapshot, causing the outer memcpy to overwrite the reentrant call's
  // bytes and the outer attrs_data_size write to shrink the record.
  if (self->encoding_) {
    isolate->ThrowError(
        "reentrant appendAttributes on the same ThreadContext is not allowed");
    return;
  }
  self->encoding_ = true;
  defer {
    self->encoding_ = false;
  };

  const size_t current_used = self->record_->attrs_data_size;
  std::vector<uint8_t> appended;
  bool truncated = false;
  if (!EncodeAttrs(
          isolate, context, args[0], current_used, &appended, &truncated)) {
    return;
  }
  if (truncated) self->truncated_ = true;

  // Nothing to append — either the input array was empty, every slot was
  // null/undefined, or every entry was dropped because the record is
  // already at the cap.
  if (appended.empty()) return;

  const size_t new_used = current_used + appended.size();
  // EncodeAttrs already enforced the cap; new_used <= MAX_ATTRS_DATA_SIZE.

  if (new_used <= self->capacity_) {
    // In-place: write the new entries past the current attrs_data_size,
    // then bump attrs_data_size with a release fence + volatile store so
    // the content writes are visible before the size store from the
    // compiler's perspective.
    //
    // No valid=0/valid=1 dance: this is an append-only operation. Bytes
    // past attrs_data_size aren't observable by the reader, and
    // attrs_data_size *is* the publication boundary. A reader firing
    // mid-append sees either the old size (old extent, ignores the
    // half-written tail) or the new size (full new extent, all bytes
    // written). Either is consistent.
    memcpy(&self->record_->attrs_data[current_used],
           appended.data(),
           appended.size());
    std::atomic_signal_fence(std::memory_order_release);
    *reinterpret_cast<volatile uint16_t*>(&self->record_->attrs_data_size) =
        static_cast<uint16_t>(new_used);
    return;
  }

  // Doesn't fit. Reallocate with geometric growth with cap.
  size_t new_cap =
      std::min(std::max(self->capacity_ * 2, new_used), MAX_ATTRS_DATA_SIZE);

  const size_t total = sizeof(OtelThreadCtxRecord) + new_cap;
  OwnedRecord new_rec(static_cast<OtelThreadCtxRecord*>(calloc(1, total)));
  if (!new_rec) {
    isolate->ThrowError("allocation failed");
    return;
  }
  // Copy the existing record (header + already-written attrs_data).
  memcpy(
      new_rec.get(), self->record_, sizeof(OtelThreadCtxRecord) + current_used);
  // Append the new entries and update attrs_data_size.
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
  OtelThreadCtxRecord* old_rec = self->record_;
  self->record_ = new_rec.release();
  self->capacity_ = new_cap;
  std::atomic_signal_fence(std::memory_order_acq_rel);
  free(old_rec);
}

// Returns true if any attribute was ever dropped from this wrapper's
// record because it would have pushed attrs_data past the cap — set during
// CtxWrap::New() if the initial set didn't fit, or by any subsequent
// CtxWrap::Append() call.
void CtxWrap::IsTruncated(const FunctionCallbackInfo<Value>& args) {
  CtxWrap* self = ObjectWrap::Unwrap<CtxWrap>(args.This());
  if (!self) {
    args.GetIsolate()->ThrowError("not a ThreadContext");
    return;
  }
  args.GetReturnValue().Set(self->truncated_);
}

// Debug accessor: returns the record (header + attrs_data) as a fresh
// Uint8Array sized to the actual on-the-wire length. Not part of the stable
// API; intended for tests and out-of-process-reader development.
void CtxWrap::DebugBytes(const FunctionCallbackInfo<Value>& args) {
  Isolate* isolate = args.GetIsolate();
  CtxWrap* self = ObjectWrap::Unwrap<CtxWrap>(args.This());
  if (!self) {
    isolate->ThrowError("not a ThreadContext");
    return;
  }
  const size_t total =
      sizeof(OtelThreadCtxRecord) + self->record_->attrs_data_size;
  Local<v8::ArrayBuffer> buf = v8::ArrayBuffer::New(isolate, total);
  memcpy(buf->GetBackingStore()->Data(), self->record_, total);
  args.GetReturnValue().Set(Uint8Array::New(buf, 0, total));
}

void CtxWrap::Init(Local<Object> exports) {
  Isolate* isolate = Isolate::GetCurrent();
  Local<Context> context = isolate->GetCurrentContext();

  Local<FunctionTemplate> tpl = FunctionTemplate::New(isolate, New);
  tpl->SetClassName(String::NewFromUtf8Literal(isolate, "ThreadContext"));
  tpl->InstanceTemplate()->SetInternalFieldCount(1);

  tpl->PrototypeTemplate()->Set(
      String::NewFromUtf8Literal(isolate, "debugBytes"),
      FunctionTemplate::New(isolate, DebugBytes));
  tpl->PrototypeTemplate()->Set(
      String::NewFromUtf8Literal(isolate, "appendAttributes"),
      FunctionTemplate::New(isolate, Append));
  tpl->PrototypeTemplate()->Set(
      String::NewFromUtf8Literal(isolate, "isTruncated"),
      FunctionTemplate::New(isolate, IsTruncated));

  Local<Function> constructor = tpl->GetFunction(context).ToLocalChecked();
  exports
      ->Set(context,
            String::NewFromUtf8Literal(isolate, "threadContext"),
            constructor)
      .FromJust();
}

// Reset the Global<Object> and the cped_slot pointer before the isolate
// is torn down. The Global lives in thread-local storage and its
// destructor only runs at thread exit, which on the main thread happens
// after the isolate is already gone — causing a segfault. Registering
// this as a per-isolate cleanup hook the first time StoreAls is called
// keeps the handle safely scoped to the isolate.
void ResetDiscoveryStruct(void* /*arg*/) {
  otel_thread_ctx_nodejs_v1.cped_slot = nullptr;
  otel_thread_ctx_nodejs_v1.als_handle.Reset();
  otel_thread_ctx_nodejs_v1.als_identity_hash = 0;
  otel_thread_ctx_nodejs_v1.undefined_addr = 0;
}

void StoreAls(const FunctionCallbackInfo<Value>& args) {
  Isolate* isolate = args.GetIsolate();
  if (!args[0]->IsObject()) {
    isolate->ThrowError("First argument must be the AsyncLocalStorage object.");
    return;
  }
  Local<Object> obj = args[0].As<Object>();
  otel_thread_ctx_nodejs_v1.als_identity_hash = obj->GetIdentityHash();
  otel_thread_ctx_nodejs_v1.als_handle = Global<Object>(isolate, obj);
#if NODE_MAJOR_VERSION >= 22
  otel_thread_ctx_nodejs_v1.cped_slot =
      reinterpret_cast<v8::internal::Address*>(
          reinterpret_cast<char*>(isolate) +
          v8::internal::Internals::kContinuationPreservedEmbedderDataOffset);
#else
  // Node < 22 lacks ContinuationPreservedEmbedderData entirely (and the
  // associated V8 internal offset). The TS layer refuses to install the
  // hook on these versions via asyncContextFrameError, so StoreAls is
  // never called from JS — this null assignment is just here so the
  // addon compiles on the older Node versions the package supports.
  otel_thread_ctx_nodejs_v1.cped_slot = nullptr;
#endif
  // `undefined_addr == 0` doubles as the "not yet initialized on this
  // isolate" flag: it starts at zero (thread-local zero-init), any real
  // V8 undefined singleton address is non-zero, and ResetDiscoveryStruct
  // clears it back to zero — so a subsequent StoreAls (e.g. isolate
  // tear-down then re-init on the same thread) re-registers the cleanup
  // hook. Register BEFORE the write so the flag transition is the last
  // observable step.
  if (otel_thread_ctx_nodejs_v1.undefined_addr == 0) {
    node::AddEnvironmentCleanupHook(isolate, ResetDiscoveryStruct, nullptr);
  }
  // Cache the per-isolate undefined singleton's tagged address. Undefined
  // is a read-only-roots heap object, never moves, so a cached numeric
  // address is fine — no Global<> tracking needed.
  otel_thread_ctx_nodejs_v1.undefined_addr =
      reinterpret_cast<v8::internal::Address>(*v8::Undefined(isolate));
}

// Without a function that explicitly reads the TLS variable, on x86 the
// linker may strip the symbol from the dynamic symbol table even though
// `nm` still reports it, breaking out-of-process discovery.
void GetStoredAlsHash(const FunctionCallbackInfo<Value>& args) {
  Isolate* isolate = args.GetIsolate();
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

// sizeof(node::ObjectWrap). Given a pointer to a CtxWrap — or any other
// ObjectWrap-derived C++ object attached to a JSObject via the V8
// wrapped-object slot — add this offset to reach the derived class's own
// fields. For CtxWrap, that's `record_` (see the static_assert on its
// offset above).
constexpr int NATIVE_WRAP_FIELDS_OFFSET =
    static_cast<int>(sizeof(node::ObjectWrap));

// V8 JSMap layout: kTableOffset within the JSMap object holds a tagged
// pointer to the backing OrderedHashMap table. Not exposed in V8's
// public headers; kept in sync with
// deps/v8/src/objects/js-collection.h (JSCollection::kTableOffset)
// and the torque-generated JSCollection layout.
constexpr int JS_MAP_TABLE_OFFSET = 0x18;

// V8 OrderedHashMap layout: the on-heap table starts with a 16-byte
// header before the element_count / deleted_element_count /
// number_of_buckets fields. Not exposed in V8's public headers; kept in
// sync with deps/v8/src/objects/ordered-hash-table.h
// (OrderedHashTable base layout).
constexpr int ORDERED_HASH_MAP_HEADER_SIZE = 0x10;

}  // namespace

void OtelThreadCtx::Init(Local<Object> exports) {
  CtxWrap::Init(exports);
  NODE_SET_METHOD(exports, "otelThreadCtxStoreAls", StoreAls);
  NODE_SET_METHOD(exports, "otelThreadCtxGetStoredAlsHash", GetStoredAlsHash);

  Isolate* isolate = Isolate::GetCurrent();
  Local<Context> ctx = isolate->GetCurrentContext();
  exports
      ->Set(ctx,
            String::NewFromUtf8Literal(isolate,
                                       "otelThreadCtxWrappedObjectOffset"),
            Integer::New(isolate, WRAPPED_OBJECT_OFFSET))
      .FromJust();
  exports
      ->Set(ctx,
            String::NewFromUtf8Literal(isolate, "otelThreadCtxTaggedSize"),
            Integer::New(isolate, TAGGED_SIZE))
      .FromJust();
  exports
      ->Set(ctx,
            String::NewFromUtf8Literal(isolate,
                                       "otelThreadCtxNativeWrapFieldsOffset"),
            Integer::New(isolate, NATIVE_WRAP_FIELDS_OFFSET))
      .FromJust();
  exports
      ->Set(
          ctx,
          String::NewFromUtf8Literal(isolate, "otelThreadCtxJsMapTableOffset"),
          Integer::New(isolate, JS_MAP_TABLE_OFFSET))
      .FromJust();
  exports
      ->Set(ctx,
            String::NewFromUtf8Literal(isolate,
                                       "otelThreadCtxOrderedHashMapHeaderSize"),
            Integer::New(isolate, ORDERED_HASH_MAP_HEADER_SIZE))
      .FromJust();
}

}  // namespace dd
