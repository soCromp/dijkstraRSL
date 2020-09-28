#pragma once

#include <atomic>
#include <cassert>

#include "../lock/loop_counter.h"

// [bll] Will be easier if we separate dead bit and orphan bit from the seqlock,
//       and make them fields of some sort.

/// An implementation of a sequence lock to meet the specific needs of the
/// concurrent skipvector. Steals two flag bits to represents the states a
/// skipvector node can be in.
///
/// INVARIANT: If the lock bit is set, the freeze bit must also be set. There is
/// no logical distinction between "locked and frozen" and "locked and
/// unfrozen", So respecting this invariant helps simplify the code and avoid
/// bugs.
class sv_lock {
  std::atomic<uint64_t> lock;

  // A bit mask representing the orphan bit.
  // Indicates if this node has a parent in the layer above.
  static constexpr uint64_t ORPHAN_BIT = 0x0000000000000001ull;

  // A bit mask indicating if this lock is frozen. While a lock is frozen,
  // readers may continue to access it, but threads other than the freezer may
  // neither freeze nor acquire it. This state is intended to reduce lock
  // contention.
  static constexpr uint64_t FREEZE_BIT = 0x0000000000000002ull;

  // Represents the lock bit,
  // as well as the amount the lock is incremented by on acquire and release.
  static constexpr uint64_t LOCK_BIT = 0x0000000000000004ull;

  // The bits which must be clear if this lock is to be acquired or frozen.
  static constexpr uint64_t LOCK_MASK = FREEZE_BIT + LOCK_BIT;

public:
  /// Static helper method. Determine if a read value returned by an instance of
  /// this class is an orphan.
  static bool is_orphan(uint64_t v) { return (v & ORPHAN_BIT) == ORPHAN_BIT; }

  /// Static helper method. Determine if a read value returned by an instance of
  /// this class is frozen.
  static bool is_frozen(uint64_t v) { return (v & FREEZE_BIT) == FREEZE_BIT; }

  /// Check if a sequence lock is held.
  static bool is_locked(uint64_t v) { return (v & LOCK_BIT) == LOCK_BIT; }

  /// Default constructor; begin unlocked and with all flag bits clear.
  sv_lock() : lock(0) {}

  /// Default constructor; begin unlocked and alive.
  /// Set the orphan bit according to the provided bool argument.
  sv_lock(bool orphan) : lock(orphan ? ORPHAN_BIT : 0) {}

  /// Constructor. Allows caller to manually set the orphan flag.
  /// Also takes a dummy void* that is ignored for template reasons.
  sv_lock(void *, bool orphan) : lock(orphan ? ORPHAN_BIT : 0) {}

  ~sv_lock() {}

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
  bool is_frozen() { return (lock & FREEZE_BIT) == FREEZE_BIT; }
  bool is_locked() { return (lock & LOCK_BIT) == LOCK_BIT; }

  /// Acquire the sequence lock as a writer. Busywait until unlocked and
  /// unfrozen, then (atomically) increment the counter to the next locked
  /// value. As per the invariant, the freeze bit is also set.
  void acquire() {
    uint64_t read_value = lock;
    bool success = false;
    loop_counter ctr;
    while (!success) {
      ctr.count();
      if ((read_value & LOCK_MASK) == 0) {
        uint64_t new_value = read_value + LOCK_MASK; // Set both bits
        success = lock.compare_exchange_weak(read_value, new_value);
      } else {
        read_value = lock;
      }
    }
  }

  /// Acquire a lock frozen by this thread.
  void acquire_frozen() {
    uint64_t read_value = lock;

    // Assert that the freeze bit is set and the lock bit is clear.
    assert((read_value & FREEZE_BIT) != 0 && (read_value & LOCK_BIT) == 0);

    // Since the lock was frozen by this thread, it shouldn't be possible for
    // any other thread to modify the lock concurrently. So, just take it.
    lock = read_value + LOCK_BIT;
  }

  /// Try to upgrade from reader to writer.
  /// Succeeds only if the lock's freeze bit and lock bit are both clear, and
  /// lock's value is unchanged. Otherwise, fails.
  bool try_upgrade(uint64_t v) {
    if ((v & LOCK_MASK) != 0) {
      return false;
    }

    bool success = lock.compare_exchange_strong(v, v + LOCK_MASK);
    return success;
  }

  /// Release the sequence lock after making changes to the protected data. This
  /// increments the counter to the next unlocked value and clears the freeze
  /// bit. Should only be called by the thread that acquired.
  uint64_t release() {
    // Assert that both the lock and freeze bit are set on a lock being
    // released.
    uint64_t read_value = lock;
    assert((read_value & LOCK_MASK) == LOCK_MASK);

    // NB: this addition will have the effect of clearing the freeze and lock
    // bits, and incrementing the counter.
    uint64_t new_value = read_value + FREEZE_BIT;
    lock = new_value;
    return new_value;
  }

  /// Release the sequence lock after making changes to the protected data,
  /// and mark this node as an orphan. Orphaning is irreversible.
  /// This increments the counter to the next unlocked value.
  /// Should only be called by the thread that acquired.
  void release_as_orphan() {
    // Assert that both the lock and freeze bit are set on a lock being
    // released.
    uint64_t read_value = lock;
    assert((read_value & LOCK_MASK) == LOCK_MASK);

    // NB: this addition will have the effect of clearing the freeze and lock
    // bits, and incrementing the counter.
    lock = (read_value + FREEZE_BIT) | ORPHAN_BIT;
  }

  /// Atomically set the freeze bit.
  /// If lock is already held or frozen, spin until it is unlocked and unfrozen.
  bool freeze() {
    uint64_t read_value = lock;
    bool success = false;
    loop_counter ctr;
    while (!success) {
      ctr.count();
      if ((read_value & LOCK_MASK) == 0) {
        uint64_t new_value = read_value + FREEZE_BIT;
        success = lock.compare_exchange_weak(read_value, new_value);
      } else {
        read_value = lock;
      }
    }
    return true;
  }

  /// Clear the freeze bit.
  /// Should only be called by the thread that set it.
  void thaw() {
    // Assert that the lock is in the unlocked and frozen state.
    uint64_t read_value = lock;
    assert((read_value & FREEZE_BIT) != 0 && (read_value & LOCK_BIT) == 0);
    lock = read_value - FREEZE_BIT;
  }

  /// Try to upgrade from reader to freezer.
  /// Succeeds only if the lock's freeze bit and lock bit are both clear, and
  /// lock's value is unchanged. Otherwise, fails.
  bool try_freeze(uint64_t v) {
    if ((v & LOCK_MASK) != 0) {
      return false;
    }

    bool success = lock.compare_exchange_strong(v, v + FREEZE_BIT);
    return success;
  }

  /// Release the sequence lock after making no changes to the protected data.
  /// This decrements the counter down to the previous unlocked value.
  /// Should only be called by the thread that acquired.
  void release_unchanged() {
    // Assert that both the lock and freeze bit are set on a lock being
    // released.
    uint64_t read_value = lock;
    assert((read_value & LOCK_MASK) == LOCK_MASK);
    lock = read_value - LOCK_MASK;
  }

  /// Releases normally if bool b is true.
  /// Releases unchanged if it is false.
  void release_changed_if(bool b) {
    if (b) {
      release();
    } else {
      release_unchanged();
    }
  }

  /// "Acquire" the lock as a reader.
  /// If lock is currently held, returns the previous, unlocked value.
  /// This is for two reasons:
  /// 1) There is a nonzero chance the writer will release_unchanged() and the
  /// read will actually succeed.
  /// 2) This will let it prefetch the memory it will need to work with.
  /// Should only be done when the shared data allows for safe concurrent reads
  /// and writes, and any unsound results from the access can easily be
  /// discarded, reversed, or repaired.
  /// The freeze bit is also cleared as it is irrelevant to a reader.
  uint64_t begin_read() { return lock & ~LOCK_MASK; }

  /// "Release" the lock as a reader.
  /// Basically just checks if the lock has changed.
  /// Reader must abort and try again if it has.
  bool confirm_read(uint64_t old_val) {
    std::atomic_thread_fence(std::memory_order_acquire);
    uint64_t new_val = lock.load(std::memory_order_relaxed);

    // Clear the freeze bit, as it is irrelevant to a reader.
    // We assume the freeze bit on the old_val was cleared by begin_read().
    new_val = new_val & ~FREEZE_BIT;

    return old_val == new_val;
  }

  /// Conclude a read with no regard for whether it was successful.
  /// In this implementation, this is a no-op.
  void abort_read() {}

  /// Just get the value of the sequence lock directly.
  /// For debug purposes only.
  uint64_t get_value() { return lock; }

  void dump() {
    uint64_t read_value = lock;

    if (is_orphan(read_value)) {
      std::cout << "o"; // orphan
    } else {
      std::cout << "C"; // child
    }

    if (is_frozen(read_value)) {
      std::cout << "F"; // frozen
    } else {
      std::cout << "T"; // thawed
    }

    if (is_locked(read_value)) {
      std::cout << "L"; // locked
    } else {
      std::cout << "U"; // unlocked
    }

    std::cout << +(read_value / LOCK_BIT);
  }
};
