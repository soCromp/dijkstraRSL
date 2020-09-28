#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <functional>

#include "../include/rsl/common/config.h"
#include "../include/rsl/common/rlx_atomic.h"
#include "../include/rsl/hp/hp_deletable.h"
#include "../include/rsl/lock/sv_lock.h"
#include "../include/rsl/rng/lehmer64.h"
#include "../include/rsl/vector_sfra.h"
#include "../include/rsl/hp/hp_manager.h"
#include "../include/rsl/hp/hp_manager_leaky.h"

#include "rsl_c.h"

#include <iostream>

/// This class is the default implementation of the skip vector. It supports
/// foreach and range operations using lazy two-phase locking for concurrency
/// control.  That is, it does not lock its entire working right away.  Instead,
/// it begins traversing the SkipVector and locking elements.  As it locks
/// elements, it operates on them.  Finally, when it has finished locking
/// elements and operating on them, then it releases all of its locks.
///
/// skipvector lazily merges orphans and uses non-resizable vectors.
///
/// Template Parameters:
///   K            - The type of the key for k/v pairs
///   V            - The type of the value for k/v pairs
///   DATA_VECTOR  - A vector type that can hold k/v pairs for the data layer
///   INDEX_VECTOR - A vector type that can hold k/v pairs for the index layer
///   DATA_EXP     - The log_2 of the target chunk size for data layer vectors - 5
///   INDEX_EXP    - The log_2 of the target chunk size for index layer vectors - 5
///   LAYERS       - The number of index layers - 32
///   HP           - The class responsible for managing hazard pointers.
  //,
//           template <typename, typename, int> typename DATA_VECTOR,
//           template <typename, typename, int> typename INDEX_VECTOR,
//           int DATA_EXP, int INDEX_EXP, int LAYERS, typename HP>
class rsl {

  const static int INDEX_EXP = 5;
  const static int DATA_EXP = 5;
  const static int LAYERS = 32;
  typedef slkey_t K;
  typedef slkey_t V;
  typedef hp_manager<200> HP;

  /// node_t is used for both the data layer and the index layer(s)
  template <typename T, int EXP,
            template <typename, typename, int> typename VECTOR>
  struct node_t : public hp_deletable {
    /// A lock to protect this node.  sv_lock is a sequence lock with a
    /// few stolen bits
    sv_lock lock;

    static constexpr int get_vector_size() {
      if (EXP == 0) {
        // Special case: if EXP equals 0, we hack the vector size to 1 so it
        // behaves as much like a skip list as possible.
        return 1;
      }

      // General case: choose a size that can hold 2 * 2^EXP elements.
      return 2 << EXP;
    }

    /// A vector of key/value pairs.
    VECTOR<K, T, 32> v; //TODO: hardcoded chunk size to 32. used to be get_vector_size()

    /// Pointer to next vector at this layer.
    rlx_atomic<node_t *> next = nullptr;

    /// Default constructor; creates node as orphan. This constructor is only
    /// ever used to create leftmost nodes, which are always orphans.
    node_t() : lock(true), v() {}

    /// Constructor; creates node, and stitches it in after a given other node.
    ///
    /// NB: Assumes that the caller has locked /prev/
    node_t(node_t *prev, bool orphan) : lock(orphan), v() {
      next = prev->next.load();
      prev->next = this;
    }

    /// Make the destuctor virtual, to simplify/optimize hazard pointer code
    virtual ~node_t() {}

    /// Merge the next node into this node, and unlink next node
    ///
    /// NB: The caller is expected to handle reclamation of unlinked node
    void merge() {
      node_t *zombie = next;
      v.merge(&(zombie->v));
      next = zombie->next.load();
      zombie->lock.release();
    }

    /// Insert a K/V pair into this node
    ///
    /// NB: May split this node if it's full
    bool insert(const std::pair<const K, T> &pair) {
      bool overfull = false;
      bool result = v.insert(pair, overfull);
      if (overfull) {
        // Insert failed because the current node was too big,
        // so split it and make an orphan.
        // Note: the orphan's constructor will stitch itself in.
        node_t *new_orphan = new node_t(this, true);
        new_orphan->v.steal_half_and_insert(&v, pair);
        return true;
      }
      return result;
    }

    /// Sequential code for checking if a node is an orphan
    ///
    /// NB: Concurrent methods should read the orphan bit from the seqlock
    bool is_orphan_seq() { return sv_lock::is_orphan(lock.get_value()); }
  };

  /// type of index nodes.  Since an index node can reference either another
  /// index node, or a data node, we use a generic void*.  Thus the map holds
  /// K/ptr pairs
  

  typedef node_t<void *, INDEX_EXP, vector_sfra> index_t; //TODO: vector type is hardcoded here

  /// type of data nodes.  A data node's vector holds k/v pairs
  typedef node_t<V, DATA_EXP, vector_sfra> data_t; //TODO: vector type is hardcoded here

  /// The threshold at which to merge chunks of the skipvector
  const double merge_threshold = 1.0; //TODO: hardcoded

  /// The number of index layers in the skipvector.
  ///
  /// NB: This does not include the data layer
  const int layers = 32; //TODO: hardcoded

  /// Array of leftmost index nodes.
  index_t index_head[LAYERS];

  /// Leftmost data vector.
  data_t dh;
  data_t *data_head;

  /// Create a context for the thread, if one doesn't exist
  void init_context() { HP::init_context(); }

  /// Minchunk
  data_t minchunk;
  int64_t minchunk_size = 1; //size of minchunk

  int64_t get_minchunk_nextind() { //get the slot in minchunk vector from which to exmin
    return --minchunk_size;
  }

  /// Generate height using a geometric distribution from 0 to layers.
  /// A height of n means it exists in the bottommost n index layers, and also
  /// the data layer. A height of zero means it exists solely in the data layer.
  int random_height() {
    // In the general case, set the target chunk size to 2^n, where n is the
    // templated exponent parameter. In the special skiplist simulation case,
    // where the exponent is set to 0, use the value 2 instead.
    constexpr int target_data_chunk_size = DATA_EXP == 0 ? 2 : 1 << DATA_EXP;
    constexpr int target_idx_chunk_size = INDEX_EXP == 0 ? 2 : 1 << INDEX_EXP;

    static thread_local __uint128_t g_lehmer64_state =
        lehmer64_seed(pthread_self());
    uint64_t r = lehmer64(g_lehmer64_state);
    int result = 0;

    // The probability that r is divisible by target_chunk_size is exactly 1 /
    // target_chunk_size (assuming RAND_MAX + 1 is a power of 2.)
    //
    // The first iteration of the loop is unrolled to specially handle the data
    // layer.
    //
    // NB: The trick here is that we can look for a series of low 0 bits, and
    //     use that to both (a) check many bits in parallel, and (b)
    //     short-circuit the search when we find any non-zero bits.
    if (r % target_data_chunk_size == 0) {
      // Use a right shift to remove the used bits.
      r = r >> DATA_EXP;

      for (result = 1; r % target_idx_chunk_size == 0 && result < layers;
           ++result) {
        // Use a right shift to remove the used bits.
        r = r >> INDEX_EXP;
      }
    }

    return result;
  }

  /// Checks two vectors to see if merging them is sensible. It's sensible if
  /// the sum of their sizes is under the merge threshold, or if b is totally
  /// empty.
  bool should_merge(index_t *a, index_t *b) {
    // Merges should never happen in skiplist simulation mode unless b is
    // totally empty. As INDEX_EXP is a templated value, this check gets
    // optimized out by the compiler.
    if (INDEX_EXP == 0)
      return b->v.get_size() == 0;

    const uint16_t idx_merge_threshold = merge_threshold * (1 << INDEX_EXP);
    return b->v.get_size() == 0 ||
           (a->v.get_size() + b->v.get_size() < idx_merge_threshold);
  }

  bool should_merge(data_t *a, data_t *b) {
    // Merges should never happen in skiplist simulation mode unless b is
    // totally empty. As DATA_EXP is a templated value, this check gets
    // optimized out by the compiler.
    if (DATA_EXP == 0)
      return b->v.get_size() == 0;

    const uint16_t data_merge_threshold = merge_threshold * (1 << DATA_EXP);
    return b->v.get_size() == 0 ||
           (a->v.get_size() + b->v.get_size() < data_merge_threshold);
  }

  /// Helper function that determines if a search should continue to the next
  /// chunk, and if so, it advances curr, repeatedly until no more advancing is
  /// necessary.
  ///
  ///  If parameter "cleanup" is set to true, this function will merge orphans
  ///  as necessary when they are found. If it is not, will only clean up empty
  ///  orphans. The cleanup flag is set to true by insert() and remove();
  ///  contains() sets it to false, to keep contains() fast.
  ///
  /// @returns true if successful, false on a seqlock verification failure
  template <typename T>
  bool check_next(T *&curr, uint64_t &curr_lock, const K &k, bool cleanup) {
    // The fastest way out of this loop is when next is nullptr or curr's last
    // element is >= k.  Finding these early avoids taking a hazard pointer on
    // next or reading its seqlock.  If the /while/ condition fails, we will
    // return true.
    T *next = curr->next;
    K last = k;
    while (next != nullptr && (!curr->v.last(last) || k > last)) {
      // Take a hazard pointer on next, then make sure curr hasn't changed
      HP::take(next);
      if (!curr->lock.confirm_read(curr_lock)) {
        HP::drop_next();
        return false;
      }

      uint64_t next_lock = next->lock.begin_read();

      // Check if /next/ needs to be removed (and possibly merged first)
      // - remove if it's an empty orphan
      // - merge+remove if it's an orphan and cleanup == should_merge() == true
      //
      // [mfs] The guard for this /if/ is the same as the guard for the
      //       do/while.  It's probably possible to refactor into a single
      //       /while/ loop
      if (sv_lock::is_orphan(next_lock) &&
          ((cleanup && should_merge(curr, next)) || next->v.get_size() == 0)) {

        // [mfs] This logic should be very infrequently needed.  I think we'd be
        //       better having it in a separate function, so that hopefully it
        //       doesn't get inlined

        // Get write lock on curr, since we'll modify curr->next
        if (!curr->lock.try_upgrade(curr_lock)) {
          HP::drop_next();
          return false;
        }

        // We unlink in a loop, since there may be multiple orphans
        bool changed = false;
        do {
          // Acquire next, so we can mark it deleted
          if (!next->lock.try_upgrade(next_lock)) {
            curr->lock.release_changed_if(changed); // may downgrade curr->lock
            HP::drop_next();
            return false;
          }

          // Unlink it, and mark it for reclamation.
          curr->merge();
          HP::drop_next();
          HP::reclaim(next);
          next = curr->next;
          changed = true;

          // We may need to keep looping.  Next==null is the easy exit case
          if (next == nullptr) {
            curr_lock = curr->lock.release();
            return true;
          }

          // NB: We don't have to take a hazard pointer on next because we have
          //     its predecessor locked as a writer.  Even if it's not an
          //     orphan, it can't be deleted without holding a lock on its
          //     predecessor, and we have that lock.
          next_lock = next->lock.begin_read();
        } while (
            sv_lock::is_orphan(next_lock) &&
            ((cleanup && should_merge(curr, next)) || next->v.get_size() == 0));

        if (cleanup) {
          // If the cleanup flag is enabled, merging may have eliminated the
          // need to check next, so start again from the top.
          //
          // NB: curr->lock.release() still gives us a read lock on curr
          curr_lock = curr->lock.release();
          continue;
        } else {
          // Before we release the write lock on curr, we need to take a hazard
          // pointer on next, so that we can continue to access it safely after
          // the release.
          HP::take(next);
          curr_lock = curr->lock.release();
        }
      }

      // At this point we know that we have a nonempty next.
      if (k < next->v.first()) {
        // Next's first element is after k, so we have ruled out next.
        // Now we just need to check its sequence lock.
        // Return true if the check succeeds, false if it fails.
        bool result = next->lock.confirm_read(next_lock);
        HP::drop_next();
        return result;
      }

      // Next's first element is before (or equal to) the sought key,
      // so we to go to next and repeat from there. We're done with curr,
      // so we just need to confirm its sequence lock hasn't changed.
      if (!curr->lock.confirm_read(curr_lock)) {
        HP::drop_next();
        return false;
      }

      curr = next;
      curr_lock = next_lock;
      next = curr->next;
      HP::drop_curr();
    }

    // We ruled out next, so return true.
    return true;
  }

  /// Sequential-only variant of check_next.
  /// Skips the merge logic.
  template <typename T> void check_next_sequential(T *&curr, const K &k) {
    T *next = curr->next;
    K last = k;
    while (next != nullptr && (!curr->v.last(last) || k > last)) {
      if (next->lock.is_orphan() &&
          (should_merge(curr, next) || next->v.get_size() == 0)) {

        do {
          // Unlink and immediately reclaim next
          // (no need for hazard pointers when running in isolation.)
          // NB: merge() expects next's lock to be held, so we have to take it,
          // even though this method is sequential-only.
          next->lock.acquire();
          curr->merge();
          delete next;
          next = curr->next;

          if (next == nullptr) {
            return;
          }

        } while (next->lock.is_orphan() &&
                 (should_merge(curr, next) || next->v.get_size() == 0));
      }

      if (k < next->v.first()) {
        return;
      }

      curr = next;
      next = curr->next;
    }

    return;
  }

  /// Given a node /curr/ that is read locked, give up that lock, and replace it
  /// with a read lock on new_node.  Also drops HP on curr, takes HP on new_node
  ///
  /// @returns true if successful, false if failed.
  template <typename T>
  bool reader_swap(index_t *curr, uint64_t &curr_lock, T *new_node) {
    // Take a hazard pointer on new_node, make sure curr hasn't changed
    HP::take(new_node);
    if (!curr->lock.confirm_read(curr_lock)) {
      HP::drop_next();
      return false;
    }

    // read-lock new_node, then check curr hasn't changed
    //
    // [mfs]: double-check on curr may be redundant?
    uint64_t new_lock = new_node->lock.begin_read();
    if (!curr->lock.confirm_read(curr_lock)) {
      HP::drop_next();
      return false;
    }

    // finish the swap
    curr_lock = new_lock;
    HP::drop_curr();
    return true;
  }

  /// follow() is used by contains to find the correct down pointer from curr.
  /// follow() also swaps the lock on curr for a lock on the new down node
  template <typename T>
  bool follow(index_t *curr, uint64_t &curr_lock, const K &k, T *&down) {
    // if check_next() fails, start over
    if (!check_next<index_t>(curr, curr_lock, k, false)) {
      return false;
    }

    // Find down pointer in curr, confirm curr's sequence lock (and next's, if
    // next was read), and take a seqlock on down.
    void *down_void = nullptr;
    if (curr->v.find_lte(k, down_void)) {
      down = static_cast<T *>(down_void);
    }

    return reader_swap<T>(curr, curr_lock, down);
  }

  /// skip_to is used by range operations to find the correct starting point in
  /// the data layer
  data_t *skip_to(const K &k) {
    // [mfs] Instead of goto, can we use recursion?
  top:
    // Start from the head node (leftmost node in topmost layer.)
    int layer = layers - 1;
    index_t *curr = &(index_head[layer]);

    // Read head node's sequence lock.
    HP::take_first(curr);
    uint64_t curr_lock = curr->lock.begin_read();

    // Skip through all index layers but the last.
    for (; layer >= 1; --layer) {
      // If follow() doesn't find a suitable down pointer,
      // default to next index layer's head.
      index_t *down = &index_head[layer - 1];
      if (!follow<index_t>(curr, curr_lock, k, down)) {
        // Sequence lock check failed
        HP::drop_all();
        goto top;
      }
      curr = down;
    }

    // Skip through the last index layer.
    data_t *curr_dl = data_head;
    if (!follow<data_t>(curr, curr_lock, k, curr_dl)) {
      // Sequence lock check failed
      HP::drop_all();
      goto top;
    }

    // Upgrade curr_dl's lock from reader to writer.
    if (!curr_dl->lock.try_upgrade(curr_lock)) {
      // Sequence lock check failed
      HP::drop_all();
      goto top;
    }

    // We now have a true mutex on curr_dl,
    // so we can drop all of our hazard pointers.
    HP::drop_all();

    // curr_dl is now locked by the traversal.
    return curr_dl;
  }

public:
  /// insert() takes a reference to a k/v pair, so we expose the type here
  typedef std::pair<const K, V> value_type;

  /// Benchmark constructor
  rsl(config *cfg)
      : merge_threshold(cfg->merge_threshold), layers(cfg->layers) {

    // Make sure number of layers is valid.
    assert(layers > 0 && layers <= LAYERS);

    // We use a single 64-bit random number on insert(), so make sure that's
    // enough for the chosen configuration.
    assert(DATA_EXP + (cfg->layers * INDEX_EXP) <= 64);

    data_head = &dh;
  }


  //other constructor
  rsl() {

    data_head = &dh;
  }

  /// Sequential-only destructor
  ~rsl() {
    // First, free all index layer nodes BUT the leftmost ones.
    for (int i = 0; i < layers; ++i) {
      index_t *curr = index_head[i].next;
      while (curr != nullptr) {
        index_t *next = curr->next;
        delete curr;
        curr = next;
      }
    }

    // Free each node in data layer but the leftmost,
    // which was statically allocated
    data_t *data_curr = data_head->next;
    while (data_curr != nullptr) {
      data_t *next = data_curr->next;
      delete data_curr;
      data_curr = next;
    }

    // Thoroughly sweep the hazard pointer lists to clean up any remaining
    // unreclaimed nodes from this skip vector.
    HP::sweep_all();
  }

  // Sequential-only teardown method.
  static void tear_down() { HP::tear_down(); }

  /// Search for a key in the skipvector
  /// Returns true if found, false if not found
  /// "val" parameter is used to pass back the value if found
  bool contains(const K &k, V &v) {
    init_context();

  top:
    // Start from the head node (leftmost node in topmost layer.)
    int layer = layers - 1;
    index_t *curr = &(index_head[layer]);

    // Read head node's sequence lock.
    HP::take_first(curr);
    uint64_t curr_lock = curr->lock.begin_read();

    // Skip through all index layers but the last.
    for (; layer >= 1; --layer) {
      // If follow() doesn't find a suitable down pointer,
      // default to next index layer's head.
      index_t *down = &index_head[layer - 1];
      if (!follow<index_t>(curr, curr_lock, k, down)) {
        // Sequence lock check failed
        HP::drop_all();
        goto top;
      }
      curr = down;
    }

    // Skip through the last index layer.
    data_t *curr_dl = data_head;
    if (!follow<data_t>(curr, curr_lock, k, curr_dl)) {
      // Sequence lock check failed
      HP::drop_all();
      goto top;
    }

    // Finally, read the data layer.
    if (!check_next<data_t>(curr_dl, curr_lock, k, false)) {
      HP::drop_all();
      goto top;
    }

    // Scan curr_dl for the sought value.
    // NB: There is a chance that this contains() will find a value,
    // but the final sequence lock checks will fail, making us start over,
    // and the element will be gone by the time we get back here.
    // This scenario would yield the somewhat undesirable result that this
    // method returns false but overwrites v with an outdated value.
    // To prevent this, we use a temporary intermediate variable, tmp.
    V tmp = v;
    bool result = curr_dl->v.contains(k, tmp);

    // Confirm curr's sequence lock.
    if (!curr_dl->lock.confirm_read(curr_lock)) {
      HP::drop_all();
      goto top;
    }

    HP::drop_all();

    if (result)
      v = tmp;

    return result;
  }

  /// Insert a new element into the map
  bool insert(const value_type &pair) {
    init_context();

    const K &k = pair.first;

    // Pre-generate a new height for the node
    int new_height = random_height();

    // Do a lookup as though doing a contains() operation, but save references
    // to index nodes we'll need later in an array.
    index_t *frozen_nodes[LAYERS] = {nullptr};

  top:

    // Start at the topmost index layer.
    int layer = layers - 1;
    index_t *curr = &(index_head[layer]);
    HP::take_first(curr);
    uint64_t curr_lock = curr->lock.begin_read();
    data_t *curr_dl = data_head;

    // checkpoint is a "safe node" that we know won't be deleted, because either
    // we hold the lock on its parent, or it is the head node of its layer.
    // If we have a sequence lock check fail during execution, and we have a
    // checkpoint, we can jump back to it rather than start all over.
    index_t *checkpoint = nullptr;
    data_t *checkpoint_dl = nullptr;
    bool load_checkpoint = false;

    // Skip through index layers.
    while (layer >= 0) {

      // Load the checkpoint if the appropriate flag is set.
      if (load_checkpoint) {
        HP::drop_all();
        load_checkpoint = false;
        if (checkpoint == nullptr) {
          // No checkpoint was set, so retry from start.
          goto top;
        }
        curr = checkpoint;

        // NB: We know the checkpoint won't be deleted, so we do not need to
        // double-check the hazard pointer we take on it.
        HP::take_first(curr);
        curr_lock = curr->lock.begin_read();
      }

      // Check next, as it may need to be maintained or followed.
      if (!check_next<index_t>(curr, curr_lock, k, true)) {
        load_checkpoint = true;
        continue;
      }

      // If the inserted node is tall enough, lock this node and save it.
      if (layer < new_height) {
        if (!curr->lock.try_freeze(curr_lock)) {
          load_checkpoint = true;
          continue;
        }

        frozen_nodes[layer] = curr;
      }

      // Now, search the vector we arrived at for the right down pointer.
      void *down = nullptr;
      index_t *down_idx = nullptr;
      K found_k = k;

      if (curr->v.find_lte(k, found_k, down)) {
        if (found_k == k) {
          // If we find k in the index layer, stop and return false.
          if (layer < new_height) {
            // If we froze any nodes, we must thaw them before returning.
            for (int i = layer; i < new_height; ++i) {
              frozen_nodes[i]->lock.thaw();
            }
          } else {
            // We don't have curr locked,
            // so we must validate its sequence lock before returning.
            if (!curr->lock.confirm_read(curr_lock)) {
              load_checkpoint = true;
              continue;
            }
          }
          HP::drop_all();
          return false;
        }

        // Otherwise, we found an appropriate down pointer, so follow it.
        if (layer > 0) {
          down_idx = static_cast<index_t *>(down);
        } else {
          curr_dl = static_cast<data_t *>(down);
        }
      } else {
        // No appropriate down pointer was found,
        // so start at the leftmost node at the next layer.
        if (layer > 0) {
          down_idx = &(index_head[layer - 1]);
        } else {
          curr_dl = data_head;
        }
      }

      if (layer < new_height) {
        // If we have locked curr, then we can use down as a checkpoint.
        if (layer > 0) {
          HP::take(down_idx);
          HP::drop_curr();
          curr = down_idx;
          curr_lock = down_idx->lock.begin_read();
          checkpoint = down_idx;
        } else {
          HP::take(curr_dl);
          HP::drop_curr();
          curr_lock = curr_dl->lock.begin_read();
          checkpoint_dl = curr_dl;
        }
      } else {
        // If we have not locked curr,
        // we must safely exchange locks and hazard pointers.
        if (layer > 0) {
          if (!reader_swap<index_t>(curr, curr_lock, down_idx)) {
            load_checkpoint = true;
            continue;
          }
          curr = down_idx;
        } else {
          if (!reader_swap<data_t>(curr, curr_lock, curr_dl)) {
            load_checkpoint = true;
            continue;
          }
        }
      }

      --layer;
    }

  retry_dl:
    // At this point we should be at the data layer.

    // Check if we have to follow any next pointers.
    bool check_next_success = check_next<data_t>(curr_dl, curr_lock, k, true);

    // Now, acquire curr_dl as a writer.
    if (!check_next_success || !curr_dl->lock.try_upgrade(curr_lock)) {
      // Go back to checkpoint_dl if it exists, or start over from top.
      HP::drop_all();

      if (checkpoint_dl != nullptr) {
        curr_dl = checkpoint_dl;
        curr_lock = curr_dl->lock.begin_read();
        HP::take_first(curr_dl);
        goto retry_dl;
      } else {
        goto top;
      }
    }

    // Common case: generated height is 0,
    // so simply attempt to insert it into the data layer.
    if (new_height == 0) {
      bool result = curr_dl->insert(pair);
      curr_dl->lock.release_changed_if(result);
      HP::drop_all();
      return result;
    }

    // Generated height is at least 1, so we need to partition the data node.
    // First we must manually check if the key is present in the data node.
    if (curr_dl->v.contains(k)) {
      // If key is present, thaw everything and just return false.
      for (int i = 0; i < new_height; ++i) {
        frozen_nodes[i]->lock.thaw();
      }
      curr_dl->lock.release_unchanged();
      HP::drop_all();
      return false;
    }

    // Key isn't present, so do the partition.

    // Edge case: curr_dl may be full and the inserted key may be less than
    // its minimum. (This can only happen if it is leftmost.)
    // If this is the case, we must first partition curr_dl.
    if (curr_dl->v.get_size() == curr_dl->v.get_capacity() &&
        k < curr_dl->v.first()) {
      data_t *new_orphan = new data_t(curr_dl, true);
      new_orphan->lock.acquire();
      new_orphan->v.steal_half(&(curr_dl->v));
      new_orphan->lock.release();
    }

    data_t *new_data_node = new data_t(curr_dl, false);
    new_data_node->lock.acquire();
    new_data_node->v.insert_and_steal_greater(&(curr_dl->v), pair);
    new_data_node->lock.release();
    curr_dl->lock.release();

    void *down_ptr = new_data_node;

    // Partition any index layers that need partitioning,
    // and insert down pointers.
    // NB: This loop's range excludes the top layer
    // because we do not partition at the top layer
    for (int i = 0; i + 1 < new_height; ++i) {
      index_t *victim = frozen_nodes[i];

      victim->lock.acquire_frozen();

      // Same edge case as above, just for index layer nodes
      if (victim->v.get_size() == victim->v.get_capacity() &&
          k < victim->v.first()) {
        index_t *new_index_orphan = new index_t(victim, true);
        new_index_orphan->v.steal_half(&(victim->v));
      }

      index_t *new_index_node = new index_t(victim, false);
      new_index_node->v.insert_and_steal_greater(&(victim->v),
                                                 std::make_pair(k, down_ptr));

      victim->lock.release();

      down_ptr = new_index_node;
    }

    // Finally, at the pre-generated height,
    // simply insert the appropriate down pointer into the appropriate vector.
    index_t *top_node = frozen_nodes[new_height - 1];
    top_node->lock.acquire_frozen();
    frozen_nodes[new_height - 1]->insert(std::make_pair(k, down_ptr));
    top_node->lock.release();

    HP::drop_all();
    return true;
  }

  /// Insert a new element into the map, in isolation for setup
  bool insert_sequential(const value_type &pair) {
    const K &k = pair.first;

    // Pre-generate a new height for the node
    int new_height = random_height();

    // Do a lookup as though doing a contains() operation, but save references
    // to index nodes we'll need later in an array.
    index_t *prev_nodes[LAYERS] = {nullptr};

    // Start at the topmost index layer.
    int layer = layers - 1;
    index_t *curr = &(index_head[layer]);
    data_t *curr_dl = data_head;

    // Skip through index layers.
    while (layer >= 0) {
      check_next_sequential<index_t>(curr, k);

      // If the inserted node is tall enough, lock this node and save it.
      if (layer < new_height) {
        prev_nodes[layer] = curr;
      }

      // Now, search the vector we arrived at for the right down pointer.
      void *down = nullptr;
      index_t *down_idx = nullptr;
      K found_k = k;

      if (curr->v.find_lte(k, found_k, down)) {
        if (found_k == k) {
          return false;
        }

        // Otherwise, we found an appropriate down pointer, so follow it.
        if (layer > 0) {
          down_idx = static_cast<index_t *>(down);
        } else {
          curr_dl = static_cast<data_t *>(down);
        }
      } else {
        // No appropriate down pointer was found,
        // so start at the leftmost node at the next layer.
        if (layer > 0) {
          down_idx = &(index_head[layer - 1]);
        } else {
          curr_dl = data_head;
        }
      }

      if (layer > 0) {
        curr = down_idx;
      }

      --layer;
    }

    // Check if we have to follow any next pointers.
    check_next_sequential<data_t>(curr_dl, k);

    // Common case: generated height is 0,
    // so simply attempt to insert it into the data layer.
    if (new_height == 0) {
      return curr_dl->insert(pair);
    }

    // Generated height is at least 1, so we need to partition the data node.
    // First we must manually check if the key is present in the data node.
    if (curr_dl->v.contains(k)) {
      // If key is present, just return false.
      return false;
    }

    // Key isn't present, so do the partition.

    // Edge case: curr_dl may be full and the inserted key may be less than
    // its minimum. (This can only happen if it is leftmost.)
    // If this is the case, we must first partition curr_dl.
    if (curr_dl->v.get_size() == curr_dl->v.get_capacity() &&
        k < curr_dl->v.first()) {
      data_t *new_orphan = new data_t(curr_dl, true);
      new_orphan->v.steal_half(&(curr_dl->v));
    }

    data_t *new_data_node = new data_t(curr_dl, false);
    new_data_node->v.insert_and_steal_greater(&(curr_dl->v), pair);

    void *down_ptr = new_data_node;

    // Partition any index layers that need partitioning,
    // and insert down pointers.
    // NB: This loop's range excludes the top layer
    // because we do not partition at the top layer
    for (int i = 0; i + 1 < new_height; ++i) {
      index_t *victim = prev_nodes[i];

      // Same edge case as above, just for index layer nodes
      if (victim->v.get_size() == victim->v.get_capacity() &&
          k < victim->v.first()) {
        index_t *new_index_orphan = new index_t(victim, true);
        new_index_orphan->v.steal_half(&(victim->v));
      }

      index_t *new_index_node = new index_t(victim, false);
      new_index_node->v.insert_and_steal_greater(&(victim->v),
                                                 std::make_pair(k, down_ptr));

      down_ptr = new_index_node;
    }

    // Finally, at the pre-generated height,
    // simply insert the appropriate down pointer into the appropriate vector.
    prev_nodes[new_height - 1]->insert(std::make_pair(k, down_ptr));

    return true;
  }

  /// Remove an element from the map
  bool remove(const K &k) {
    init_context();

    // First, search for the uppermost instance of k in the data structure.
    // Clean up after other lazy removes along the way.

  top:

    // Start at the topmost index layer.
    int layer = layers - 1;
    index_t *curr = &(index_head[layer]);
    uint64_t curr_lock = curr->lock.begin_read();
    HP::take_first(curr);
    data_t *curr_dl = data_head;

    // Skip through index layers.
    while (layer >= 0) {
      // Check next, as it may need to be maintained or followed.
      if (!check_next<index_t>(curr, curr_lock, k, true)) {
        HP::drop_all();
        goto top;
      }

      // Now, search the vector we arrived at for the right down pointer.
      void *down = nullptr;
      index_t *down_idx = nullptr;
      K found_k = k;

      if (curr->v.find_lte(k, found_k, down)) {
        if (found_k == k) {
          // If we find the uppermost instance of k in the skipvector, then lock
          // it and proceed to remove it.

          // NB: Here we must confirm that this is the uppermost instance of k.
          // If curr is not an orphan, and k is the first element in this list,
          // then this is NOT the uppermost instance of k, so start over.
          // Otherwise, it is safe to proceed. This can happen if this remove()
          // call interleaves with an insert() call on the same k.
          if (!sv_lock::is_orphan(curr_lock) && curr->v.first() == k) {
            HP::drop_all();
            goto top;
          }

          if (!curr->lock.try_upgrade(curr_lock)) {
            HP::drop_all();
            goto top;
          }

          // We have a write lock on curr now, so we don't need the hazard
          // pointer anymore.
          HP::drop_curr();
          break;
        }

        // Otherwise, we found an appropriate down pointer, so follow it.
        if (layer > 0) {
          down_idx = static_cast<index_t *>(down);
        } else {
          curr_dl = static_cast<data_t *>(down);
        }
      } else {
        // No appropriate down pointer was found,
        // so start at the leftmost node at the next layer.
        if (layer > 0) {
          down_idx = &(index_head[layer - 1]);
        } else {
          curr_dl = data_head;
        }
      }

      // Exchange curr's lock for down's lock.
      if (layer > 0) {
        if (!reader_swap<index_t>(curr, curr_lock, down_idx)) {
          HP::drop_all();
          goto top;
        }
        curr = down_idx;
      } else {
        if (!reader_swap<data_t>(curr, curr_lock, curr_dl)) {
          HP::drop_all();
          goto top;
        }
      }

      --layer;
    }

    // Normal case: k wasn't found in upper levels, so check data layer.
    if (layer == -1) {
      // Check if we have to follow any next pointers.
      bool check_next_success = check_next<data_t>(curr_dl, curr_lock, k, true);

      // Now, acquire curr_dl as a writer.
      if (!check_next_success || !curr_dl->lock.try_upgrade(curr_lock)) {
        HP::drop_all();
        goto top;
      }

      // Edge case: Same as above; this may not be the uppermost instance of k,
      // if this call interleaves with an insert() call on k.
      // Double-check that it is.
      if (!sv_lock::is_orphan(curr_lock) && curr_dl->v.first() == k) {
        curr_dl->lock.release_unchanged();
        goto top;
      }

      // At this point, simply try to remove from the data node we arrived at.
      bool result = curr_dl->v.remove(k);
      curr_dl->lock.release_changed_if(result);
      HP::drop_all();
      return result;
    }

    // remove() starts being lazy here.
    // We broke out of loop early, so lock all the way down.
    // NB: For each new node we access here, we have its parent locked as a
    // writer, so there is no need to take a hazard pointer on them.

    for (int i = layer; i > 0; --i) {
      void *down_void = nullptr;
      curr->v.remove(k, down_void);
      index_t *down_idx = static_cast<index_t *>(down_void);
      // TODO: See todo just above.
      down_idx->lock.acquire();

      // Release first node normally, subsequent nodes as orphans.
      if (i == layer) {
        curr->lock.release();
      } else {
        curr->lock.release_as_orphan();
      }

      curr = down_idx;
    }

    // And do it once more for the last index layer.
    void *down_void = nullptr;
    curr->v.remove(k, down_void);
    curr_dl = static_cast<data_t *>(down_void);
    // TODO: See todo just above.
    curr_dl->lock.acquire();
    if (layer == 0) {
      curr->lock.release();
    } else {
      curr->lock.release_as_orphan();
    }

    // Finally, remove the element from the data layer.
    curr_dl->v.remove(k);
    curr_dl->lock.release_as_orphan();

    HP::drop_all();
    return true;
  }

  /// Remove an element from the map, put its value into v
  bool remove(const K &k, V &v) {
    init_context();

    // First, search for the uppermost instance of k in the data structure.
    // Clean up after other lazy removes along the way.

  top:

    // Start at the topmost index layer.
    int layer = layers - 1;
    index_t *curr = &(index_head[layer]);
    uint64_t curr_lock = curr->lock.begin_read();
    HP::take_first(curr);
    data_t *curr_dl = data_head;

    // Skip through index layers.
    while (layer >= 0) {
      // Check next, as it may need to be maintained or followed.
      if (!check_next<index_t>(curr, curr_lock, k, true)) {
        HP::drop_all();
        goto top;
      }

      // Now, search the vector we arrived at for the right down pointer.
      void *down = nullptr;
      index_t *down_idx = nullptr;
      K found_k = k;

      if (curr->v.find_lte(k, found_k, down)) {
        if (found_k == k) {
          // If we find the uppermost instance of k in the skipvector, then lock
          // it and proceed to remove it.

          // NB: Here we must confirm that this is the uppermost instance of k.
          // If curr is not an orphan, and k is the first element in this list,
          // then this is NOT the uppermost instance of k, so start over.
          // Otherwise, it is safe to proceed. This can happen if this remove()
          // call interleaves with an insert() call on the same k.
          if (!sv_lock::is_orphan(curr_lock) && curr->v.first() == k) {
            HP::drop_all();
            goto top;
          }

          if (!curr->lock.try_upgrade(curr_lock)) {
            HP::drop_all();
            goto top;
          }

          // We have a write lock on curr now, so we don't need the hazard
          // pointer anymore.
          HP::drop_curr();
          break;
        }

        // Otherwise, we found an appropriate down pointer, so follow it.
        if (layer > 0) {
          down_idx = static_cast<index_t *>(down);
        } else {
          curr_dl = static_cast<data_t *>(down);
        }
      } else {
        // No appropriate down pointer was found,
        // so start at the leftmost node at the next layer.
        if (layer > 0) {
          down_idx = &(index_head[layer - 1]);
        } else {
          curr_dl = data_head;
        }
      }

      // Exchange curr's lock for down's lock.
      if (layer > 0) {
        if (!reader_swap<index_t>(curr, curr_lock, down_idx)) {
          HP::drop_all();
          goto top;
        }
        curr = down_idx;
      } else {
        if (!reader_swap<data_t>(curr, curr_lock, curr_dl)) {
          HP::drop_all();
          goto top;
        }
      }

      --layer;
    }

    // Normal case: k wasn't found in upper levels, so check data layer.
    if (layer == -1) {
      // Check if we have to follow any next pointers.
      bool check_next_success = check_next<data_t>(curr_dl, curr_lock, k, true);

      // Now, acquire curr_dl as a writer.
      if (!check_next_success || !curr_dl->lock.try_upgrade(curr_lock)) {
        HP::drop_all();
        goto top;
      }

      // Edge case: Same as above; this may not be the uppermost instance of k,
      // if this call interleaves with an insert() call on k.
      // Double-check that it is.
      if (!sv_lock::is_orphan(curr_lock) && curr_dl->v.first() == k) {
        curr_dl->lock.release_unchanged();
        goto top;
      }

      // At this point, simply try to remove from the data node we arrived at.
      bool result = curr_dl->v.remove(k, v);
      curr_dl->lock.release_changed_if(result);
      HP::drop_all();
      return result;
    }

    // remove() starts being lazy here.
    // We broke out of loop early, so lock all the way down.
    // NB: For each new node we access here, we have its parent locked as a
    // writer, so there is no need to take a hazard pointer on them.

    for (int i = layer; i > 0; --i) {
      void *down_void = nullptr;
      curr->v.remove(k, down_void);
      index_t *down_idx = static_cast<index_t *>(down_void);
      // TODO: See todo just above.
      down_idx->lock.acquire();

      // Release first node normally, subsequent nodes as orphans.
      if (i == layer) {
        curr->lock.release();
      } else {
        curr->lock.release_as_orphan();
      }

      curr = down_idx;
    }

    // And do it once more for the last index layer.
    void *down_void = nullptr;
    curr->v.remove(k, down_void);
    curr_dl = static_cast<data_t *>(down_void);
    // TODO: See todo just above.
    curr_dl->lock.acquire();
    if (layer == 0) {
      curr->lock.release();
    } else {
      curr->lock.release_as_orphan();
    }

    // Finally, remove the element from the data layer.
    curr_dl->v.remove(k, v);
    curr_dl->lock.release_as_orphan();

    HP::drop_all();
    return true;
  }

  //sequential method of extract min. puts extracted min into k and v args
  bool extract_min(K *k, V *v) {
    data_head->lock.acquire();

    //Plan A: DH holds data so extract from DH
    if(data_head->v.get_size() > 0) {

      bool result = data_head->v.remove_pair_at(data_head->v.get_size()-1, *k, *v); // could also try extracting from index 0
      data_head->lock.release();
      return result;

    } else if(data_head->next != nullptr) {//DH is empty, so see what DH->next is doing

      data_t *next = data_head->next;
      HP::take(next);
      uint64_t next_lock = next->lock.begin_read();
      if (!next->lock.confirm_read(next_lock)) { //see if successfully acquired read lock
        HP::drop_next();
        data_head->lock.release();
        return false;
      }

      if(next->lock.is_orphan()) { //Plan B: DH empty, DH->next is orphan. Make DH->next new head, extract from it

        data_t *old_head = data_head;
        data_head = next; //get rid of old, empty head
        //TO DO: stop leaking empty heads
        old_head->lock.release();
        if(!data_head->lock.try_upgrade(next_lock)) {
          HP::drop_next();
          return false;
        }
        bool result = data_head->v.remove_pair_at(data_head->v.get_size()-1, *k, *v); 
        data_head->lock.release();
        return result;

      } else { //Plan C: DH->next isn't orphan so extract min element to orphanize it

        //std::cout << "about to remove DH next min\n";
        const K key = next->v.first();
        bool result = remove(key, *v);
        HP::drop_all();
        data_head->lock.release();
        return result;
      
      }
      
    } else {
      *k = -1;
      *v = -1;
      data_head->lock.release();
      return true;
    }

    
    
    data_head->lock.release();
    return false;
  }

  /// foreach() applies an elemental function f() to each element in the map.
  /// This implementation is linearizable.
  void foreach (std::function<void(const K &, V &, bool &)> f, bool) {
    data_t *curr = data_head;
    bool exit_flag = false;

    // Acquiring locks and traversing
    while (curr != nullptr && !exit_flag) {
      curr->lock.acquire();
      curr->v.foreach (f, exit_flag);
      curr = curr->next;
    }

    // Lock-releasing phase
    data_t *last = curr;
    curr = data_head;
    while (curr != last) {
      data_t *next = curr->next;
      curr->lock.release();
      curr = next;
    }
  }

  /// Perform a range operation by applying f to the keys between from and to,
  /// inclusive
  void range(const K &from, const K &to,
             std::function<void(const K &, V &, bool &)> f, bool) {
    init_context();

    // Validate input parameters
    if (from > to)
      return;

    data_t *first_locked = skip_to(from);
    data_t *curr = first_locked;
    bool done = false;
    bool exit_flag = false;

    // Process the first node.
    done = curr->v.range(from, to, f, exit_flag);
    curr = curr->next;

    // Process the nodes.
    while (curr != nullptr && !done && !exit_flag) {
      curr->lock.acquire();
      done = curr->v.range(from, to, f, exit_flag);
      curr = curr->next;
    }

    // Lock-releasing phase
    data_t *last = curr;
    curr = first_locked;
    while (curr != last) {
      data_t *next = curr->next;
      curr->lock.release();
      curr = next;
    }
  }

  // Verify internal structure of skipvector. For debug purposes.
  bool verify() {
    // Verify that no hazard pointers are held (not allowed while quiescent.)
    int hps = HP::count_reserved();
    if (hps != 0) {
      std::cout << "Verify failure: " << hps
                << " hazard pointer(s) held at verify time!" << std::endl;
      return false;
    }

    // Verify each index layer against the next layer down.
    for (int layer = layers - 1; layer > 0; --layer) {
      verify_index<index_t>(layer, &(index_head[layer - 1]));
    }

    // Verify the last index layer against the data layer.
    verify_index<data_t>(0, data_head);

    // Verify the data layer.
    data_t *curr = data_head;

    while (curr != nullptr) {
      int curr_size = curr->v.get_size();
      data_t *next = curr->next;
      verify_lock<data_t>(curr);

      // Verify the node's vector, delegating to its own verify function.
      if (!curr->v.verify()) {
        dump();
        return false;
      }

      // Empty orphans are allowed, so skip any empty orphans until a proper
      // next is found.
      while (next != nullptr && next->v.get_size() == 0) {
        // But still check the sequence lock.
        verify_lock<data_t>(next);
        if (!next->is_orphan_seq()) {
          dump();
          std::cout << "Verify failure: empty node not orphan!" << std::endl;
          return false;
        }
        next = next->next;
      }

      // If next exists, make sure its first element is greater than our last
      if (next != nullptr) {
        // If curr is empty, make sure it is an orphan.
        // If it is not empty, then make sure curr and next are properly
        // ordered.
        if (curr_size == 0) {
          if (!curr->is_orphan_seq()) {
            dump();
            std::cout
                << "Verify failure: curr is a empty non-orphan at data layer"
                << std::endl;
          }
        } else if (curr->v.max_key() > next->v.first()) {
          dump();
          std::cout
              << "Verify failure: nodes are improperly ordered in data layer."
              << std::endl;
          return false;
        }
      }

      // Advance to the next node
      curr = next;
    }

    return true;
  }

  // SEQUENTIAL-ONLY size function
  size_t get_size() {
    size_t result = 0;
    data_t *curr = data_head;

    while (curr != nullptr) {
      result += curr->v.get_size();
      curr = curr->next;
    }

    return result;
  }

  void verbose_analysis() {
    std::cout << "Merge threshold: " << merge_threshold << std::endl;

    // Print the number of vectors at each layer to check vertical balance
    std::cout << "Number of nodes at each layer: " << std::endl;

    // Number of elements on previous layer.
    size_t last_elts = 0;

    for (int i = layers - 1; i >= 0; --i) {
      size_t count = 0;
      size_t elements = 0;
      index_t *curr = &(index_head[i]);
      while (curr != nullptr) {
        elements += curr->v.get_size();
        ++count;
        curr = curr->next;
      }
      size_t orphans = count - last_elts;
      std::cout << i << ": " << count << " nodes (" << orphans << " orphans)"
                << std::endl;
      last_elts = elements;
    }

    size_t count = 0;
    size_t elements = 0;
    data_t *curr = data_head;
    while (curr != nullptr) {
      elements += curr->v.get_size();
      ++count;
      curr = curr->next;
    }
    size_t orphans = count - last_elts;
    std::cout << "D: " << count << " nodes (" << orphans << " orphans)"
              << std::endl;
    std::cout << "Elements: " << elements << std::endl;
  }

  // Dump the entire state of the skipvector for debug
  void dump() {
    // Print contents of all index layers
    for (int i = layers - 1; i >= 0; --i) {
      std::cout << "Index " << std::hex << i << ": ";
      index_t *curr = &(index_head[i]);
      while (curr != nullptr) {
        curr->lock.dump();
        curr->v.dump();
        curr = curr->next;
      }
      std::cout << std::endl;
    }

    std::cout << "Data: ";
    data_t *curr = data_head;
    while (curr != nullptr) {
      curr->lock.dump();
      curr->v.dump();
      curr = curr->next;
    }
    std::cout << std::endl;
  }

  // Helper method for verify(). Makes sure the sequence lock is not locked
  // (which is not allowed while quiescent,) and returns a bool indicating if it
  // is an orphan (which is allowed.)
  template <typename T> bool verify_lock(T *curr) {
    uint64_t lock = curr->lock.get_value();
    if (sv_lock::is_locked(lock)) {
      dump();
      std::cout << "node " << curr << " had lock value of " << lock
                << " at verify time!" << std::endl;
      return false;
    }
    return true;
  }

  // Helper method to verify(). Verifies nodes in the index layer.
  // The layer below may consist of index_t nodes or data_t nodes, so this
  // method is templated.
  template <typename T> bool verify_index(int layer, T *trace) {
    index_t *curr = &(index_head[layer]);

    while (curr != nullptr) {
      int curr_size = curr->v.get_size();
      index_t *next = curr->next;
      verify_lock<index_t>(curr);

      // Verify the node's vector, delegating to its own verify function.
      if (!curr->v.verify()) {
        dump();
        return false;
      }

      curr->v.sort();

      // Trace along next layer to verify structural consistency with the down
      // pointers in this layer
      for (int j = 0; j < curr_size; ++j) {
        K key = curr->v.at(j);
        void *down_void = nullptr;
        curr->v.contains(key, down_void);
        T *down = static_cast<T *>(down_void);

        // Advance trace until current down pointer is found
        while (trace != down) {
          if (trace == nullptr) {
            dump();
            std::cout
                << "Verify failure: Trace reached nullptr before finding key "
                << +key << " at index layer " << layer << std::endl;
            return false;
          }

          // If trace doesn't match the down pointer, it must be an orphan
          if (!trace->is_orphan_seq()) {
            dump();
            std::cout
                << "Verify failure: Trace found non-orphan with start key "
                << +trace->v.first() << " while looking for key " << +key
                << " at index layer " << layer << std::endl;
            return false;
          }

          trace = trace->next;
        }

        // Currently, trace == down, so advance it once more in preparation
        // for next loop
        trace = trace->next;

        // Down pointer must not point to an orphan
        if (down->is_orphan_seq()) {
          dump();
          std::cout << "Verify failure: down pointer for key " << +key
                    << " pointing to orphan in layer: " << layer << std::endl;
          return false;
        }

        // Down pointer must not point to empty vector
        if (down->v.get_size() == 0) {
          dump();
          std::cout << "Verify failure: down pointer for key " << +key
                    << " pointing to empty vector in layer: " << layer
                    << std::endl;
          return false;
        }

        // Down pointer's first element must be sought key
        if (key != down->v.first()) {
          dump();
          std::cout << "Verify failure: down pointer at layer " << layer
                    << " with key " << +key
                    << " points to vector with first element "
                    << down->v.first() << std::endl;
          return false;
        }
      }

      // Empty orphans are allowed, so skip any empty orphans until a proper
      // next is found.
      while (next != nullptr && next->v.get_size() == 0) {
        // But still check the sequence lock.
        verify_lock<index_t>(next);
        if (!next->is_orphan_seq()) {
          dump();
          std::cout << "Verify failure: empty node not orphan!" << std::endl;
          return false;
        }
        next = next->next;
      }

      // If next exists, make sure its first element is greater than our last
      if (next != nullptr) {
        // If curr is empty, make sure it is an orphan.
        if (curr_size == 0) {
          if (!curr->is_orphan_seq()) {
            dump();
            std::cout << "Verify failure: curr is a empty non-orphan at "
                      << layer << std::endl;
          }
        } else if (curr->v.max_key() > next->v.first()) {
          // If curr is not empty, then make sure curr and next are properly
          // ordered.
          dump();
          std::cout << "Verify failure: nodes are improperly ordered at layer "
                    << layer << std::endl;
          return false;
        }
      }

      // Advance to the next node
      curr = next;
    }

    // If we get here, the layer verified successfully.
    return true;
  }
};
