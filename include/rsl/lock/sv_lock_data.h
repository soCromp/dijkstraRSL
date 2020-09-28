#pragma once

#include <atomic>
#include <cassert>

#include "../lock/loop_counter.h"

/// A simple mutex with a bit stolen.
/// Uses the same interface as sv_lock for templating purposes.
/// A "read lock" simply acquires the lock outright.
class sv_lock_data {
  std::atomic<uint64_t> lock;

  // A bit mask representing the orphan bit.
  // Indicates if this node has a parent in the layer above.
  static constexpr uint64_t ORPHAN_BIT = 0x0000000000000001ull;

  // Represents the lock bit.
  static constexpr uint64_t LOCK_BIT = 0x0000000000000002ull;

public:
  /// Static helper method. Determine if a read value returned by an instance of
  /// this class is an orphan.
  static bool is_orphan(uint64_t v) { return (v & ORPHAN_BIT) == ORPHAN_BIT; }

  /// Check if a lock is held.
  static bool is_locked(uint64_t v) { return (v & LOCK_BIT) == LOCK_BIT; }

  /// Default constructor; begin unlocked and with all flag bits clear.
  sv_lock_data() : lock(0) {}

  /// Default constructor; begin unlocked and alive.
  /// Set the orphan bit according to the provided bool argument.
  sv_lock_data(bool orphan) : lock(orphan ? ORPHAN_BIT : 0) {}

  /// Constructor. Allows caller to manually set the orphan flag.
  /// Also takes a dummy void* that is ignored for template reasons.
  sv_lock_data(void *, bool orphan) : lock(orphan ? ORPHAN_BIT : 0) {}

  ~sv_lock_data() {}

  /// Initializes a node that was constructed with the default constructor.
  void initialize(void *, bool _orphan) {
    if (_orphan) {
      // NB: No concurrency at initialize time, so it's OK for this to a load
      // and a store rather than a CAS.
      lock.store(lock.load() | ORPHAN_BIT);
    }
  }

  /// Determine if the node owning this lock is an orphan.
  /// May only be called while lock is held.
  bool is_orphan() { return (lock.load() & ORPHAN_BIT) == ORPHAN_BIT; }
  bool is_locked() { return (lock & LOCK_BIT) == LOCK_BIT; }

  /// Acquire the lock as a writer. Busywait until unlocked, then CAS to locked.
  void acquire() {
    uint64_t read_value = lock;
    bool success = false;
    loop_counter ctr;
    while (!success) {
      ctr.count();
      if ((read_value & LOCK_BIT) == 0) {
        uint64_t new_value = read_value | LOCK_BIT;
        success = lock.compare_exchange_weak(read_value, new_value);
      } else {
        read_value = lock;
      }
    }
  }

  /// Release the lock. Should only be called by the thread that acquired.
  uint64_t release() {
    // Assert that the lock bit is set on a lock being released.
    uint64_t read_value = lock;
    assert((read_value & LOCK_BIT) == LOCK_BIT);

    uint64_t new_value = read_value & ~LOCK_BIT;
    lock = new_value;
    return new_value;
  }

  /// Release the lock and mark this node as an orphan. Orphaning is
  /// irreversible. Should only be called by the thread that acquired.
  void release_as_orphan() {
    // Assert that both the lock bit is set on a lock being released.
    uint64_t read_value = lock;
    assert((read_value & LOCK_BIT) == LOCK_BIT);
    lock = (read_value & ~LOCK_BIT) | ORPHAN_BIT;
  }

  /// Acquire the lock as a reader.
  /// In this implementation, this locks the node outright.
  uint64_t begin_read() {
    acquire();
    return lock;
  }

  /// Conclude a read with no regard for whether it was successful.
  /// In this implementation, this is a release.
  void abort_read() { release(); }

  /// Just get the value of the lock directly.
  /// For debug purposes only.
  uint64_t get_value() { return lock; }

  void dump() {
    uint64_t read_value = lock;

    if (is_orphan(read_value)) {
      std::cout << "o"; // orphan
    } else {
      std::cout << "C"; // child
    }

    if (is_locked(read_value)) {
      std::cout << "L"; // locked
    } else {
      std::cout << "U"; // unlocked
    }
  }
};
