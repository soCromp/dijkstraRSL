#pragma once

#include <cassert>
#include <cstring>
#include <functional>
#include <iostream>

#include "rlx_atomic.h"

/// A data structure which uses an unsorted vector to implement a map interface.
/// Terrible asymptotes (O(n)), but fast at small sizes.
/// Non-concurrent. Non-resizable.
/// UFRA: Unsorted, Fixed capacity, Relaxed Atomic
/// INVARIANT: If vector has at least 2 elements, the minimum element is at
/// index 0 and the maximum is at index 1.
template <typename K, typename V, int CAPACITY> class vector_ufra {
  /// List of keys
  rlx_atomic<K> key_list[CAPACITY];

  /// List of values
  rlx_atomic<V> val_list[CAPACITY];

  /// Current number of elements in the list.
  rlx_atomic<int> size = 0;

  /// Internal function used to find a key in the vector.
  /// If it exists, it returns true, and sets pos.
  /// Otherwise, it returns false.
  bool find(const K &k, int &pos) {
    for (int i = 0; i < size; i++) {
      if (key_list[i] == k) {
        pos = i;
        return true;
      }
    }
    return false;
  }

  /// As above, but we do not care about the index.
  bool find(const K &k) {
    int _;
    return find(k, _);
  }

  /// Internal function used to find a key in a vector WHILE SORTED.
  /// If it exists, it returns the exact position;
  /// Otherwise, the position it should be inserted into.
  /// A binary search that takes lg(n) time.
  int binary_find(const K &k) const {
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

  // Swap key and value at indices n and m.
  void swap(int n, int m) {
    K temp_k = key_list[n];
    V temp_v = val_list[n];

    key_list[n] = key_list[m].load();
    val_list[n] = val_list[m].load();

    key_list[m] = temp_k;
    val_list[m] = temp_v;
  }

  // Note: implemented based on pseudocode at:
  // https://en.wikipedia.org/wiki/Quickselect
  // Quickselect algorithm is by Tony Hoare.
  // Partitions the vector based on the key at pivot_index.
  // Modified by me for use in quickselect rather than quicksort.
  int partition(int lo, int hi) {
    // Choose element in middle as pivot.
    // This works well when list is already mostly sorted,
    // as vectors used by a skipvector are likely to be.
    int pivot_idx = (hi + lo) / 2;
    K pivot_k = key_list[pivot_idx];

    // Swap pivot to end, to avoid repeatedly swapping it back and forth in the
    // loop body below.
    swap(pivot_idx, hi);

    int i = lo;
    int j = hi - 1;

    while (true) {
      while (key_list[i] < pivot_k)
        ++i;
      while (key_list[j] > pivot_k)
        --j;
      if (i > j) {
        // Move pivot to its proper place and return.
        swap(i, hi);
        return i;
      }
      swap(i, j);
    }
  }

  // Chooses a pivot index between the left and right index.
  int select_pivot(int left, int right) {
    // We just choose the middle element. Previous range operations and
    // quickselect calls may have produced a sorted or roughly sorted vector.
    return (left + right) / 2;
  }

  // Note: implemented based on pseudocode at:
  // https://en.wikipedia.org/wiki/Quickselect
  // Quickselect algorithm is by Tony Hoare.
  // Returns the k-th smallest element of list within left..right inclusive
  // (i.e. left <= k <= right).
  void quickselect(int left, int right, int k) {
    // If the list contains only one element, return that element.
    if (left == right)
      return;

    int pivot_index = partition(left, right);

    if (k == pivot_index)
      return;
    else if (k < pivot_index)
      quickselect(left, pivot_index - 1, k);
    else
      quickselect(pivot_index + 1, right, k);
  }

  // Performs a quickselect to take the median.
  void quickselect_median() {
    if (CAPACITY > 2 && size > 2) {
      // Move the maximum to the end.
      swap(1, size - 1);

      // Quickselect for the median element, taking the entire list minus the
      // already-sorted min and max elements.
      quickselect(1, size - 2, size / 2);
    }
  }

  // Restore the invariant when max is at the final position.
  void restore_invariant() {
    if (CAPACITY > 2 && size > 2) {
      swap(1, size - 1);
    }
  }

  // Finds the minimum element and swaps it to its correct position
  // after the invariant was ruined by some method.
  // This method assumes max is in its correct position.
  void find_new_min() {
    if (CAPACITY <= 2 || size <= 2)
      // min should already be in place.
      return;

    int min_pos = 0;
    K min_k = key_list[0];

    for (int i = 2; i < size; ++i) {
      if (key_list[i] < min_k) {
        min_k = key_list[i];
        min_pos = i;
      }
    }

    if (min_pos != 0) {
      swap(0, min_pos);
    }
  }

  // Finds the maximum element and swaps it to its correct position
  // after the invariant was ruined by some method.
  // This method assumes min is in its correct position.
  void find_new_max() {
    if (CAPACITY <= 2 || size <= 2)
      // max should already be in place.
      return;

    int max_pos = 1;
    K max_k = key_list[1];

    for (int i = 2; i < size; ++i) {
      if (key_list[i] > max_k) {
        max_k = key_list[i];
        max_pos = i;
      }
    }

    if (max_pos != 1) {
      swap(1, max_pos);
    }
  }

public:
  /// insert() takes a reference to a k/v pair, so we expose the type here
  typedef std::pair<const K, V> value_type;

  /// Default constructor performing no initialization.
  vector_ufra() {
    // Do not allow synthesis of vector with zero or negative capacity
    static_assert(CAPACITY >= 1);
  }

  /// Destructor
  ~vector_ufra() {}

  /// Populate by stealing elements from another chunk.
  /// Insert (k,v) as the first element, and then take all entries greater than
  /// k from a given other vector.
  /// This method overwrites the contents of the current vector with the stolen
  /// elements; it is assumed it is called when the current vector is empty.
  /// Returns true if successful;
  /// returns false if it cannot be done as it would make this vector too full.
  bool insert_and_steal_greater(vector_ufra *victim, const value_type &pair) {
    const K &k = pair.first;

    // First, determine the needed capacity.
    // Also, scan for the element that will become victim's new max.
    int elements_to_steal = 0;
    int victims_new_max_pos = 0;
    K victims_new_max_key = victim->key_list[0];

    // We want to start looking for victims_new_max_key at element 2,
    // so unroll the first two iterations of the loop.
    if (victim->size > 0 && victim->key_list[0] > k) {
      elements_to_steal++;
    }

    if (victim->size > 1 && victim->key_list[1] > k) {
      elements_to_steal++;
    }

    for (int i = 2; i < victim->size; ++i) {
      // Count elements to steal
      if (victim->key_list[i] > k) {
        elements_to_steal++;
      }

      // Find victim's new max element
      if (victim->key_list[i] < k &&
          victim->key_list[i] > victims_new_max_key) {
        victims_new_max_key = victim->key_list[i];
        victims_new_max_pos = i;
      }

      // Assert that k is not in the victim vector.
      assert(victim->key_list[i] != k);
    }

    int needed_capacity = elements_to_steal + 1;

    assert(needed_capacity <= CAPACITY);

    // Initialize the first entry.
    key_list[0] = k;
    val_list[0] = pair.second;

    // Edge case 1: there are no elements to be stolen from victim.
    // NB: If the caller respects the invariant that this method is never
    // invoked when it would result in this vector becoming overfull,
    // and capacity is 1, then there can not be any elements to steal.
    if (CAPACITY == 1 || elements_to_steal == 0) {
      // We're already done!
      size = 1;
      return true;
    }

    // Edge case 2: steal ALL elements from victim.
    if (elements_to_steal == victim->size) {

      // Edge case 2A: victim has just one element
      if (elements_to_steal == 1) {
        // Move victim's only element to our maximum position.
        key_list[1] = victim->key_list[0].load();
        val_list[1] = victim->val_list[0].load();
      } else {
        // Edge case 2B: victim has at least two elements
        // Move element in victim's maximum position to ours.
        key_list[1] = victim->key_list[1].load();
        val_list[1] = victim->val_list[1].load();

        // Steal element in victim's minimum position.
        key_list[2] = victim->key_list[0].load();
        val_list[2] = victim->val_list[0].load();

        // Steal all other elements.
        elements_to_steal -= 2;
        asm volatile("" ::: "memory"); // ensure everything is flushed to RAM
        std::memcpy(key_list + 3, victim->key_list + 2,
                    elements_to_steal * sizeof(K));
        std::memcpy(val_list + 3, victim->val_list + 2,
                    elements_to_steal * sizeof(V));
        asm volatile("" ::: "memory");
      }

      victim->size = 0;
      size = needed_capacity;
      return true;
    }

    // Edge case 3: Steal all elements from victim except its minimum.
    if (victims_new_max_key == victim->key_list[0]) {
      // NB: Element in victim's max position will be moved to our max position.
      asm volatile("" ::: "memory"); // ensure everything is flushed to RAM
      std::memcpy(key_list + 1, victim->key_list + 1,
                  elements_to_steal * sizeof(K));
      std::memcpy(val_list + 1, victim->val_list + 1,
                  elements_to_steal * sizeof(V));
      asm volatile("" ::: "memory");

      victim->size = 1;
      size = needed_capacity;
      return true;
    }

    // NB: Due to the edge cases ruled out above, we know for certain that we
    // will steal at least one of the victim's elements, and that the victim
    // will retain at least two elements. Since we will steal at least one
    // element, we know we will steal victim's maximum. Since we will leave
    // at least two elements, we know we will not steal the victim's minimum,
    // and that the victim's maximum position will be occupied after we steal.

    // Steal the victim's maximum element.
    key_list[1] = victim->key_list[1].load();
    val_list[1] = victim->val_list[1].load();
    size = 2;

    // Move the victim's new maximum element to its proper position.
    victim->key_list[1] = victim->key_list[victims_new_max_pos].load();
    victim->val_list[1] = victim->val_list[victims_new_max_pos].load();
    victim->size--;

    // Move victim's last element into the gap left by this move.
    if (victims_new_max_pos != victim->size) {
      victim->key_list[victims_new_max_pos] =
          victim->key_list[victim->size].load();
      victim->val_list[victims_new_max_pos] =
          victim->val_list[victim->size].load();
    }

    // Now, steal all the elements > k from the victim vector.
    // This loop carefully avoids moving any element more than once.
    int i = 2;
    while (i < victim->size) {
      if (victim->key_list[i] < k) {
        // Skip over any elements that are < k.
        i++;

      } else if (victim->key_list[victim->size - 1] > k) {
        // Move any elements > k from the end of the victim.
        victim->size--;
        key_list[size] = victim->key_list[victim->size].load();
        val_list[size] = victim->val_list[victim->size].load();
        size++;

      } else {
        // At this point we know victim[i] > k, victim[size - 1] < k, and
        // i != size - 1.  Thus, move victim[i] to this vector,
        // and victim[size - 1] to victim[i].
        key_list[size] = victim->key_list[i].load();
        val_list[size] = victim->val_list[i].load();
        size++;
        victim->size--;
        victim->key_list[i] = victim->key_list[victim->size].load();
        victim->val_list[i] = victim->val_list[victim->size].load();
        i++;
      }
    }

    return true;
  }

  /// Construct and populate by stealing the latter half of the elements from
  /// another chunk. Also insert (k,v) into either the victim or the newly
  /// constructed vector as appropriate.
  /// If victim starts with n elements, victim will end with ceil((n+1)/2)
  /// elements, and this vector will end with floor((n+1)/2) elements.
  /// This method overwrites the contents of the current vector with the stolen
  /// elements; it is assumed it is called when the current vector is empty.
  void steal_half_and_insert(vector_ufra *victim, const value_type &pair) {
    victim->quickselect_median();

    const K &k = pair.first;

    // NB: We slightly abuse the term "median" here. Normally, if the number of
    // elements is even, the median is between the two elements in the middle.
    // Here we simply take the greater of the two.
    int median_pos = victim->size / 2;
    K median_k = victim->key_list[median_pos];
    int first_to_steal;

    if (k < median_k) {
      // Case 1: (k,v) will be inserted into victim.
      // In this case, we steal the median and all elements that follow.
      first_to_steal = median_pos;
    } else {
      // Case 2: (k,v) will be inserted into this vector.
      // In this case, we allow the victim to keep the median,
      // and so the first element we steal is the one after that.
      first_to_steal = median_pos + 1;
    }

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

    // Our quickselect algorithm places the max at the end,
    // so this will restore this vector's maximum element.
    restore_invariant();

    // At this point, victim's min is correct, as it was never disturbed.
    // All that remains is to correct our min and victim's max.
    if (k < median_k) {
      // If we took the median, it ended up as our min,
      // so no op there.
      // Find victim's new max.
      victim->find_new_max();

      // Finally, insert element into victim.
      victim->insert(pair);
    } else {
      // If we let the victim keep the median, it ended up in its last position.
      // Therefore, this will restore it to position 1.
      victim->restore_invariant();

      // Find our new min.
      find_new_min();

      // Finally, insert element into this vector.
      insert(pair);
    }
  }

  /// Populate by stealing the latter half of the elements from another chunk.
  /// If victim starts with n elements, victim will end with floor(n/2)
  /// elements, and new vector will end with ceil(n/2) elements.
  /// This method overwrites the contents of the current vector with the stolen
  /// elements; it is assumed it is called when the current vector is empty.
  void steal_half(vector_ufra *victim) {
    victim->quickselect_median();

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

    // Our quickselect algorithm places the max at the end,
    // so this will restore this vector's maximum element.
    restore_invariant();

    // At this point, victim's min is correct, as it was never disturbed.
    // Our min is correct, as it is the median.
    // All that remains is to correct victim's max.
    victim->find_new_max();
  }

  /// Insert a new element into the list.
  /// If already exists, do nothing and return false.
  bool insert(const value_type &pair, bool &overfull) {
    const K &k = pair.first;

    if (find(k)) {
      // Already exists
      return false;
    }

    // Prevent vector from becoming overfull
    if (size == CAPACITY) {
      overfull = true;
      return false;
    }

    // Insert new element

    if (CAPACITY >= 1 && size >= 1 && k < key_list[0]) {
      // Case 1: element is smaller than existing minimum
      // Move previous min to end of list
      key_list[size] = key_list[0].load();
      val_list[size] = val_list[0].load();

      // Insert new element as new minimum
      key_list[0] = k;
      val_list[0] = pair.second;
    } else if (CAPACITY >= 2 && size >= 2 && k > key_list[1]) {
      // Case 2: element is larger than existing maximum
      // Move previous max to end of list
      key_list[size] = key_list[1].load();
      val_list[size] = val_list[1].load();

      // Insert new element as new maximum
      key_list[1] = k;
      val_list[1] = pair.second;
    } else {
      // Normal case: new element is between existing max and min
      // (Or, correctly inserting new element as new minimum or maximum
      // because that slot happens to be at the end of the vector.)
      key_list[size] = k;
      val_list[size] = pair.second;
    }

    size++;
    return true;
  }

  /// Insert a new element into the list.
  /// If already exists, do nothing and return false.
  /// If it does not exist, but there isn't room to insert it,
  /// throw overfull.
  bool insert(const value_type &pair) {
    bool overfull = false;
    bool result = insert(pair, overfull);
    assert(!overfull);
    return result;
  }

  /// Remove an element from the list.
  /// Return true if successful, false if didn't exist.
  bool remove(const K &k, V &v) {
    // Edge case 1: Removing minimum
    if (size >= 1 && k == key_list[0]) {
      v = val_list[0];

      // Edge case 1A: Removing sole element
      if (size == 1) {
        size = 0;
      } else {
        // Edge case 1B: Need to find new minimum
        K new_min = key_list[1];
        int new_min_pos = 1;
        for (int i = 2; i < size; i++) {
          if (key_list[i] < new_min) {
            new_min = key_list[i];
            new_min_pos = i;
          }
        }

        // Move new minimum to minimum slot
        key_list[0] = key_list[new_min_pos].load();
        val_list[0] = val_list[new_min_pos].load();

        // Move final element into gap
        size--;
        if (new_min_pos != size) {
          key_list[new_min_pos] = key_list[size].load();
          val_list[new_min_pos] = val_list[size].load();
        }
      }
      return true;
    } else if (CAPACITY >= 2 && size >= 2 && k == key_list[1]) {
      // Edge case 2: Removing maximum
      v = val_list[1];

      // Edge case 2A: Maximum position will be vacant
      if (size == 2) {
        size = 1;
      } else {
        // Edge case 2B: Need to find new maximum
        K new_max = key_list[2];
        int new_max_pos = 2;
        for (int i = 3; i < size; i++) {
          if (key_list[i] > new_max) {
            new_max = key_list[i];
            new_max_pos = i;
          }
        }

        // Move new maximum to maximum slot
        key_list[1] = key_list[new_max_pos].load();
        val_list[1] = val_list[new_max_pos].load();

        // Move final element into gap
        size--;
        if (new_max_pos != size) {
          key_list[new_max_pos] = key_list[size].load();
          val_list[new_max_pos] = val_list[size].load();
        }
      }
      return true;
    }

    // Normal case: Removing neither maximum nor minimum.
    int pos;

    if (!find(k, pos)) {
      // Didn't exist
      return false;
    }

    v = val_list[pos];

    // Move the last element into the gap left by this removal
    // (unless removed element was the last element)
    size--;
    if (pos != size) {
      key_list[pos] = key_list[size].load();
      val_list[pos] = val_list[size].load();
    }

    return true;
  }

  /// As above, but caller does not care about value.
  bool remove(const K &k) {
    V _;
    return remove(k, _);
  }

  /// Find a given key in the list.
  /// Return false if not found.
  bool contains(const K &k, V &v) {
    int pos;

    bool found = find(k, pos);
    if (found)
      v = val_list[pos].load();
    return found;
  }

  /// As above, but caller doesn't care about found value.
  bool contains(const K &k) {
    int _; // Dummy argument
    return find(k, _);
  }

  /// Return the minimum key.
  /// CAVEAT: If vector is empty, may return junk data!
  K first() const { return key_list[0]; }

  /// Return the last key via the parameter k.
  /// Do nothing and return false if empty.
  bool last(K &k) {
    if (size >= 2) {
      k = key_list[1];
      return true;
    }

    if (size == 1) {
      k = key_list[0];
      return true;
    }

    return false;
  }

  /// Find the biggest key that is Less Than or Equal to sought_k (hence "lte").
  /// The found key is assigned to found_k, and the value is assigned to v.
  /// Returns false if there is no such element.
  bool find_lte(const K &sought_k, K &found_k, V &v) {
    int found_pos = -1;

    // Search until a single element <= sought_k is found
    int i = 0;
    for (; i < size; i++) {
      if (key_list[i] <= sought_k) {
        found_pos = i;
        found_k = key_list[i];
        break;
      }
    }

    // Search rest of array for maximum element <= sought_k
    for (; i < size; i++) {
      if (found_k < key_list[i] && key_list[i] <= sought_k) {
        found_pos = i;
        found_k = key_list[i];
      }
    }

    // If one was found,
    // return the found key and value via parameter refs and return true.
    bool found = found_pos >= 0;
    if (found)
      v = val_list[found_pos];
    return found;
  }

  /// As above, but caller doesn't care about the found k
  bool find_lte(const K &sought_k, V &v) {
    K _ = sought_k;
    return find_lte(sought_k, _, v);
  }

  /// Consume another vector_ufra, stealing all of its elements. This
  /// method assumes the other vector's minimum element > this vector's maximum
  /// element.
  void merge(vector_ufra *victim) {
    // NOTE: Implemented slightly inefficiently.
    // Has a few swaps more than an optimal solution, but is much simpler.

    if (victim->size == 0) {
      // Case 1: victim has no elements to steal.
      return;
    }

    // Check against overfull
    assert(size + victim->size <= CAPACITY);

    if (size == 0) {
      // Case 2: we are empty. Copy the entire victim vector.
      asm volatile("" ::: "memory"); // ensure everything is flushed to RAM
      std::memcpy(key_list, victim->key_list, victim->size * sizeof(K));
      std::memcpy(val_list, victim->val_list, victim->size * sizeof(V));
      asm volatile("" ::: "memory");
      size = victim->size.load();
      victim->size = 0;
      return;
    }

    if (CAPACITY == 1) {
      // If capacity equals 1, then one of the two cases above should have
      // worked. Thus, we fail.
      exit(1);
    }

    // Case 3: Victim has exactly one element to steal.
    // It becomes our new maximum.
    if (victim->size == 1) {
      // Move our max element to end
      key_list[size] = key_list[1].load();
      val_list[size] = val_list[1].load();

      // Move victim's element to max positions
      key_list[1] = victim->key_list[0].load();
      val_list[1] = victim->val_list[0].load();

      victim->size = 0;
      size++;
      return;
    }

    // General case: Due to the edge cases ruled out above, we know we are
    // stealing at least two elements, and so victim's max must become curr's
    // max. curr has at least one element, and thus its minimum position is
    // filled.

    // Steal victim's minimum.
    key_list[size] = victim->key_list[0].load();
    val_list[size] = victim->val_list[0].load();
    size++;

    if (size >= 2) {
      // We have at least two elements,
      // so our maximum must be displaced to make room.
      key_list[size] = key_list[1].load();
      val_list[size] = val_list[1].load();
    }

    // victim's maximum becomes our new maximum.
    key_list[1] = victim->key_list[1].load();
    val_list[1] = victim->val_list[1].load();

    size++;

    // Copy all elements from victim other than min and max.
    int elements_to_steal = victim->size - 2;
    asm volatile("" ::: "memory"); // ensure everything is flushed to RAM
    std::memcpy(key_list + size, victim->key_list + 2,
                elements_to_steal * sizeof(K));
    std::memcpy(val_list + size, victim->val_list + 2,
                elements_to_steal * sizeof(V));
    asm volatile("" ::: "memory");
    victim->size = 0;
    size += elements_to_steal;
    return;
  }

  bool verify() {
    // Assert size does not exceed CAPACITY
    if (size > CAPACITY) {
      std::cout << "Somehow, size of " << size << " has exceeded capacity of "
                << CAPACITY << std::endl;
      dump();
      throw "verify failure";
      return false;
    }

    // The following checks are nonsensical for a vector of capacity 1, so...
    if (CAPACITY == 1)
      return true;

    // Check for duplicated keys
    for (int i = 1; i < size; i++) {
      for (int j = 0; j < i; j++) {
        if (key_list[i] == key_list[j]) {
          std::cout << "Found duplicated key " << key_list[i] << " at indices "
                    << i << "," << j << std::endl;
          dump();
          std::cout << std::endl;
          throw "verify failure";
          return false;
        }
      }
    }

    // Verify max > min.
    if (size >= 2 && key_list[1] < key_list[0]) {
      std::cout << "min > max: " << key_list[0] << "," << key_list[1]
                << std::endl;
      dump();
      std::cout << std::endl;
      throw "verify failure";
      return false;
    }

    // Verify elements in range [2:size-1] are between max and min.
    for (int i = 2; i < size; i++) {
      // Verify element is not > max or < min
      if (key_list[i] < key_list[0] || key_list[i] > key_list[1]) {
        std::cout << "Found key " << key_list[i] << " at index " << i
                  << " not between min " << key_list[0] << " and max "
                  << key_list[1] << std::endl;
        dump();
        std::cout << std::endl;
        throw "verify failure";
        return false;
      }
    }

    return true;
  }

  /// Sort this vector map (EXCEPT for max element.)
  /// Only to be used in total isolation.
  void sort() {
    if (CAPACITY <= 2) {
      // No-op if vector is too small to ever become unsorted.
      // As CAPACITY is a templated parameter, this check will be optimized out.
      return;
    }

    // This method implements an insertion sort, which is known to be fast at
    // small vector sizes. Unsorted vectors are also only advantageous at
    // small sizes, so it is assumed this class won't be used at sizes where
    // insertion sort's performance is a problem.

    // Min is left at index 0 and max at index 1; sort begins from index 2.
    for (int i = 3; i < size; i++) {
      K k = key_list[i];
      V v = val_list[i];

      int j;
      for (j = i; j >= 3 && k < key_list[j - 1]; j--) {
        key_list[j] = key_list[j - 1].load();
        val_list[j] = val_list[j - 1].load();
      }

      key_list[j] = k;
      val_list[j] = v;
    }
  }

  /// Return the key at a given order in the vector.
  /// Note: Only works if vector is sorted! Call sort() first!
  /// @throws out_of_range
  K at(int index) {
    if (index < 0 || index >= size) {
      // Error case: caller wants out-of-bounds element.
      throw std::out_of_range("at(" + std::to_string(index) + ")");
    }

    if (index == 0) {
      // Case 1: caller wants minimum element.
      return key_list[0];
    } else if (index == size - 1) {
      // Case 2: caller wants maximum element.
      return key_list[1];
    }

    // Common case: caller wants element between min and max.
    // Offset by 1 to account for max at position 1.
    return key_list[index + 1];
  }

  void verbose_analysis() {
    std::cout << "[";

    // Print first element
    if (size > 0) {
      std::cout << +key_list[0];
    }

    // Print last element if distinct from first element
    if (size > 1) {
      std::cout << "-" << +key_list[1];
    }

    // Print size/capacity
    std::cout << "](" << size << "/" << CAPACITY << ")" << std::endl;
  }

  // Get the maximum key
  K max_key() {
    if (size == 0) {
      throw std::out_of_range(
          "max_key(): maximum of empty vector is undefined");
    }

    if (size == 1) {
      return key_list[0];
    }

    return key_list[1];
  }

  void dump() {
    // Print all elements
    std::cout << "[ ";
    for (int i = 0; i < size; i++) {
      std::cout << +key_list[i] << " ";
    }

    // Print size/capacity
    std::cout << "](" << size << "/" << CAPACITY << ")" << std::endl;
  }

  size_t get_capacity() { return CAPACITY; }
  size_t get_size() { return size; }

  /// Process an element for a range or foreach operation.
  void iterate_element(std::function<void(const K &, V &, bool &)> f, int i,
                       bool &exit_flag) {
    // NB: same note as sfra foreach()
    V v = val_list[i].load();
    f(key_list[i].load(), v, exit_flag);
    val_list[i].store(v);
  }

  // Process an element for a range operation,
  // when it is unknown if the element is less than from.
  bool iterate_element_range1(std::function<void(const K &, V &, bool &)> f,
                              int i, const K &from, const K &to,
                              bool &exit_flag) {
    // Skip elements below from.
    if (key_list[i] < from)
      return false;

    // If we find an element in excess of to,
    // set exit_flag to end the iteration.
    if (key_list[i] > to) {
      exit_flag = true;
      return true;
    }

    // Element is neither below from nor above to, so process it.
    iterate_element(f, i, exit_flag);

    // If we process to itself, set exit_flag.
    if (key_list[i] == to)
      exit_flag = true;

    return true;
  }

  // Process an element for a range operation,
  // when it is known that the element exceeds from.
  void iterate_element_range2(std::function<void(const K &, V &, bool &)> f,
                              int i, const K &to, bool &exit_flag) {
    // If we find an element in excess of to,
    // set exit_flag to end the iteration.
    if (key_list[i] > to)
      exit_flag = true;

    // Element is neither below from nor above to, so process it.
    iterate_element(f, i, exit_flag);

    // If we process to itself, set exit_flag.
    if (key_list[i] == to)
      exit_flag = true;
  }

  /// Apply a function f() to all key/value pairs in this vector.
  /// NOTE: As this vector is unsorted, elements are not processed in order.
  void foreach (std::function<void(const K &, V &, bool &)> f,
                bool &exit_flag) {

    // The easiest way to process the elements in order is to sort them.
    sort();

    // Process first element.
    if (size > 0 && !exit_flag)
      iterate_element(f, 0, exit_flag);

    // Process sorted elements.
    for (int i = 2; i < size && !exit_flag; ++i)
      iterate_element(f, i, exit_flag);

    // Process maximum element.
    if (size > 1 && !exit_flag)
      iterate_element(f, 1, exit_flag);
  }

  /// Apply a function f() to all key/value pairs in the intersection of this
  /// vector and the given range [from, to].
  /// Returns true if exit_flag is set or end of range is reached, else false.
  /// NOTE: As this vector is unsorted, elements are not processed in order.
  bool range(const K &from, const K &to,
             std::function<void(const K &, V &, bool &)> f, bool &exit_flag) {

    bool from_reached = false;

    // The easiest way to process the elements in order is to sort them.
    sort();

    // Process minimum element.
    if (size > 0 && !exit_flag)
      from_reached = iterate_element_range1(f, 0, from, to, exit_flag);

    // Process sorted elements.
    int i = 2;
    for (; i < size && !exit_flag && !from_reached; ++i)
      from_reached = iterate_element_range1(f, i, from, to, exit_flag);

    for (; i < size && !exit_flag; ++i)
      iterate_element_range2(f, i, to, exit_flag);

    // Process maximum element.
    if (size > 1 && !exit_flag)
      // NB: It is not guaranteed that from_reached is set at this point,
      // so we use iterate_element_range1 instead of iterate_element_range2.
      iterate_element_range1(f, 1, from, to, exit_flag);

    return exit_flag;
  }
};
