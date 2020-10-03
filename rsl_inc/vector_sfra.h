#pragma once

#include <cassert>
#include <cstring>
#include <functional>
#include <iostream>

#include "rlx_atomic.h"

/// A data structure which uses a sorted vector to implement a map interface.
/// Terrible asymptotes on insertion and deletion (O(n)), but fast lookup,
/// and fast at small sizes.
/// Non-concurrent. Non-resizable.
/// SFRA: Sorted, Fixed capacity, Relaxed Atomic
template <typename K, typename V, int CAPACITY> class vector_sfra {
  /// List of keys
  rlx_atomic<K> key_list[CAPACITY];

  /// List of values
  rlx_atomic<V> val_list[CAPACITY];

  /// Current number of elements in the list.
  rlx_atomic<int> size = 0;

  /// Internal function used to find a key in the vector.
  /// If it exists, it returns the exact position;
  /// Otherwise, the position it should be inserted into.
  /// A binary search that takes lg(n) time.
  int find(const K &k) const {
    int left = 0;
    int right = size - 1;

    while (right >= left) {
      // NB: logically equivalent to "int mid = (left + right)/2",
      // but does not overflow.
      int mid = left + ((right - left) / 2);

      if (key_list[mid] == k) {
        return mid;
      } else if (key_list[mid] > k) {
        right = mid - 1;
      } else {
        left = mid + 1;
      }
    }

    // key not found, but return the position where it would be
    return left;
  }

public:
  /// insert() takes a reference to a k/v pair, so we expose the type here
  typedef std::pair<const K, V> value_type;

  /// Constructor
  vector_sfra() {
    // Do not allow synthesis of vector with zero or negative capacity
    static_assert(CAPACITY >= 1);
  }

  /// Destructor
  ~vector_sfra() {}

  /// Populate by stealing elements from another chunk.
  /// Insert (k,v) as the first element, and then take all entries greater than
  /// k from a given other vector.
  /// This method overwrites the contents of the current vector with the stolen
  /// elements; it is assumed it is called when the current vector is empty.
  /// Returns true if successful;
  /// returns false if it cannot be done as it would make this vector too full.
  bool insert_and_steal_greater(vector_sfra *victim, const value_type &pair) {

    // First, determine how many entries to steal, and initialize key_list and
    // val_list with the appropriate sizes.
    const K &k = pair.first;
    int start_pos = victim->find(k);

    // Assert that k is not in the victim vector.
    // NB: first condition guards against out-of-bounds read on second condition
    assert(start_pos == victim->size || victim->key_list[start_pos] != k);

    int entries_to_steal = victim->size - start_pos;

    assert(entries_to_steal + 1 <= CAPACITY);

    // Initialize the first entry.
    key_list[0] = k;
    val_list[0] = pair.second;

    // Copy over the other entries.
    asm volatile("" ::: "memory"); // ensure everything is flushed to RAM
    std::memcpy(key_list + 1, victim->key_list + start_pos,
                entries_to_steal * sizeof(rlx_atomic<K>));
    std::memcpy(val_list + 1, victim->val_list + start_pos,
                entries_to_steal * sizeof(rlx_atomic<V>));
    asm volatile("" ::: "memory"); // ensure everything is flushed to RAM

    // Finally, correct the sizes of the two vectors.
    size = 1 + entries_to_steal;
    victim->size -= entries_to_steal;

    // Return true to indicate success.
    return true;
  }

  /// Construct and populate by stealing the latter half of the elements from
  /// another chunk. Also insert (k,v) into either the victim or the newly
  /// constructed vector as appropriate.
  /// If victim starts with n elements, victim will end with ceil((n+1)/2)
  /// elements, and this vector will end with floor((n+1)/2) elements.
  /// This method overwrites the contents of the current vector with the stolen
  /// elements; it is assumed it is called when the current vector is empty.
  void steal_half_and_insert(vector_sfra *victim, const value_type &pair) {

    const K &k = pair.first;

    // First, determine where the inserted element should go.
    int insert_pos = victim->find(k);

    // Assert that k is not in the victim vector.
    // NB: first condition guards against out-of-bounds read on second condition
    assert(insert_pos == victim->size || victim->key_list[insert_pos] != k);

    // NB: We slightly abuse the term "median" here. Normally, if the number of
    // elements is even, the median is between the two elements in the middle.
    // Here we simply take the greater of the two.
    int median_pos = victim->size / 2;

    if (insert_pos <= median_pos) {
      // Case 1: (k,v) will be inserted into victim.
      // In this case, we simply split the vector and then insert the new
      // element, as there isn't a more efficient way to do it.

      // In this case, we steal the median and all elements that follow.
      int first_to_steal = median_pos;

      // First, copy elements from victim.
      // NB: If victim->size is even, then first_to_steal == entries_to_steal,
      // but if it's odd, then entries_to_steal ends up being 1 greater.
      int entries_to_steal = victim->size - first_to_steal;

      asm volatile("" ::: "memory"); // ensure everything is flushed to RAM
      std::memcpy(key_list, victim->key_list + first_to_steal,
                  entries_to_steal * sizeof(rlx_atomic<K>));
      std::memcpy(val_list, victim->val_list + first_to_steal,
                  entries_to_steal * sizeof(rlx_atomic<V>));
      asm volatile("" ::: "memory"); // ensure everything is flushed to RAM

      // Correct the sizes of the two vectors.
      size = entries_to_steal;
      victim->size = first_to_steal;

      // Finally, insert the new element into the victim.
      victim->insert(pair);
    } else {
      // Case 2: (k,v) will be inserted into this vector.
      // We combine the insert and copy procedures to avoid wastefully moving
      // elements twice, which would be the result of the naive approach (split
      // then insert).

      // In this case, we allow the victim to keep the median,
      // and so the first element we steal is the one after that.
      int first_to_steal = median_pos + 1;
      int entries_to_steal = victim->size - first_to_steal;

      int first_batch_size = insert_pos - first_to_steal;
      int second_batch_size = entries_to_steal - first_batch_size;

      // Copy the first batch, the elements between the start point and the
      // inserted element (may be zero.)

      // we don't have P1478R1 yet, so we do the fences by hand:
      asm volatile("" ::: "memory"); // ensure everything is flushed to RAM
      std::memcpy(key_list, victim->key_list + first_to_steal,
                  first_batch_size * sizeof(rlx_atomic<K>));
      std::memcpy(val_list, victim->val_list + first_to_steal,
                  first_batch_size * sizeof(rlx_atomic<V>));
      asm volatile("" ::: "memory"); // ensure everything is flushed to RAM

      // Write the inserted element.
      key_list[first_batch_size] = k;
      val_list[first_batch_size] = pair.second;

      // Copy the second batch, the elements between the inserted element and
      // the end (may be zero.)
      asm volatile("" ::: "memory"); // ensure everything is flushed to RAM
      std::memcpy(key_list + first_batch_size + 1,
                  victim->key_list + first_to_steal + first_batch_size,
                  second_batch_size * sizeof(rlx_atomic<K>));
      std::memcpy(val_list + first_batch_size + 1,
                  victim->val_list + first_to_steal + first_batch_size,
                  second_batch_size * sizeof(rlx_atomic<V>));
      asm volatile("" ::: "memory"); // ensure everything is flushed to RAM

      // Correct the sizes of the two vectors.
      size = entries_to_steal + 1;
      victim->size -= entries_to_steal;
    }
  }

  /// Populate by stealing the latter half of the elements from another chunk.
  /// If victim starts with n elements, victim will end with floor(n/2)
  /// elements, and new vector will end with ceil(n/2) elements.
  /// This method overwrites the contents of the current vector with the stolen
  /// elements; it is assumed it is called when the current vector is empty.
  void steal_half(vector_sfra *victim) {
    int median_pos = victim->size / 2;

    // Steal the median and all elements that follow.
    int first_to_steal = median_pos;

    // Copy elements from victim.
    int entries_to_steal = victim->size - first_to_steal;

    asm volatile("" ::: "memory"); // ensure everything is flushed to RAM
    std::memcpy(key_list, victim->key_list + first_to_steal,
                entries_to_steal * sizeof(rlx_atomic<K>));
    std::memcpy(val_list, victim->val_list + first_to_steal,
                entries_to_steal * sizeof(rlx_atomic<V>));
    asm volatile("" ::: "memory"); // ensure everything is flushed to RAM

    // Correct the sizes of the two vectors.
    size = entries_to_steal;
    victim->size = first_to_steal;
  }

  /// Insert a new element into the list.
  /// If already exists, do nothing and return false.
  /// If it does not exist, but there isn't room to insert it,
  /// mark the overfull parameter true and return false.
  bool insert(const value_type &pair, bool &overfull) {
    const K &k = pair.first;

    int pos = find(k);

    if (pos < size && key_list[pos] == k) {
      // Already exists
      overfull = false;
      return false;
    }

    // Prevent vector from becoming overfull
    if (size == CAPACITY) {
      overfull = true;
      return false;
    }

    // Shift other elements to make room
    for (int i = size; i > pos; --i) {
      key_list[i] = key_list[i - 1].load();
      val_list[i] = val_list[i - 1].load();
    }

    // Insert new element
    key_list[pos] = k;
    val_list[pos] = pair.second;

    ++size;

    overfull = false;
    return true;
  }

  /// Insert a new element into the list.
  /// If already exists, do nothing and return false.
  /// If it does not exist, but there isn't room to insert it,
  /// an assert will fail.
  bool insert(const value_type &pair) {
    bool overfull = false;
    bool result = insert(pair, overfull);
    assert(!overfull);
    return result;
  }

  /// Remove an element from the list and fetch its value.
  /// Return true if successful, false if didn't exist.
  bool remove(const K &k, V &v) {
    int pos = find(k);

    if (pos >= size || key_list[pos] != k) {
      // Didn't exist
      return false;
    }

    v = val_list[pos];
    --size;

    // Fill gap by shifting other elements
    for (int i = pos; i < size; ++i) {
      key_list[i] = key_list[i + 1].load();
      val_list[i] = val_list[i + 1].load();
    }

    return true;
  }

  /// As above, but caller doesn't care about found value.
  bool remove(const K &k) {
    V _; // Dummy argument
    return remove(k, _);
  }

  /// Find a given key in the list.
  /// Return false if not found.
  bool contains(const K &k, V &v) const {
    int pos = find(k);

    if (pos < size && key_list[pos] == k) {
      v = val_list[pos];
      return true;
    }

    return false;
  }

  /// As above, but caller doesn't care about found value.
  bool contains(const K &k) const {
    int pos = find(k);
    return pos < size && key_list[pos] == k;
  }

  /// Return the minimum key.
  /// CAVEAT: If vector is empty, may return junk data!
  K first() const { return key_list[0]; }

  /// Return the last key via the parameter k.
  /// Do nothing and return false if empty.
  bool last(K &k) const {
    if (size > 0) {
      k = key_list[size - 1];
      return true;
    }

    return false;
  }

  /// Find the biggest key that is Less Than or Equal to sought_k (hence "lte").
  /// The found key is assigned to found_k, and the value is assigned to v.
  /// Returns false if there is no such element.
  bool find_lte(const K &sought_k, K &found_k, V &v) const {
    // Edge case: if entirely empty, just return false.
    if (size == 0)
      return false;

    // First, call find.
    int pos = find(sought_k);

    // find() has "GTE" behavior; it finds the smallest element Greater Than or
    // Equal to sought_k. If sought_k is in the vector, this is what we want;
    // otherwise, we subtract 1 to find the largest element less than sought_k.
    if (pos >= size || key_list[pos] > sought_k) {
      --pos;

      // pos goes out of bounds if and only if there are no elements smaller
      // than sought_k in the entire vector.
      if (pos < 0)
        return false;
    }

    // Otherwise, pos is the position of the element we want.
    found_k = key_list[pos];
    v = val_list[pos];

    return true;
  }

  /// As above, but caller doesn't care about the found k
  bool find_lte(const K &sought_k, V &v) const {
    K _ = sought_k;
    return find_lte(sought_k, _, v);
  }

  /// Consume another vector_sfra, stealing all of its elements. This
  /// method assumes the other vector's minimum element > this vector's maximum
  /// element.
  void merge(vector_sfra *victim) {
    // Check against overfull
    assert(size + victim->size <= CAPACITY);

    // Copy the elements over
    asm volatile("" ::: "memory"); // ensure everything is flushed to RAM
    std::memcpy(key_list + size, victim->key_list,
                victim->size * sizeof(rlx_atomic<K>));
    std::memcpy(val_list + size, victim->val_list,
                victim->size * sizeof(rlx_atomic<V>));
    asm volatile("" ::: "memory"); // ensure everything is flushed to RAM

    size += victim->size;
    victim->size = 0;
  }

  bool verify() const {
    if (size < 0) {
      std::cout << "Verification failed! Vector has negative size: " << size
                << std::endl;
      return false;
    }

    if (CAPACITY < size) {
      std::cout << "Verification failed! Vector is " << size << "/" << CAPACITY
                << std::endl;
      return false;
    }

    // Check that keys are monotonically increasing
    for (int i = 1; i < size; ++i) {
      if (key_list[i] <= key_list[i - 1]) {
        std::cout << "Verification failed! Key " << +key_list[i - 1]
                  << " is at position " << i - 1 << ", but key " << +key_list[i]
                  << " is at position " << i << "!" << std::endl;
        std::cout << "Current size: " << size << std::endl;
        return false;
      }
    }
    return true;
  }

  /// Sort this vector.
  /// This implementation is already sorted, so this is a no-op.
  void sort() const {}

  /// Return the a key at a specific index.
  /// Assertion failure if index out of range.
  K at(int index) const {
    // Bounds check
    assert(index >= 0 && index < size);
    return key_list[index];
  }

  void verbose_analysis() const {
    std::cout << "[";

    // Print first element
    if (size > 0) {
      std::cout << +key_list[0];
    }

    // Print last element if distinct from first element
    if (size > 1) {
      std::cout << "-" << +key_list[size - 1];
    }

    // Print size out of capacity
    std::cout << "](" << size << "/" << CAPACITY << ")" << std::endl;
  }

  void dump() const {
    // Print all elements
    std::cout << "[ ";
    for (int i = 0; i < size; ++i) {
      std::cout << +key_list[i] << " ";
    }

    // Print size/capacity
    std::cout << "](" << size << "/" << CAPACITY << ")" << std::endl;
  }

  // Get the maximum key
  K max_key() const {
    // Assert that there's at least one element
    // (the maximum of the empty set is undefined)
    assert(size != 0);
    return key_list[size - 1];
  }

  size_t get_capacity() const { return CAPACITY; }
  size_t get_size() const { return size; }

  /// Apply a function f() to all key/value pairs in this vector.
  void foreach (std::function<void(const K &, V &, bool &)> f,
                bool &exit_flag) {
    for (int i = 0; i < size && !exit_flag; ++i) {
      // NB: This is necessary because val_list is atomic,
      // and f() wants a non-atomic T.
      // Copying v twice will be expensive if it's big...
      // Application of f() isn't atomic, but we trust the caller (in this
      // project, the skipvector) to provide true mutex with any other
      // modifying operations while foreach() is running.
      V v = val_list[i].load();
      f(key_list[i].load(), v, exit_flag);
      val_list[i].store(v);
    }
  }

  /// Apply a function f() to all key/value pairs in the intersection of this
  /// vector and the given range [from, to].
  /// Returns true if end of range is reached, false otherwise.
  bool range(const K &from, const K &to,
             std::function<void(const K &, V &, bool &)> f, bool &exit_flag) {

    int i = 0;

    // Skip over any elements less than the start.
    while (i < size && key_list[i] < from) {
      ++i;
    }

    // Apply f() to elements in the range.
    while (i < size && !exit_flag) {
      if (key_list[i] > to) {
        // If we encounter an element after the end of the range, return true.
        return true;
      }

      // NB: Same note as in foreach().
      V v = val_list[i].load();
      f(key_list[i], v, exit_flag);
      val_list[i].store(v);
      ++i;
    }

    return false;
  }
};
