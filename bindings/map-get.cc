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

// Find a value in JavaScript map by directly reading the underlying V8 hash
// map.
//
// V8 uses TWO internal hash map representations:
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
// Practical testing shows that at least the AsyncContextFrame maps use the
// large map format even for small cardinality maps, but just in case we handle
// both.

#include <cstdint>

namespace dd {

using Address = uintptr_t;

#ifndef _WIN32
// ============================================================================
// Constants from V8 internals
// ============================================================================

// Heap object tagging
constexpr int kHeapObjectTag = 1;

// OrderedHashMap/SmallOrderedHashMap shared constants
constexpr int kNotFound = -1;
constexpr int kLoadFactor = 2;

// ============================================================================
// Helper Functions (needed by struct methods)
// ============================================================================

inline Address UntagPointer(Address tagged) {
  return tagged - kHeapObjectTag;
}

// IsSmi and SmiToInt implementations are valid only on 64-bit platforms, the
// only ones we support.
static_assert(sizeof(void*) == 8, "Only 64-bit platforms supported");

inline bool IsSmi(Address value) {
  // More rigorous than (value & 1) but valid on 64-bit platforms only.
  return (value & 0xFFFFFFFF) == 0;
}

inline int SmiToInt(Address smi) {
  return static_cast<int>(static_cast<intptr_t>(smi) >> 32);
}

// ============================================================================
// V8 Hashtable Structure Definitions
// ============================================================================

// HeapObject layout - base for all V8 heap objects
// From v8/src/objects/heap-object.h
struct HeapObjectLayout {
  Address classMap_;  // Tagged pointer to the class map
};

// JavaScript Map object
struct JSMapLayout {
  HeapObjectLayout header_;     // Map is a HeapObject
  Address properties_or_hash_;  // not used by us
  Address elements_;            // not used by us
  // Tagged pointer to a [Small]OrderedHashMapLayout
  Address table_;
};

// V8 FixedArray: length_ is a Smi, followed by that many element slots
struct FixedArrayLayout {
  HeapObjectLayout header_;  // FixedArray is a HeapObject
  Address length_;
  Address elements_[0];
};

// NOTE: both OrderedHashMap and SmallOrderedHashMap have compatible method
// definitions so FindEntryByHash and FindValueByHash can be defined as
// templated function working on both.

// OrderedHashMap layout (for large maps, capacity >254)
// From v8/src/objects/ordered-hash-table.h
struct OrderedHashMapLayout {
  FixedArrayLayout fixedArray_;  // OrderedHashMap is a FixedArray
  // The first 3 address slots in the FixedArray that is a Hashtable are the
  // number of elements, deleted elements, and buckets. Each one is a Smi.
  Address number_of_elements_;
  Address number_of_deleted_elements_;
  Address number_of_buckets_;
  // First number_of_buckets_ entries in head_and_data_table_ is the head table:
  // each entry is an index of the first entry (head of the linked list of
  // entries) in the data table for that bucket. This is followed by the data
  // table. Each data table entry uses three (kEntrySize == 3) tagged pointer
  // slots:
  //   [0]: key (Tagged Object)
  //   [1]: value (Tagged Object)
  //   [2]: chain (Smi - next entry index or -1)
  // All indices (both to the head of the list and to the next entry are
  // expressed in number of entries from the start of the data table, so to
  // convert it into a head_and_data_table_ you need to add number_of_buckets_
  // (length of the head table) and then 3 * index.
  Address head_and_data_table_[0];  // Variable: [head_table][data_table]

  // Constants for entry structure
  static constexpr int kEntrySize = 3;
  static constexpr int kKeyOffset = 0;
  static constexpr int kValueOffset = 1;
  static constexpr int kChainOffset = 2;
  static constexpr int kNotFoundValue = kNotFound;

  // Get number of buckets (converts Smi to int)
  int NumberOfBuckets() const { return SmiToInt(number_of_buckets_); }

  int GetEntryCount() const {
    return SmiToInt(number_of_elements_) +
           SmiToInt(number_of_deleted_elements_);
  }

  // Get first entry index for a bucket
  int GetFirstEntry(int bucket) const {
    Address entry_smi = head_and_data_table_[bucket];
    return IsSmi(entry_smi) ? SmiToInt(entry_smi) : kNotFound;
  }

  // Convert entry index to head_and_data_table_ index for the entry's key
  int EntryToIndex(int entry) const {
    return NumberOfBuckets() + (entry * kEntrySize);
  }

  // Get key at entry index
  Address GetKey(int entry) const {
    int index = EntryToIndex(entry);
    return head_and_data_table_[index + kKeyOffset];
  }

  // Get value at entry index
  Address GetValue(int entry) const {
    int index = EntryToIndex(entry);
    return head_and_data_table_[index + kValueOffset];
  }

  // Get next entry in chain
  int GetNextChainEntry(int entry) const {
    int index = EntryToIndex(entry);
    Address chain_smi = head_and_data_table_[index + kChainOffset];
    return IsSmi(chain_smi) ? SmiToInt(chain_smi) : kNotFound;
  }
};

// SmallOrderedHashMap layout (for small maps, capacity 4-254)
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
// Each entry is 2 Tagged elements (kEntrySize = 2):
//   [0]: key (Tagged Object)
//   [1]: value (Tagged Object)
//
// From v8/src/objects/ordered-hash-table.h
struct SmallOrderedHashMapLayout {
  HeapObjectLayout header_;
  uint8_t number_of_elements_;
  uint8_t number_of_deleted_elements_;
  uint8_t number_of_buckets_;
  uint8_t padding_[5];  // 5 bytes on 64-bit
  // Variable length:
  // - Address data_table_[capacity * kEntrySize]  // Keys and values
  // - uint8_t hash_table_[number_of_buckets_]     // Bucket -> first entry
  // - uint8_t chain_table_[capacity]              // Entry -> next entry
  Address data_table_[0];

  // Constants for entry structure
  static constexpr int kEntrySize = 2;
  static constexpr int kKeyOffset = 0;
  static constexpr int kValueOffset = 1;
  static constexpr int kNotFoundValue = 255;

  // Get capacity from number of buckets
  int Capacity() const { return number_of_buckets_ * kLoadFactor; }

  int NumberOfBuckets() const { return number_of_buckets_; }

  int GetEntryCount() const {
    return number_of_elements_ + number_of_deleted_elements_;
  }

  const uint8_t* GetHashTable() const {
    return reinterpret_cast<const uint8_t*>(data_table_ +
                                            Capacity() * kEntrySize);
  }

  const uint8_t* GetChainTable() const {
    return GetHashTable() + number_of_buckets_;
  }

  // Get key at entry index
  Address GetKey(int entry) const {
    return data_table_[entry * kEntrySize + kKeyOffset];
  }

  // Get value at entry index
  Address GetValue(int entry) const {
    return data_table_[entry * kEntrySize + kValueOffset];
  }

  // Get first entry in bucket
  uint8_t GetFirstEntry(int bucket) const {
    const uint8_t* hash_table = GetHashTable();
    return hash_table[bucket];
  }

  // Get next entry in chain
  uint8_t GetNextChainEntry(int entry) const {
    const uint8_t* chain_table = GetChainTable();
    return chain_table[entry];
  }
};

// ============================================================================
// Templated Hash Table Lookup
// ============================================================================

// Find an entry by a key and its hash in any hash table layout
// Template parameter LayoutT should be either OrderedHashMapLayout or
// SmallOrderedHashMapLayout
template <typename LayoutT>
int FindEntryByHash(const LayoutT* layout, int hash, Address key_to_find) {
  const int entry_count = layout->GetEntryCount();
  const int bucket = hash & (layout->NumberOfBuckets() - 1);
  int entry = layout->GetFirstEntry(bucket);

  // Paranoid: by never traversing more than the total number of entries in the
  // map we guarantee this terminates in bound time even if for some unforeseen
  // reason the chain is cyclical. Also, every entry value must be between
  // [0, GetEntryCount).
  for (int max_entries_left = entry_count;
       entry != LayoutT::kNotFoundValue && entry >= 0 && entry < entry_count &&
       max_entries_left > 0;
       max_entries_left--) {
    Address key_at_entry = layout->GetKey(entry);
    if (key_at_entry == key_to_find) {
      return entry;
    }
    entry = layout->GetNextChainEntry(entry);
  }

  return kNotFound;
}

// Find an entry by a key and its hash in any hash table layout, and return its
// value or the zero address if it is not found.
// Template parameter LayoutT should be either OrderedHashMapLayout or
// SmallOrderedHashMapLayout
template <typename LayoutT>
Address FindValueByHash(const LayoutT* layout, int hash, Address key_to_find) {
  auto entry = FindEntryByHash(layout, hash, key_to_find);
  return entry == kNotFound ? 0 : layout->GetValue(entry);
}

// Detect if the table is an OrderedHashMap or a SmallOrderedHashMap (or it can
// not safely be determined) by checking padding bytes. SmallOrderedHashMap has
// always-zero padding bytes after the metadata.
static uint8_t GetOrderedHashMapType(Address table_untagged) {
  const SmallOrderedHashMapLayout* potential_small =
      reinterpret_cast<const SmallOrderedHashMapLayout*>(table_untagged);

  // Read the header as one 64-bit value for validation
  uint64_t smallHeader =
      *reinterpret_cast<const uint64_t*>(&potential_small->number_of_elements_);

  static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__,
                "Little-endian required");
  // Small map will have some bits in bytes 0-2 be nonzero, and all bits in
  // bytes 3-7 zero. That effectively limits the value range of smallHeader to
  // [0x1-0xFFFFFF].
  if (smallHeader > 0 && smallHeader < 0x1000000) {
    auto num_elements = potential_small->number_of_elements_;
    auto num_deleted = potential_small->number_of_deleted_elements_;
    auto num_buckets = potential_small->number_of_buckets_;

    // SmallOrderedHashMap has constraints:
    // - num_buckets must be a power of 2 between 2 and 127
    // - num_elements + num_deleted <= capacity (buckets * 2)
    if (num_buckets >= 2 && num_buckets <= 127) {
      // Check if num_buckets is a power of 2
      if ((num_buckets & (num_buckets - 1)) == 0) {
        auto capacity = num_buckets * kLoadFactor;
        if (num_elements + num_deleted <= capacity) {
          return 1;  // small map
        }
      }
    }
    return 2;  // undecided
  }
  // At this point, it should be an ordinary (that is, large) map. Let's
  // validate its invariants.
  const OrderedHashMapLayout* layout =
      reinterpret_cast<const OrderedHashMapLayout*>(table_untagged);
  if (IsSmi(layout->fixedArray_.length_)) {
    auto length = SmiToInt(layout->fixedArray_.length_);
    if (length > 2) {  // at least 3 for the 3 values below
      if (IsSmi(layout->number_of_buckets_) &&
          IsSmi(layout->number_of_deleted_elements_) &&
          IsSmi(layout->number_of_elements_)) {
        auto num_buckets = SmiToInt(layout->number_of_buckets_);
        auto num_deleted = SmiToInt(layout->number_of_deleted_elements_);
        auto num_elements = SmiToInt(layout->number_of_elements_);
        // Check if num_buckets is a power of 2
        if (num_buckets > 0 && (num_buckets & (num_buckets - 1)) == 0) {
          auto capacity = num_buckets * kLoadFactor;
          // Check if number of elements and deleted elements looks valid:
          // neither is non-negative and they don't add up to more than capacity
          if (num_elements >= 0 && num_deleted >= 0 &&
              num_elements + num_deleted <= capacity) {
            // Check if the fixed array is large enough to contain the whole map
            if (length >= 3 + num_buckets + 3 * capacity) {
              return 0;  // large map
            }
          }
        }
      }
    }
  }
  return 2;  // undecided
}

// ============================================================================
// Main entry point
// ============================================================================

// Lookup value in a Map given the hash and key pointer. If the key is not found
// in the map (or the lookup can not be performed) returns a zero Address (which
// is essentially a zero Smi value.)
Address GetValueFromMap(Address map_addr, int hash, Address key) {
  const JSMapLayout* map_untagged =
      reinterpret_cast<const JSMapLayout*>(UntagPointer(map_addr));
  Address table_untagged = UntagPointer(map_untagged->table_);

  switch (GetOrderedHashMapType(table_untagged)) {
    case 0: {
      const OrderedHashMapLayout* layout =
          reinterpret_cast<const OrderedHashMapLayout*>(table_untagged);
      return FindValueByHash(layout, hash, key);
    }
    case 1: {
      const SmallOrderedHashMapLayout* layout =
          reinterpret_cast<const SmallOrderedHashMapLayout*>(table_untagged);
      return FindValueByHash(layout, hash, key);
    }
  }
  return 0;  // We couldn't determine the kind of the map, just return zero.
}

#else  // _WIN32

Address GetValueFromMap(Address map_addr, int hash, Address key) {
  return 0;
}

#endif  // _WIN32
}  // namespace dd
