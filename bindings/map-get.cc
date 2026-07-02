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

// Find a value in a JavaScript map by directly reading the underlying V8 hash
// map.
//
// V8 defines two internal hash map representations: a compact
// SmallOrderedHashMap (for low-cardinality maps) and the regular
// OrderedHashMap. However, a JS Map always uses the regular OrderedHashMap:
// V8's Map constructor hardcodes AllocateOrderedHashMap() and the "start small,
// then promote" path that could ever install a SmallOrderedHashMap
// (OrderedHashMapHandler) is test-only, never used by the JSMap/JSSet builtins.
// We only ever read AsyncContextFrame maps (the CPED map), which are ordinary
// JS Maps, so we only handle the OrderedHashMap layout here.

#include <cstdint>

namespace dd {

using Address = uintptr_t;

#ifndef _WIN32
// ============================================================================
// Constants from V8 internals
// ============================================================================

// Heap object tagging
constexpr int kHeapObjectTag = 1;

// OrderedHashMap constants
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
  // Tagged pointer to an OrderedHashMapLayout
  Address table_;
};

// V8 FixedArray: length_ is a Smi, followed by that many element slots
struct FixedArrayLayout {
  HeapObjectLayout header_;  // FixedArray is a HeapObject
  Address length_;
  Address elements_[0];
};

// OrderedHashMap layout
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

// ============================================================================
// Hash Table Lookup
// ============================================================================

// Find an entry by a key and its hash in an OrderedHashMap.
int FindEntryByHash(const OrderedHashMapLayout* layout,
                    int hash,
                    Address key_to_find) {
  const int entry_count = layout->GetEntryCount();
  const int bucket = hash & (layout->NumberOfBuckets() - 1);
  int entry = layout->GetFirstEntry(bucket);

  // Paranoid: by never traversing more than the total number of entries in the
  // map we guarantee this terminates in bound time even if for some unforeseen
  // reason the chain is cyclical. Also, every entry value must be between
  // [0, GetEntryCount).
  for (int max_entries_left = entry_count;
       entry != OrderedHashMapLayout::kNotFoundValue && entry >= 0 &&
       entry < entry_count && max_entries_left > 0;
       max_entries_left--) {
    Address key_at_entry = layout->GetKey(entry);
    if (key_at_entry == key_to_find) {
      return entry;
    }
    entry = layout->GetNextChainEntry(entry);
  }

  return kNotFound;
}

// Find an entry by a key and its hash in an OrderedHashMap, and return its
// value or the zero address if it is not found.
Address FindValueByHash(const OrderedHashMapLayout* layout,
                        int hash,
                        Address key_to_find) {
  auto entry = FindEntryByHash(layout, hash, key_to_find);
  return entry == kNotFound ? 0 : layout->GetValue(entry);
}

static bool IsOrderedHashMap(Address table_untagged) {
  const OrderedHashMapLayout* layout =
      reinterpret_cast<const OrderedHashMapLayout*>(table_untagged);

  // Let's validate its invariants!

  // Its length must be a Smi.
  if (!IsSmi(layout->fixedArray_.length_)) return false;
  auto length = SmiToInt(layout->fixedArray_.length_);

  // Must have at least 3 elements for number_of_* fields.
  if (length < 3) return false;

  // All of them must be Smis
  if (!IsSmi(layout->number_of_buckets_) ||
      !IsSmi(layout->number_of_deleted_elements_) ||
      !IsSmi(layout->number_of_elements_))
    return false;
  auto num_buckets = SmiToInt(layout->number_of_buckets_);
  auto num_deleted = SmiToInt(layout->number_of_deleted_elements_);
  auto num_elements = SmiToInt(layout->number_of_elements_);

  // num_buckets must be a power of 2
  if (num_buckets <= 0 || (num_buckets & (num_buckets - 1)) != 0) return false;
  auto capacity = num_buckets * kLoadFactor;

  // number of elements and number of deleted elements can't be negative, and
  // they can't add up to more than the capacity.
  if (num_elements < 0 || num_deleted < 0 ||
      num_elements + num_deleted > capacity)
    return false;

  // The length of the array must be enough to store the whole map.
  return length >=
         3 + num_buckets + OrderedHashMapLayout::kEntrySize * capacity;
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

  if (IsOrderedHashMap(table_untagged)) {
    const OrderedHashMapLayout* layout =
        reinterpret_cast<const OrderedHashMapLayout*>(table_untagged);
    return FindValueByHash(layout, hash, key);
  }
  return 0;  // We couldn't determine the kind of the map, just return zero.
}

#else  // _WIN32

Address GetValueFromMap(Address map_addr, int hash, Address key) {
  return 0;
}

#endif  // _WIN32
}  // namespace dd
