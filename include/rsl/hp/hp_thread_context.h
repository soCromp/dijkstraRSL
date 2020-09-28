#include <unordered_set>

#include "hp_deletable.h"

/// A thread hazard pointer context. This specific context type assumes we
/// are doing hand-over-hand traversals, and thus only ever need two
/// hazard pointers. The two hazard pointers are called "curr" and "next."
///
/// hp_thread_context tracks two things.  First, it has a small number (2)
/// of hazard pointers: shared single-writer, multi-reader locations where a
/// thread can store the locations it is protecting.  Second, it has a large
/// segment of memory that stores retired pointers that are not yet reclaimed
/// (presumably because of a product of laziness and the hazard pointers held by
/// other threads).
///
/// ZOMBIE_CAP : The capacity of the zombie array.
template <int ZOMBIE_CAP> class hp_thread_context {

  // Returns the index of the slot that is not curr_idx.
  // If curr_idx is 0, returns 1. If curr_idx is 1, returns 0.
  // Unless there is a bug in this class, curr_idx should never take any other
  // value.
  int next_idx() { return 1 - curr_idx; }

  /// This variable indicates which of the two hazard pointer slots contains
  /// "curr." This makes the other "next," naturally.
  int curr_idx = 0;

public:
  /// The current size of the zombies array.
  int count = 0;

  /// The set of objects to be reclaimed.
  hp_deletable *zombies[ZOMBIE_CAP];

  /// The hazard pointers held by this thread.
  /// It is hard-coded at two because that meets the needs of this use case.
  std::atomic<hp_deletable *> hazP[2];

  /// The threads' HP contexts form a linked list. The order in the list is
  /// not important, but we need a next pointer to create the list.
  hp_thread_context *next = nullptr;

  /// Create a thread's hazard pointer context.
  hp_thread_context() {
    hazP[0].store(nullptr, std::memory_order_relaxed);
    hazP[1].store(nullptr, std::memory_order_relaxed);
  }

  /// When destroying the thread hazard pointer context, we assume that the
  /// program has shifted to a new phase (possibly sequential), and thus
  /// all zombies can now safely be reclaimed, so we do just that.
  ~hp_thread_context() {
    for (; count > 0; --count) {
      delete zombies[count - 1];
    }
  }

  /// Protect a location by taking a hazard pointer on it.
  /// Assumes no hazard pointers are held by the thread yet.
  void take_first(hp_deletable *ptr) {
    hazP[0].store(ptr, std::memory_order_relaxed);
    curr_idx = 0;
  }

  /// Protect a location by taking a hazard pointer on it. Assumes there is
  /// exactly one other hazard pointer currently held. If none are held, use
  /// take_first() instead. If two are held, no more can be taken.
  void take(hp_deletable *ptr) {
    hazP[1 - curr_idx].store(ptr, std::memory_order_relaxed);
  }

  /// Count how many hazard pointers are reserved by this thread.
  int count_reserved() {
    int result = 0;

    for (int i = 0; i < 2; ++i) {
      if (hazP[i].load(std::memory_order_relaxed) != nullptr) {
        ++result;
      }
    }

    return result;
  }

  /// Drop the current hazard pointer. If two hazard pointers are held, this
  /// causes the next hazard pointer to become the current hazard pointer.
  void drop_curr() {
    hazP[curr_idx].store(nullptr, std::memory_order_relaxed);

    // Switch curr_slot to the other slot. If there is a node in the other slot,
    // this is necessary; if there isn't, this is harmless.
    // It's cheaper than an if statement, so just do it.
    curr_idx = next_idx();
  }

  /// Drop the hazard pointer on next.
  /// This is only valid when two hazard pointers are held.
  void drop_next() {
    hazP[next_idx()].store(nullptr, std::memory_order_relaxed);
  }

  /// Drop all hazard pointers held by this thread.
  void drop_all() {
    hazP[0].store(nullptr, std::memory_order_relaxed);
    hazP[1].store(nullptr, std::memory_order_relaxed);
    curr_idx = 0;
  }

  /// Reclaim all zombies that aren't in use.
  /// The provide unordered set indicates which ones are in use.
  void sweep(std::unordered_set<hp_deletable *> &in_use) {
    int i = 0;
    while (i < count) {
      hp_deletable *zombie = zombies[i];
      if (in_use.count(zombie) > 0) {
        // Someone else has a hazard pointer on this zombie, so don't delete it
        // yet. Just move onto the next item.
        ++i;
      } else {
        // No one else has a hazard pointer on this zombie, so delete it, and
        // move the last element into this empty slot to maintain compactness.
        delete zombie;
        zombies[i] = zombies[--count];
      }
    }
  }
};
