/*
 * Copyright (C) 2005-2016 Christoph Rupp (chris@crupp.de).
 * All Rights Reserved.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * See the file COPYING for License information.
 */

/*
 * Fixed length KeyList for built-in data types ("POD types")
 *
 * This is the fastest KeyList available. It stores POD data sequentially
 * in an array, i.e. PodKeyList<uint32_t> is simply a plain
 * C array of type uint32_t[]. Each key has zero overhead.
 *
 * This KeyList cannot be resized.
 */

#ifndef UPS_BTREE_KEYS_POD_H
#define UPS_BTREE_KEYS_POD_H

#include "0root/root.h"

#include <sstream>
#include <iostream>

// Always verify that a file of level N does not include headers > N!
#include "1globals/globals.h"
#include "1base/dynamic_array.h"
#include "2page/page.h"
#include "3btree/btree_node.h"
#include "3btree/btree_keys_base.h"

#ifndef UPS_ROOT_H
#  error "root.h was not included"
#endif

namespace upscaledb {

//
// The template classes in this file are wrapped in a separate namespace
// to avoid naming clashes with btree_impl_default.h
//
namespace PaxLayout {

//
// The PodKeyList provides simplified access to a list of keys where each
// key is of type T (i.e. uint32_t).
//
template<typename T>
class PodKeyList : public BaseKeyList
{
  public:
    enum {
      // A flag whether this KeyList has sequential data
      kHasSequentialData = 1,

      // A flag whether this KeyList supports the scan() call
      kSupportsBlockScans = 1,

      // This KeyList has a custom find() implementation
      kCustomFind = 1,

      // This KeyList has a custom find_lower_bound() implementation
      kCustomFindLowerBound = 1,
    };

    // Constructor
    PodKeyList(LocalDatabase *db)
      : m_data(0) {
    }

    // Creates a new PodKeyList starting at |ptr|, total size is
    // |range_size| (in bytes)
    void create(uint8_t *data, size_t range_size) {
      m_data = (T *)data;
      m_range_size = range_size;
    }

    // Opens an existing PodKeyList starting at |ptr|
    void open(uint8_t *data, size_t range_size, size_t node_count) {
      m_data = (T *)data;
      m_range_size = range_size;
    }

    // Returns the required size for the current set of keys
    size_t get_required_range_size(size_t node_count) const {
      return (node_count * sizeof(T));
    }

    // Returns the actual key size including overhead
    size_t get_full_key_size(const ups_key_t *key = 0) const {
      return (sizeof(T));
    }

    // Finds a key
#ifdef __SSE__
    // Searches the node for the key and returns the slot of this key
    // - only for exact matches!
    //
    // This is the SIMD implementation. If SIMD is disabled then the
    // BaseKeyList::find method is used.
    template<typename Cmp>
    int find(Context *context, size_t node_count, const ups_key_t *key,
                    Cmp &comparator) {
      return (find_simd_sse<T>(node_count, &m_data[0], key));
    }
#else
    template<typename Cmp>
    int find(Context *context, size_t node_count, const ups_key_t *hkey,
                    Cmp &comparator) {
      T key = *(T *)hkey->data;
      T *result = std::lower_bound(&m_data[0], &m_data[node_count], key);
      if (result == &m_data[node_count] || *result != key)
        return (-1);
      return (result - &m_data[0]);
    }
#endif

    // Performs a lower-bound search for a key
    template<typename Cmp>
    int find_lower_bound(Context *context, size_t node_count,
                    const ups_key_t *hkey, Cmp &comparator, int *pcmp) {
      T key = *(T *)hkey->data;
      T *result = std::lower_bound(&m_data[0], &m_data[node_count], key);
      if (result == &m_data[node_count]) {
        if (key > m_data[node_count - 1]) {
          *pcmp = +1;
          return (node_count - 1);
        }
        if (key < m_data[0]) {
          *pcmp = -1;
          return (0);
        }
        assert(!"shouldn't be here");
        throw Exception(UPS_INTERNAL_ERROR);
      }

      if (key > *result)
        *pcmp = +1;
      else if (key < *result) {
        *pcmp = +1;
        result--;
      }
      else
        *pcmp = 0;
      return (result - &m_data[0]);
    }

    // Copies a key into |dest|
    void get_key(Context *context, int slot, ByteArray *arena, ups_key_t *dest,
                    bool deep_copy = true) const {
      dest->size = sizeof(T);
      if (deep_copy == false) {
        dest->data = &m_data[slot];
        return;
      }

      // allocate memory (if required)
      if (!(dest->flags & UPS_KEY_USER_ALLOC)) {
        arena->resize(dest->size);
        dest->data = arena->data();
      }

      ::memcpy(dest->data, &m_data[slot], sizeof(T));
    }

    // Iterates all keys, calls the |visitor| on each
    ScanResult scan(ByteArray *arena, size_t node_count, uint32_t start) {
      return std::make_pair(&m_data[start], node_count - start);
    }

    // Erases a whole slot by shifting all larger keys to the "left"
    void erase(Context *context, size_t node_count, int slot) {
      if (slot < (int)node_count - 1)
        ::memmove(&m_data[slot], &m_data[slot + 1],
                        sizeof(T) * (node_count - slot - 1));
    }

    // Inserts a key
    template<typename Cmp>
    PBtreeNode::InsertResult insert(Context *context, size_t node_count,
                    const ups_key_t *key, uint32_t flags, Cmp &comparator,
                    int slot) {
      if (node_count > (size_t)slot)
        ::memmove(&m_data[slot + 1], &m_data[slot],
                        sizeof(T) * (node_count - slot));
      set_key_data(slot, key->data, key->size);
      return (PBtreeNode::InsertResult(0, slot));
    }

    // Copies |count| key from this[sstart] to dest[dstart]
    void copy_to(int sstart, size_t node_count, PodKeyList<T> &dest,
                    size_t other_count, int dstart) {
      ::memcpy(&dest.m_data[dstart], &m_data[sstart],
                      sizeof(T) * (node_count - sstart));
    }

    // Returns true if the |key| no longer fits into the node
    bool requires_split(size_t node_count, const ups_key_t *key) const {
      return ((node_count + 1) * sizeof(T) >= m_range_size);
    }

    // Change the range size; just copy the data from one place to the other
    void change_range_size(size_t node_count, uint8_t *new_data_ptr,
            size_t new_range_size, size_t capacity_hint) {
      ::memmove(new_data_ptr, m_data, node_count * sizeof(T));
      m_data = (T *)new_data_ptr;
      m_range_size = new_range_size;
    }

    // Fills the btree_metrics structure
    void fill_metrics(btree_metrics_t *metrics, size_t node_count) {
      BaseKeyList::fill_metrics(metrics, node_count);
      BtreeStatistics::update_min_max_avg(&metrics->keylist_unused,
              m_range_size - (node_count * sizeof(T)));
    }

    // Prints a slot to |out| (for debugging)
    void print(Context *context, int slot, std::stringstream &out) const {
      out << m_data[slot];
    }

    // Returns the size of a key
    size_t get_key_size(int slot) const {
      return (sizeof(T));
    }

    // Returns a pointer to the key's data
    uint8_t *get_key_data(int slot) {
      return ((uint8_t *)&m_data[slot]);
    }

  private:
    // Returns a pointer to the key's data (const flavour)
    uint8_t *get_key_data(int slot) const {
      return ((uint8_t *)&m_data[slot]);
    }

    // Overwrites an existing key; the |size| of the new data HAS to be
    // identical with the key size specified when the database was created!
    void set_key_data(int slot, const void *ptr, size_t size) {
      assert(size == sizeof(T));
      m_data[slot] = *(T *)ptr;
    }

    // The actual array of T's
    T *m_data;
};

} // namespace PaxLayout

} // namespace upscaledb

#endif /* UPS_BTREE_KEYS_POD_H */
