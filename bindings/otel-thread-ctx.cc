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
// object and stored in an AsyncLocalStorage instance; an out-of-process
// reader discovers it by walking the V8 isolate's
// ContinuationPreservedEmbedderData to the AsyncContextFrame (a JS Map),
// looking up the ALS instance as the key, and reading the record pointer
// out of the resulting object's internal field. That field points
// straight at the record. The record is preceded in memory by an instance
// of CtxWrap used for internal bookkeeping.

#include "otel-thread-ctx.hh"

#include "internal-field.hh"

#include <node.h>
#include <v8-internal.h>

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <atomic>
#include <new>
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
//    JSObject / internal-field-0 dereference when no ThreadContext is
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

// Floor on the attrs_data capacity of a freshly allocated record. Matching the
// OTEP-4947 "frugal writer" guidance ("a frugal writer may aim to keep the
// entire record under 64 bytes") — and giving small records some slack so the
// first few appends (if any) can be in-place. The CtxWrap fields preceding
// the record in the same block are writer bookkeeping the reader never
// sees, so they don't count against that budget.
constexpr size_t MIN_INITIAL_CAPACITY = 64 - sizeof(OtelThreadCtxRecord);

// Upper bound on the attribute payload. Sized so the total record (28-byte
// header + attrs_data) stays under the OTEP-4947 recommended 640 bytes,
// which is the read-buffer ceiling for typical eBPF readers. Attributes
// that would push past this are silently dropped (with `truncated_` set on
// the wrapper) rather than the writer throwing — the OTEP treats the cap
// as best-effort.
constexpr size_t MAX_ATTRS_DATA_SIZE = 640 - sizeof(OtelThreadCtxRecord);

// Wraps an OTEP-4947 record. The record isn't a separate allocation: a
// CtxWrap is one contiguous block whose head is the CtxWrap object itself
// and whose tail — starting at `RECORD_OFFSET`, immediately past the
// object's own fields — holds the record header followed by `capacity_`
// bytes of attrs_data. Lifetime is managed by V8 GC: when no JS code (or
// AsyncLocalStorage entry) holds a reference, the whole block is freed.
//
// Layout note for the reader: the holder JSObject's internal field 0 points
// at the record itself, not at the CtxWrap, so a reader that has walked to
// the holder is a single dereference away from the record and never has to
// know that CtxWrap exists at all. C++ code goes the other way with
// FromRecord(), which just subtracts RECORD_OFFSET.
// Deliberately not a node::ObjectWrap, for two independent reasons. First, it
// has a known bug in interaction with GC when numerous instances are created
// and can abort the process during isolate teardown; instances live at
// shutdown are deleted using DrainLive instead. Second, node::ObjectWrap owns
// internal field 0 — its Wrap() stores the ObjectWrap pointer there and its
// Unwrap() reads it back — so deriving from it would force that slot to hold a
// CtxWrap pointer, and every reader would be stuck with the extra hop through
// the wrapper that the layout above exists to avoid. Not deriving from it is
// what frees the slot for the record pointer.
class CtxWrap {
 public:
  static void Init(Local<Object> exports);

  CtxWrap(const CtxWrap&) = delete;
  CtxWrap& operator=(const CtxWrap&) = delete;
  CtxWrap(CtxWrap&&) = delete;
  CtxWrap& operator=(CtxWrap&&) noexcept = delete;

 private:
  static void New(const FunctionCallbackInfo<Value>& args);
  static void DebugBytes(const FunctionCallbackInfo<Value>& args);
  static void Append(const FunctionCallbackInfo<Value>& args);
  static void Invalidate(const FunctionCallbackInfo<Value>& args);
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

  // Splice `appended` onto the end of the record: in place if it fits in the
  // current allocation's slack, otherwise by moving the whole wrap to a larger
  // one and repointing `holder`'s internal field at the new record. In that
  // case `this` is destroyed before returning, so the caller must not touch it
  // afterwards. Returns false if the allocation fails.
  bool AppendEncoded(Local<Object> holder,
                     const std::vector<uint8_t>& appended);

  explicit CtxWrap(size_t capacity);
  ~CtxWrap();

  // Allocate one zero-initialised block big enough for the object plus a
  // record with `capacity` bytes of attrs_data, and construct the CtxWrap
  // at its head. Returns nullptr if the allocation fails. CtxWraps are
  // never created with plain `new` — the trailing record wouldn't be there.
  static CtxWrap* Create(size_t capacity);
  // Destroy and free a CtxWrap obtained from Create(). Runs the destructor
  // and then `free`, which is what the `calloc` in Create() pairs with;
  // `delete` would reach for `operator delete` instead, and the mismatch is
  // undefined behaviour that a replaced global allocator or ASan will
  // actually trip over. Deleting `operator delete` below makes reaching for
  // it a compile error.
  static void Destroy(CtxWrap* self);
  static void operator delete(void*) = delete;

  // The record living in the tail of this object's own allocation.
  OtelThreadCtxRecord* record();
  // The CtxWrap that precedes the `record`. Inverse of record().
  static CtxWrap* FromRecord(void* record);

  // Attach to the holder JSObject: store our record pointer in internal
  // field 0 (the reader's entry point) and take a weak handle on the
  // holder, so V8 destroys us once it collects it.
  void Wrap(Local<Object> holder);
  static CtxWrap* Unwrap(Local<Object> holder);
  static void WeakCallback(const v8::WeakCallbackInfo<CtxWrap>& data);
  static void DrainLive(void* arg);

  // attrs_data capacity in bytes of the record in this object's tail. The
  // total allocation is `RECORD_OFFSET + sizeof(OtelThreadCtxRecord) +
  // capacity_`. Always `record()->attrs_data_size <= capacity_ <=
  // MAX_ATTRS_DATA_SIZE`.
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
  // from under the outer call's snapshot of it, causing the outer memcpy
  // to overwrite the reentrant call's bytes and the outer
  // attrs_data_size write to shrink the record. We reject the reentrant
  // call instead. New() doesn't need the guard because a freshly constructed
  // CtxWrap isn't observable to JS until New() returns.
  bool encoding_;
  // Intrusive doubly-linked list of the CtxWraps still alive on this thread,
  // threaded through g_live_ctx_wraps. `pprev_` is the address of the pointer
  // currently referencing us, so unlinking needs no head/non-head branch;
  // `pprev_ == nullptr` is the "already detached" sentinel set by the drain
  // hook before it deletes us. Same shape as WallProfiler's PCP list.
  CtxWrap** pprev_;
  CtxWrap* next_;
  // Weak handle on the holder object; owns this CtxWrap.
  v8::Global<v8::Object> handle_;
};

// Byte offset of the record within a CtxWrap allocation: the record starts
// immediately after the object's own fields. Both record() and FromRecord()
// are defined in terms of it.
constexpr size_t RECORD_OFFSET = sizeof(CtxWrap);
static_assert(RECORD_OFFSET % alignof(OtelThreadCtxRecord) == 0,
              "record must land on its natural alignment");
// Most likely subsumed in the previous assert, but still call out directly
// that record must be 2-aligned as that's a requirement for storing it
// with v8::Object::SetAlignedPointerInInternalField().
static_assert(RECORD_OFFSET % 2 == 0, "record must land on 2 boundary");

inline OtelThreadCtxRecord* CtxWrap::record() {
  return reinterpret_cast<OtelThreadCtxRecord*>(
      reinterpret_cast<uint8_t*>(this) + RECORD_OFFSET);
}

inline CtxWrap* CtxWrap::FromRecord(void* record) {
  return reinterpret_cast<CtxWrap*>(static_cast<uint8_t*>(record) -
                                    RECORD_OFFSET);
}

CtxWrap* CtxWrap::Create(size_t capacity) {
  void* mem = calloc(1, RECORD_OFFSET + sizeof(OtelThreadCtxRecord) + capacity);
  if (mem == nullptr) return nullptr;
  return new (mem) CtxWrap(capacity);
}

void CtxWrap::Destroy(CtxWrap* self) {
  self->~CtxWrap();
  free(self);
}

// Head of the live-CtxWrap list for this thread. Node pins each isolate to a
// thread, and CtxWraps are only ever constructed and destroyed on their own
// isolate's thread, so a thread-local needs no lock — the same reasoning the
// wall profiler uses for its active-profiler pointer.
// `otel_thread_ctx_nodejs_v1` above is thread-local for the same reason.
thread_local CtxWrap* g_live_ctx_wraps = nullptr;

// Destroy every CtxWrap V8 has not collected yet. Without it the records would
// simply leak at exit. Registered once per isolate from Init(). It fires
// exactly once, at teardown, while the Environment is still alive.
void CtxWrap::DrainLive(void* arg) {
  auto* isolate = static_cast<Isolate*>(arg);
  // We must allocate our own HandleScope here as node::FreeEnvironment wraps
  // RunCleanup in a SealHandleScope, so handle_.Get() below has to allocate
  // inside a scope of our own or V8 aborts with "Cannot create a handle without
  // a HandleScope".
  v8::HandleScope scope(isolate);

  CtxWrap* p = g_live_ctx_wraps;
  while (p != nullptr) {
    CtxWrap* next = p->next_;
    p->pprev_ = nullptr;
    p->next_ = nullptr;
    // Clear the holder's internal field (containing p's record pointer), so
    // nothing can reach a dangling record through it including the
    // out-of-process reader, which walks this slot. Being on the live list
    // means V8 has not collected the holder, so the handle is safe to read
    // here; the WeakCallback path cannot do this and does not need to,
    // since there the holder is the thing being collected.
    if (!p->handle_.IsEmpty()) {
      SetAlignedPointerInInternalField(p->handle_.Get(isolate), 0, nullptr);
    }
    Destroy(p);
    p = next;
  }
  g_live_ctx_wraps = nullptr;
}

CtxWrap::~CtxWrap() {
  // pprev_ != nullptr means we are still on the live list, i.e. V8 collected
  // the holder and we got here from WeakCallback. If it is null the drain hook
  // is walking the list and has already detached us.
  if (pprev_ != nullptr) {
    *pprev_ = next_;
    if (next_ != nullptr) next_->pprev_ = pprev_;
  }
}

void CtxWrap::WeakCallback(const v8::WeakCallbackInfo<CtxWrap>& data) {
  Destroy(data.GetParameter());
}

void CtxWrap::Wrap(Local<Object> holder) {
  Isolate* isolate = Isolate::GetCurrent();
  SetAlignedPointerInInternalField(holder, 0, record());
  handle_.Reset(isolate, holder);
  handle_.SetWeak(this, &WeakCallback, v8::WeakCallbackType::kParameter);
  next_ = g_live_ctx_wraps;
  pprev_ = &g_live_ctx_wraps;
  if (next_ != nullptr) next_->pprev_ = &next_;
  g_live_ctx_wraps = this;
}

CtxWrap* CtxWrap::Unwrap(Local<Object> holder) {
  if (holder->InternalFieldCount() < 1) return nullptr;
  void* record = GetAlignedPointerFromInternalField(*holder, 0);
  if (record == nullptr) return nullptr;
  return FromRecord(record);
}

CtxWrap::CtxWrap(size_t capacity)
    : capacity_(capacity),
      truncated_(false),
      encoding_(false),
      pprev_(nullptr),
      next_(nullptr) {}

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
  CtxWrap* self = CtxWrap::Create(capacity);
  if (self == nullptr) {
    isolate->ThrowError("allocation failed");
    return;
  }
  self->truncated_ = truncated;
  OtelThreadCtxRecord* record = self->record();
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

  // Only now does the record become reachable — Wrap() is what publishes
  // its address into the holder's internal field.
  self->Wrap(args.This());
  args.GetReturnValue().Set(args.This());
}

// `appendAttributes(attributes)`: validate and encode the attributes, then
// hand the encoded bytes to AppendEncoded to splice onto the active record.
void CtxWrap::Append(const FunctionCallbackInfo<Value>& args) {
  Isolate* isolate = args.GetIsolate();
  Local<Context> context = isolate->GetCurrentContext();

  CtxWrap* self = CtxWrap::Unwrap(args.This());
  if (!self) {
    isolate->ThrowError("not a ThreadContext");
    return;
  }
  if (args.Length() != 1) {
    isolate->ThrowError("append expects 1 argument: attributes");
    return;
  }

  // Reject reentrant Append on the same wrap. EncodeAttrs' element getters
  // and `ToString` below can execute user JS, and if that JS calls
  // `appendAttributes` on this same ThreadContext, the reentrant call would
  // grow attrs_data_size out from under the outer call's snapshot of it,
  // causing the outer memcpy to overwrite the reentrant call's bytes and
  // the outer attrs_data_size write to shrink the record.
  if (self->encoding_) {
    isolate->ThrowError(
        "reentrant appendAttributes on the same ThreadContext is not allowed");
    return;
  }

  std::vector<uint8_t> appended;
  bool truncated = false;
  self->encoding_ = true;
  const bool encoded = EncodeAttrs(isolate,
                                   context,
                                   args[0],
                                   self->record()->attrs_data_size,
                                   &appended,
                                   &truncated);
  // EncodeAttrs was the only thing that can run user JS, so the
  // reentrancy guard had to span only it.
  self->encoding_ = false;
  if (!encoded) {
    return;
  }
  if (truncated) self->truncated_ = true;

  // Nothing to append — either the input array was empty, every slot was
  // null/undefined, or every entry was dropped because the record is
  // already at the cap.
  if (appended.empty()) return;

  if (!self->AppendEncoded(args.This(), appended)) {
    isolate->ThrowError("allocation failed");
  }
}

// Splice `appended` onto the end of the record: in place if the bytes fit in
// the current allocation's slack, otherwise by moving the whole wrap to a
// larger allocation (grown geometrically), keeping invariant
// `record()->attrs_data_size <= capacity_`. See the declaration for the
// `this`-is-destroyed caveat on the growing path.
bool CtxWrap::AppendEncoded(Local<Object> holder,
                            const std::vector<uint8_t>& appended) {
  const size_t current_used = record()->attrs_data_size;
  const size_t new_used = current_used + appended.size();
  // EncodeAttrs already enforced the cap; new_used <= MAX_ATTRS_DATA_SIZE.

  if (new_used <= capacity_) {
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
    memcpy(
        &record()->attrs_data[current_used], appended.data(), appended.size());
    std::atomic_signal_fence(std::memory_order_release);
    *reinterpret_cast<volatile uint16_t*>(&record()->attrs_data_size) =
        static_cast<uint16_t>(new_used);
    return true;
  }

  // Doesn't fit. Reallocate with geometric growth with cap. The record lives
  // inside the CtxWrap allocation, so growing it means moving the CtxWrap
  // too: build a replacement, hand it the same holder object, and retire the
  // old one.
  size_t new_cap =
      std::min(std::max(capacity_ * 2, new_used), MAX_ATTRS_DATA_SIZE);

  CtxWrap* new_self = CtxWrap::Create(new_cap);
  if (new_self == nullptr) return false;
  new_self->truncated_ = truncated_;
  // Copy the existing record (header + already-written attrs_data).
  memcpy(
      new_self->record(), record(), sizeof(OtelThreadCtxRecord) + current_used);
  // Append the new entries and update attrs_data_size.
  memcpy(&new_self->record()->attrs_data[current_used],
         appended.data(),
         appended.size());
  new_self->record()->attrs_data_size = static_cast<uint16_t>(new_used);

  // Publish: the internal-field store inside Wrap() is the atomic boundary
  // the reader sees. The first fence keeps the new_self content writes
  // ordered before that store from the compiler's perspective. The second
  // fence prevents the free() inside Destroy() from being hoisted above it —
  // without it, a reader stopped between a reordered free() and the
  // not-yet-completed store would follow the internal field into freed
  // memory. OTEP signal-handler semantics (the writer is stopped during
  // reads) take care of CPU-side ordering and make immediate freeing of the
  // old block safe.
  std::atomic_signal_fence(std::memory_order_release);
  new_self->Wrap(holder);
  std::atomic_signal_fence(std::memory_order_acq_rel);
  // Destroying the old wrap runs ~Global on its handle_, which resets the
  // weak handle and so cancels the WeakCallback V8 would otherwise fire on
  // the freed block.
  CtxWrap::Destroy(this);
  return true;
}

// Mark this record's `valid` byte as 0 in place. Every async-context
// frame that holds this ThreadContext reference — including those that
// merely inherited it verbatim from a parent frame — will subsequently
// present the same shared record to a reader, so this one write drops
// the record out of scope for every such frame at once. Intended for
// span-finish, where clearing the current frame's context via
// `clearContext()` alone leaves sibling / detached-continuation frames
// still exposing the finished span. Idempotent; safe to call multiple
// times.
void CtxWrap::Invalidate(const FunctionCallbackInfo<Value>& args) {
  CtxWrap* self = CtxWrap::Unwrap(args.This());
  if (!self) {
    args.GetIsolate()->ThrowError("not a ThreadContext");
    return;
  }
  std::atomic_signal_fence(std::memory_order_release);
  *reinterpret_cast<volatile uint8_t*>(&self->record()->valid) = 0;
}

// Returns true if any attribute was ever dropped from this wrapper's
// record because it would have pushed attrs_data past the cap — set during
// CtxWrap::New() if the initial set didn't fit, or by any subsequent
// CtxWrap::Append() call.
void CtxWrap::IsTruncated(const FunctionCallbackInfo<Value>& args) {
  CtxWrap* self = CtxWrap::Unwrap(args.This());
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
  CtxWrap* self = CtxWrap::Unwrap(args.This());
  if (!self) {
    isolate->ThrowError("not a ThreadContext");
    return;
  }
  const size_t total =
      sizeof(OtelThreadCtxRecord) + self->record()->attrs_data_size;
  Local<v8::ArrayBuffer> buf = v8::ArrayBuffer::New(isolate, total);
  memcpy(buf->GetBackingStore()->Data(), self->record(), total);
  args.GetReturnValue().Set(Uint8Array::New(buf, 0, total));
}

void CtxWrap::Init(Local<Object> exports) {
  Isolate* isolate = Isolate::GetCurrent();
  node::AddEnvironmentCleanupHook(isolate, DrainLive, isolate);
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
      String::NewFromUtf8Literal(isolate, "invalidate"),
      FunctionTemplate::New(isolate, Invalidate));
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
  // hook on these versions via isAsyncContextFrameActive, so StoreAls is
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
// out-of-process reader can decode V8's JSObject / internal hashmap
// layout without doing its own V8-internal-symbol lookups for the
// pointer-compression / sandbox state. Note that nothing published here
// describes our own wrapper: internal field 0 points straight at the
// record, so the reader needs no offset of ours to reach it.
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
  auto publish_int = [&](const char* name, int value) {
    exports
        ->Set(ctx,
              String::NewFromUtf8(isolate, name).ToLocalChecked(),
              Integer::New(isolate, value))
        .FromJust();
  };
  publish_int("otelThreadCtxJsMapTableOffset", JS_MAP_TABLE_OFFSET);
  publish_int("otelThreadCtxOrderedHashMapHeaderSize",
              ORDERED_HASH_MAP_HEADER_SIZE);
  publish_int("otelThreadCtxTaggedSize", TAGGED_SIZE);
  publish_int("otelThreadCtxWrappedObjectOffset", WRAPPED_OBJECT_OFFSET);
}

}  // namespace dd
