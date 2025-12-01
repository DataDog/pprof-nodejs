/**
 * Copyright 2025 Datadog. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "map-get.hh"

// Find a value in a JavaScript Map using pointer arithmetic and hash.
//
// V8 uses TWO internal table representations:
//   1. SmallOrderedHashMap: For small maps (capacity 4-254)
//      - Metadata stored as uint8_t bytes
//      - Entry size: 2 (key, value)
//      - Chain table separate from entries
//
//   2. OrderedHashMap: For larger maps (capacity >254)
//      - Metadata stored as Smis in FixedArray
//      - Entry size: 3 (key, value, chain)
//      - Chain stored inline with entries
//
// This code handles both types by detecting the table format at runtime.
// Anecdotally, we're only seeing maps with the larger memory layout even when
// they only have few elements, though.

#include <cstdint>
#include <iostream>

namespace dd {

using Address = uintptr_t;

// ============================================================================
// Constants from V8 internals
// ============================================================================

// Heap object tagging
constexpr int kHeapObjectTag = 1;
constexpr int kApiTaggedSize = sizeof(void*);

// JSMap structure
constexpr int kJSMapTableOffset = 3 * kApiTaggedSize;  // JSObject::kHeaderSize

// OrderedHashMap structure (from src/objects/ordered-hash-table.h)
// Memory layout:
//   [0]: map pointer (HeapObject)
//   [1]: number_of_elements (Smi)
//   [2]: number_of_deleted_elements (Smi)
//   [3]: number_of_buckets (Smi)
//   [4..4+buckets-1]: hash table (array of entry indices, each is a Smi)
//   [4+buckets...]: data table (entries)
//
// Each entry is 3 elements (kEntrySize = 3):
//   [0]: key (Tagged Object)
//   [1]: value (Tagged Object)  ← kValueOffset
//   [2]: chain (Smi - next entry index or -1) ← kChainOffset

constexpr int kNumberOfElementsIndex = 0;
constexpr int kNumberOfDeletedElementsIndex = 1;
constexpr int kNumberOfBucketsIndex = 2;
constexpr int kHashTableStartIndex = 3;

constexpr int kEntrySize = 3;
constexpr int kKeyOffset = 0;
constexpr int kValueOffset = 1;
constexpr int kChainOffset = 2;

constexpr int kNotFound = -1;

// SmallOrderedHashMap structure (for small maps, capacity 4-254)
// Memory layout (stores metadata as uint8_t, not Smis):
//   [0]: map pointer (HeapObject)
//   [kHeaderSize + 0]: number_of_elements (uint8)
//   [kHeaderSize + 1]: number_of_deleted_elements (uint8)
//   [kHeaderSize + 2]: number_of_buckets (uint8)
//   [kHeaderSize + 3...]: padding (5 bytes on 64-bit, 1 byte on 32-bit)
//   [DataTableStartOffset...]: data table (key-value pairs as Tagged)
//   [...]: hash table (uint8 bucket indices)
//   [...]: chain table (uint8 next entry indices)
//
// Each entry is 2 Tagged elements (kSmallEntrySize = 2):
//   [0]: key (Tagged Object)
//   [1]: value (Tagged Object)

constexpr int kSmallEntrySize = 2;
constexpr int kSmallKeyOffset = 0;
constexpr int kSmallValueOffset = 1;
constexpr int kSmallNotFound = 0xFF;
constexpr int kSmallLoadFactor = 2;

// Offsets for SmallOrderedHashMap
constexpr int kSmallNumberOfElementsOffset = kApiTaggedSize;  // After map field
constexpr int kSmallNumberOfDeletedElementsOffset =
    kSmallNumberOfElementsOffset + 1;
constexpr int kSmallNumberOfBucketsOffset =
    kSmallNumberOfDeletedElementsOffset + 1;
constexpr int kSmallPaddingOffset = kSmallNumberOfBucketsOffset + 1;

// Calculate DataTableStartOffset
constexpr int kSmallPaddingSize = 5;
constexpr int kSmallDataTableStartOffset =
    kSmallPaddingOffset + kSmallPaddingSize;

// ============================================================================
// Helper Functions
// ============================================================================

inline Address UntagPointer(Address tagged) {
  return tagged - kHeapObjectTag;
}

inline Address ReadTaggedField(Address obj, int offset) {
  return *reinterpret_cast<Address*>(obj + offset);
}

inline bool IsSmi(Address value) {
  return (value & 1) == 0;
}

inline int SmiToInt(Address smi) {
  // Conversion below valid only on 64-bit platforms, the only ones we care
  // about.
  static_assert(sizeof(void*) == 8);

  return static_cast<int>(static_cast<intptr_t>(smi) >> 32);
}

// Read a field from FixedArray at given index
inline Address GetFixedArrayElement(Address array_untagged, int index) {
  // FixedArray layout: [map, length, element0, element1, ...]
  // Elements start at offset 2 * kApiTaggedSize
  int offset = (2 + index) * kApiTaggedSize;
  return ReadTaggedField(array_untagged, offset);
}

// Read a uint8_t byte from object
inline uint8_t ReadByte(Address obj_untagged, int offset) {
  return *reinterpret_cast<uint8_t*>(obj_untagged + offset);
}

// Read an Address field at byte offset
inline Address ReadTaggedAtOffset(Address obj_untagged, int offset) {
  return *reinterpret_cast<Address*>(obj_untagged + offset);
}

// ============================================================================
// SmallOrderedHashMap Inspector (for small maps)
// ============================================================================

class SmallOrderedHashMapInspector {
 public:
  explicit SmallOrderedHashMapInspector(Address table_tagged) {
    table_addr_ = UntagPointer(table_tagged);

    uint64_t smallHeader = *reinterpret_cast<uint64_t*>(
        table_addr_ + kSmallNumberOfElementsOffset);
    auto num_elements = smallHeader & 0xFF;
    auto num_deleted_elements = (smallHeader >> 1) & 0xFF;
    num_buckets_ = (smallHeader >> 2) & 0xFF;
    max_chain_length_ = num_elements + num_deleted_elements;
  }

  int NumberOfBuckets() const { return num_buckets_; }
  int Capacity() const { return num_buckets_ * kSmallLoadFactor; }

  int HashToBucket(int hash) const { return hash & (num_buckets_ - 1); }

  // Get buckets start offset (after data table)
  int GetBucketsStartOffset() const {
    int capacity = Capacity();
    int data_table_size = capacity * kSmallEntrySize * kApiTaggedSize;
    return kSmallDataTableStartOffset + data_table_size;
  }

  // Get chain table offset (after hash table)
  int GetChainTableOffset() const {
    return GetBucketsStartOffset() + num_buckets_;
  }

  // Get first entry in bucket
  uint8_t GetFirstEntry(int bucket) const {
    int offset = GetBucketsStartOffset() + bucket;
    return ReadByte(table_addr_, offset);
  }

  // Get data entry offset
  int GetDataEntryOffset(int entry, int relative_index) const {
    int offset_in_datatable = entry * kSmallEntrySize * kApiTaggedSize;
    int offset_in_entry = relative_index * kApiTaggedSize;
    return kSmallDataTableStartOffset + offset_in_datatable + offset_in_entry;
  }

  // Read key at entry
  Address GetKey(int entry) const {
    int offset = GetDataEntryOffset(entry, kSmallKeyOffset);
    return ReadTaggedAtOffset(table_addr_, offset);
  }

  // Read value at entry
  Address GetValue(int entry) const {
    int offset = GetDataEntryOffset(entry, kSmallValueOffset);
    return ReadTaggedAtOffset(table_addr_, offset);
  }

  // Read next chain entry
  int GetNextChainEntry(int entry) const {
    int offset = GetChainTableOffset() + entry;
    return ReadByte(table_addr_, offset);
  }

  // Find entry by hash
  int FindEntryByHash(int hash, Address key_to_find) const {
    int bucket = HashToBucket(hash);
    int entry = GetFirstEntry(bucket);

    // Paranoid: by never traversing more than the sum of elements and deleted
    // elements we guarantee this terminates in bound time even if for some
    // unforeseen reason the chain is cyclical.
    for (int max_chain_left = max_chain_length_;
         entry != kSmallNotFound && max_chain_left > 0;
         max_chain_left--) {
      Address key_at_entry = GetKey(entry);
      if (key_at_entry == key_to_find) {
        return entry;
      }
      entry = GetNextChainEntry(entry);
    }

    return -1;  // Return -1 for consistency with OrderedHashMap
  }

 private:
  Address table_addr_;
  int num_buckets_;
  int max_chain_length_;
};

// ============================================================================
// OrderedHashMap Inspector (for regular/large maps)
// ============================================================================

class OrderedHashMapInspector {
 public:
  explicit OrderedHashMapInspector(Address table_tagged) {
    table_addr_ = UntagPointer(table_tagged);

    // Read metadata
    Address num_elements =
        GetFixedArrayElement(table_addr_, kNumberOfElementsIndex);
    Address num_deleted_elements =
        GetFixedArrayElement(table_addr_, kNumberOfDeletedElementsIndex);
    Address num_buckets =
        GetFixedArrayElement(table_addr_, kNumberOfBucketsIndex);

    if (IsSmi(num_buckets) && IsSmi(num_elements) &&
        IsSmi(num_deleted_elements)) {
      num_buckets_ = SmiToInt(num_buckets);
      max_chain_length_ =
          SmiToInt(num_elements) + SmiToInt(num_deleted_elements);
    } else {
      num_buckets_ = 0;
      max_chain_length_ = 0;
    }
  }

  int NumberOfBuckets() const { return num_buckets_; }

  // Convert hash to bucket index
  int HashToBucket(int hash) const { return hash & (num_buckets_ - 1); }

  // Get the first entry index for a given bucket
  int GetFirstEntry(int bucket) const {
    Address entry_smi =
        GetFixedArrayElement(table_addr_, kHashTableStartIndex + bucket);
    if (IsSmi(entry_smi)) {
      return SmiToInt(entry_smi);
    }
    return kNotFound;
  }

  // Convert entry index to FixedArray index
  int EntryToIndex(int entry) const {
    return kHashTableStartIndex + num_buckets_ + (entry * kEntrySize);
  }

  // Read key at entry
  Address GetKey(int entry) const {
    int index = EntryToIndex(entry);
    return GetFixedArrayElement(table_addr_, index + kKeyOffset);
  }

  // Read value at entry
  Address GetValue(int entry) const {
    int index = EntryToIndex(entry);
    return GetFixedArrayElement(table_addr_, index + kValueOffset);
  }

  // Read next chain entry
  int GetNextChainEntry(int entry) const {
    int index = EntryToIndex(entry);
    Address chain_smi = GetFixedArrayElement(table_addr_, index + kChainOffset);
    if (IsSmi(chain_smi)) {
      return SmiToInt(chain_smi);
    }
    return kNotFound;
  }

  // Find entry by hash (requires key comparison)
  // Returns entry index or kNotFound
  int FindEntryByHash(int hash, Address key_to_find) const {
    int bucket = HashToBucket(hash);
    int entry = GetFirstEntry(bucket);

    // Paranoid: by never traversing more than the sum of elements and deleted
    // elements we guarantee this terminates in bound time even if for some
    // unforeseen reason the chain is cyclical or runs off (although we'd
    // probably get a segfault before that happened in the latter case.)
    for (int max_chain_left = max_chain_length_;
         entry != kNotFound && max_chain_left > 0;
         max_chain_left--) {
      Address key_at_entry = GetKey(entry);
      if (key_at_entry == key_to_find) {
        return entry;
      }
      entry = GetNextChainEntry(entry);
    }

    return kNotFound;
  }

 private:
  Address table_addr_;
  int num_buckets_;
  int max_chain_length_;
};

// ============================================================================
// Complete Map Lookup Implementation
// ============================================================================

// Detect if the table is an OrderedHashMap or a SmallOrderedHashMap (or it can
// not safely be determined) by checking padding bytes. SmallOrderedHashMap has
// always-zero padding bytes after the metadata:
//   [0]: number_of_elements (uint8)
//   [1]: number_of_deleted_elements (uint8)
//   [2]: number_of_buckets (uint8)
//   [3-7]: padding bytes (always zero)
// OrderedHashMap (FixedArray) would have non-zero Smi value for the size of the
// FixedArray in this region.
// So: small map value can be masked with FFFFFFFFFF000000 and ordinary map
// value could be masked with 00000000FFFFFFFF (on a 64-bit little-endian
// build.)
static uint8_t GetOrderedHashMapType(Address table_tagged) {
  Address table_addr =
      (UntagPointer(table_tagged) + kSmallNumberOfElementsOffset);

  uint64_t smallHeader = *reinterpret_cast<uint64_t*>(table_addr);
  static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__);
  // small map will have some bits in bytes 0-2 be nonzero, and all bits in
  // bytes 3-7 zero. That effectively limits the value range of smallHeader to
  // [0x1-0xFFFFFF].
  if (smallHeader > 0 && smallHeader < 0x1000000) {
    auto num_elements = smallHeader & 0xFF;
    auto num_deleted = (smallHeader >> 1) & 0xFF;
    auto num_buckets = (smallHeader >> 2) & 0xFF;
    // SmallOrderedHashMap has constraints:
    // - num_buckets must be a power of 2 between 2 and 127
    // - num_elements + num_deleted <= capacity (buckets * 2)
    if (num_buckets >= 2 && num_buckets <= 127) {
      // Check if num_buckets is a power of 2
      if ((num_buckets & (num_buckets - 1)) == 0) {
        auto capacity = num_buckets * kSmallLoadFactor;
        if (num_elements + num_deleted <= capacity) {
          return 1;  // small map
        }
      }
    }
    return 2;  // undecided
  }
  return 0;  // large map
}

// Lookup value in a Map given the hash and key pointer. If the key is not found
// in the map (or the lookup can not be performed) returns a zero Address (which
// is essentially a zero Smi value.)
Address GetValueFromMap(Address map_addr, int hash, Address key) {
  Address map_untagged = UntagPointer(map_addr);
  Address table_tagged = ReadTaggedField(map_untagged, kJSMapTableOffset);

  switch (GetOrderedHashMapType(table_tagged)) {
    case 0: {
      OrderedHashMapInspector inspector(table_tagged);
      auto entry = inspector.FindEntryByHash(hash, key);
      return entry == kNotFound ? 0 : inspector.GetValue(entry);
    }
    case 1: {
      SmallOrderedHashMapInspector inspector(table_tagged);
      auto entry = inspector.FindEntryByHash(hash, key);
      return entry == kNotFound ? 0 : inspector.GetValue(entry);
    }
  }
  return 0;  // We couldn't determine the kind of the map, just return zero.
}

}  // namespace dd
