#pragma once

#include <atomic>
#include <cassert>

#include "../lock/loop_counter.h"

/// FIXME: The classes that use this class should be refactored to instead use
/// sv_lock, and this class should be deleted.

/// An implementation of a sequence lock to meet the specific needs of the
/// concurrent skipvector. Steals two flag bits to represents the states a
/// skipvector node can be in.
class sv_lock_old {
  std::atomic<uint64_t> lock;

  // Bit masks representing the position of flag bits.
  static constexpr uint64_t DEAD_BIT = 0x0000000000000001ull;
  static constexpr uint64_t ORPHAN_BIT = 0x0000000000000002ull;

  // Represents the lock bit,
  // as well as the amount the lock is incremented by on acquire and release.
  static constexpr uint64_t LOCK_BIT = 0x0000000000000004ull;

  // The bits which must be clear if this lock is to be acquired.
  static constexpr uint64_t LOCK_MASK = DEAD_BIT + LOCK_BIT;

public:
  /// Static helper method. Determine if a read value returned by an instance of
  /// this class is an orphan.
  static bool is_orphan(uint64_t v) { return (v & ORPHAN_BIT) == ORPHAN_BIT; }

  /// Static helper method. Determine if a read value returned by an instance of
  /// this class is dead.
  static bool is_dead(uint64_t v) { return (v & DEAD_BIT) == DEAD_BIT; }

  /// Check if a sequence lock is held.
  static bool is_locked(uint64_t v) { return (v & LOCK_BIT) == LOCK_BIT; }

  /// Default constructor; begin unlocked and with all flag bits clear.
  sv_lock_old() : lock(0) {}

  /// Default constructor; begin unlocked and alive.
  /// Set the orphan bit according to the provided bool argument.
  sv_lock_old(bool orphan) : lock(orphan ? ORPHAN_BIT : 0) {}

  /// Constructor. Allows caller to manually set the orphan flag.
  /// Also takes a dummy void* that is ignored for template reasons.
  sv_lock_old(void *, bool orphan) : lock(orphan ? ORPHAN_BIT : 0) {}

  ~sv_lock_old() {}

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
  bool is_dead() { return (lock & DEAD_BIT) == DEAD_BIT; }
  bool is_locked() { return (lock & LOCK_BIT) == LOCK_BIT; }

  /// Acquire the sequence lock as a writer.
  /// Busywait until unlocked,
  /// then (atomically) increment the counter to the next locked value.
  bool acquire() {
    uint64_t read_value = lock;
    bool success = false;
    loop_counter ctr;
    while (!success) {
      ctr.count();
      if ((read_value & LOCK_MASK) == 0) {
        uint64_t new_value = read_value + LOCK_BIT;
        success = lock.compare_exchange_weak(read_value, new_value);
      } else if ((read_value & DEAD_BIT) != 0) {
        return false;
      } else {
        read_value = lock;
      }
    }
    return true;
  }

  /// Try to upgrade from reader to writer.
  /// Succeeds only if the lock's dead bit and lock bit are both clear, and
  /// lock's value is unchanged. Otherwise, fails.
  bool upgrade(uint64_t v) {
    if ((v & LOCK_MASK) != 0) {
      return false;
    }

    bool success = lock.compare_exchange_strong(v, v + LOCK_BIT);
    return success;
  }

  /// Release the sequence lock after making changes to the protected data.
  /// This increments the counter to the next unlocked value.
  /// Should only be called by the thread that acquired.
  uint64_t release() {
    // Only held locks should be released.
    assert((lock & LOCK_BIT) != 0);
    return lock += LOCK_BIT;
  }

  /// Release the sequence lock after making changes to the protected data,
  /// and mark this node as an orphan. Orphaning is irreversible.
  /// This increments the counter to the next unlocked value.
  /// Should only be called by the thread that acquired.
  void release_as_orphan() {
    // Only held locks should be released.
    assert((lock & LOCK_BIT) != 0);
    lock = (lock + LOCK_BIT) | ORPHAN_BIT;
  }

  /// Release the sequence lock after making changes to the protected data,
  /// and mark this node as dead. Death is irreversible.
  /// This increments the counter to the next unlocked value.
  /// Should only be called by the thread that acquired.
  void release_as_dead() {
    // Only held locks should be released.
    assert((lock & LOCK_BIT) != 0);
    lock = (lock + LOCK_BIT) | DEAD_BIT;
  }

  /// Release the sequence lock after making no changes to the protected data.
  /// This decrements the counter down to the previous unlocked value.
  /// Should only be called by the thread that acquired.
  void release_unchanged() {
    // Only held locks should be released.
    assert((lock & LOCK_BIT) != 0);
    lock -= LOCK_BIT;
  }

  /// Releases normally if bool b is true.
  /// Releases unchanged if it is false.
  void release_changed_if(bool b) {
    // Only held locks should be released.
    assert((lock & LOCK_BIT) != 0);

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
  uint64_t begin_read() { return lock & (~LOCK_BIT); }

  /// "Release" the lock as a reader.
  /// Basically just checks if the lock has changed.
  /// Reader must abort and try again if it has.
  bool confirm_read(uint64_t v) {
    std::atomic_thread_fence(std::memory_order_acquire);
    return lock.load(std::memory_order_relaxed) == v;
  }

  /// Just get the value of the sequence lock directly.
  /// For debug purposes only.
  uint64_t get_value() { return lock; }

  void dump() {
    uint64_t read_value = lock;

    if (is_orphan(read_value)) {
      std::cout << "o";
    } else {
      std::cout << "C";
    }

    if (is_dead(read_value)) {
      std::cout << "D";
    } else {
      std::cout << "A";
    }

    if (is_locked(read_value)) {
      std::cout << "L";
    } else {
      std::cout << "U";
    }

    std::cout << +(read_value / LOCK_BIT);
  }
};
