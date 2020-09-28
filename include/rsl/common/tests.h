/// tests.h defines any tempalted functions that we use as microbenchmarks on
/// data structures.  Currently there are two tests, for data structures that
/// present a set interface or a map interface

#pragma once

#include <signal.h>
#include <thread>
#include <unistd.h>

#include "config.h"
#include "defs.h"
#include "experiment_manager.h"
#include "thread_context.h"

// Fill the data structure before the test.
template <class map_t>
void fill(map_t &MAP, int thread_id, int nthreads, size_t key_range) {
  if (nthreads == 1) {
    // If thread count is 1, use the fast sequential fill method to speed it up
    // moderately. Fill backwards, so inserts are O(1) instead of O(log n).
    for (size_t i = 0; i < key_range; i += 2) {
      size_t k = key_range - i;
      MAP.insert_sequential({k, k});
    }

    return;
  }

  // If there are multiple threads, we have to use the normal insert method,
  // and do something more complicated to save time and ensure allocations are
  // probabilistically fair across NUMA zones.

  // How much to increment the key by each loop iteration. We populate with even
  // keys only, and other threads are inserting as well, so each loop we
  // increment the inserted key by the twice the number of threads.
  size_t key_step = nthreads * 2;

  // Which "family" of keys we are inserting.
  // Thread 0 inserts 0, key_step, 2 * key_step, etc.
  // Thread 1 inserts 2, key_step + 2, 2* key_step + 2, etc.
  size_t key_family = thread_id * 2;

  // start_offset indicates where to begin inserting. To reduce contention, each
  // thread starts insertion at a different place. Once the end is reached, the
  // thread will wrap around and start filling from the start.

  // First, calculate where to start as a fraction; thread_id / nthreads.
  double start_ratio = thread_id;
  start_ratio /= nthreads;

  // Multiply by key_range, and we have a rough guess for what key to start on.
  double start_offset_double = start_ratio * key_range;

  // We now have to round down to a multiple of key_step. We do this by dividing
  // by key_step, casting to int, and multiplying by key_step.
  start_offset_double /= key_step;

  size_t start_offset = start_offset_double;
  start_offset *= key_step;

  // Finally, add key_family to determine the key to start on.
  start_offset += key_family;

  // Insert keys from the start point to the end.
  for (size_t i = start_offset; i < key_range; i += key_step) {
    MAP.insert({i, i});
  }

  // Now, insert keys from the beginning of the map to the start point.
  for (size_t i = key_family; i < start_offset; i += key_step) {
    MAP.insert({i, i});
  }
}

/// Run tests on data structures that implement an ordered map interface. This
/// requires map_t to have insert, lookup, remove, foreach, and range
/// operations, and to operate on key / value pairs
template <class map_t> void ordered_map_test(config *cfg) {
  using std::cout;
  using std::endl;
  using std::thread;
  using std::vector;
  using namespace std::chrono;

  // Create a global stats object for managing this experiment
  experiment_manager exp;

  // Create a map and initialize it to 50% full
  map_t MAP(cfg);

  // This is the benchmark task for threads doing traversals
  auto traversal_task = [&](int id) {
    // The thread's PRNG and counters are here
    thread_context self(id);

    // Do this thread's part to populate the data structure before the first
    // thread barrier.
    fill<map_t>(MAP, id, cfg->nthreads, cfg->key_range);

    // set up distributions for our PRNG
    using std::uniform_int_distribution;
    uniform_int_distribution<size_t> key_dist(0, cfg->key_range - 1);
    uniform_int_distribution<size_t> early_exit_dist(0, cfg->key_range / 2);
    uniform_int_distribution<int> percent_dist(0, 99);

    // Synchronize threads and get time
    exp.sync_before_launch(id, cfg);

    // Run randomly-chosen operations for a fixed interval
    while (exp.running.load()) {
      // Check if we should be testing foreach() or range()
      int iteration_type = percent_dist(self.mt);

      int sum = 0;
      bool readonly = percent_dist(self.mt) < cfg->readonly_traversal_pctg;
      bool exit_early = percent_dist(self.mt) < cfg->early_exit_pctg;

      if (iteration_type >= cfg->range_pctg) {
        // Perform a sum operation utilizing foreach().
        if (exit_early) {
          // TODO: Since the skipvector is ordered we can get a MUCH more
          // uniform early exit distribution by simply choosing a key
          // uniformly at random and stopping once we reach a key >= it.
          // This is a holdover from the iteration project.
          size_t exit_count = early_exit_dist(self.mt);
          size_t visit_count = 0;
          MAP.foreach (
              [&](size_t, const int &v, bool &exit_flag) {
                ++visit_count;
                sum += v;
                if (visit_count >= exit_count)
                  exit_flag = true;
              },
              readonly);
          self.stats[EARLY_EXIT]++;
        } else
          // Just do a regular end-to-end foreach.
          MAP.foreach ([&](size_t, const int &v, bool &) { sum += v; },
                       readonly);

        self.stats[FOREACH]++;
        self.stats[FOREACH_SUM] += sum;
      } else {
        // Else, we are doing a range query

        size_t start_key = key_dist(self.mt);
        size_t end_key;

        if (cfg->range_dist == 0) {
          // If range_dist is set to the special value zero,
          // then choose start and end uniformly at random,
          // resulting in variable length range operations.
          uniform_int_distribution<size_t> end_dist(start_key, cfg->key_range);
          end_key = end_dist(self.mt);
        } else
          // Otherwise, set an end that gives the desired range length.
          end_key = start_key + cfg->range_dist;

        if (exit_early) {
          // Pick a random point during the range to bail.
          // NB: Unordered maps don't implement range() at all, so, here we
          // can implement early exit in the sensible way without worrying
          // about fair comparisons to unordered maps.
          uniform_int_distribution<size_t> early_exit_range_dist(start_key,
                                                                 end_key);
          size_t key_exit = early_exit_range_dist(self.mt);
          MAP.range(
              start_key, end_key,
              [&](size_t k, const int &v, bool &exit_flag) {
                sum += v;
                if (k >= key_exit)
                  exit_flag = true;
              },
              readonly);
          self.stats[EARLY_EXIT]++;
        } else
          // Just do a normal range operation
          MAP.range(
              start_key, end_key,
              [&sum](size_t, const int &v, bool &) { sum += v; }, readonly);

        self.stats[RANGE]++;
        self.stats[RANGE_SUM] += sum;
      }

      // Increment readonly count (for either type of iteration)
      if (readonly)
        self.stats[READONLY]++;
    }

    // arrive at the last barrier, then get the timer again
    exp.sync_after_launch(id, cfg);

    // merge stats into global
    for (size_t i = 0; i < event_types::COUNT; ++i)
      exp.stats[i].fetch_add(self.stats[i]);
  };

  // This is the benchmark task for threads doing elementals
  auto elemental_task = [&](int id) {
    // The thread's PRNG and counters are here
    thread_context self(id);

    // Do this thread's part to populate the data structure before the first
    // thread barrier.
    fill<map_t>(MAP, id, cfg->nthreads, cfg->key_range);

    // set up distributions for our PRNG
    using std::uniform_int_distribution;
    uniform_int_distribution<size_t> key_dist(0, cfg->key_range - 1);
    uniform_int_distribution<int> percent_dist(0, 99);

    // Synchronize threads and get time
    exp.sync_before_launch(id, cfg);

    // Run randomly-chosen operations for a fixed interval
    while (exp.running.load()) {
      // Do a random mix of lookup/insert/remove
      int action = percent_dist(self.mt);
      size_t key = key_dist(self.mt);

      // Lookup
      if (action < cfg->lookup) {
        int val = 0;
        if (MAP.contains(key, val))
          self.stats[LOOKUP_HIT]++;
        else
          self.stats[LOOKUP_MISS]++;
      }

      // Insert
      else if (action < cfg->lookup + (100 - cfg->lookup) / 3) {
        size_t val = key;
        if (MAP.insert({key, val}))
          self.stats[INSERT_HIT]++;
        else
          self.stats[INSERT_MISS]++;
      }

      //Extract min
      else if (action < cfg->lookup + 2*(100-cfg->lookup) / 3) {
        int val = key;
        if( MAP.extract_min(&key, &val) )
          self.stats[EXMIN_HIT]++;
        else
          self.stats[EXMIN_MISS]++;
      }

      // Remove
      else {
        if (MAP.remove(key))
          self.stats[REMOVE_HIT]++;
        else
          self.stats[REMOVE_MISS]++;
      }
    }

    // arrive at the last barrier, then get the timer again
    exp.sync_after_launch(id, cfg);

    // merge stats into global
    for (size_t i = 0; i < event_types::COUNT; ++i)
      exp.stats[i].fetch_add(self.stats[i]);
  };

  // Launch more worker threads as needed.
  int nthreads = cfg->nthreads;
  vector<thread> threads;
  size_t traversal_threads = 0;

  // If a negative value is set for traversal_pctg, contrive a fraction that
  // will ensure the number of threads doing traversals is exactly the
  // absolute value of that.
  float traversal_pctg = cfg->traversal_pctg;
  if (traversal_pctg < 0) {
    int tthreads = -std::round(traversal_pctg);

    // NB: tthreads / nthreads gives us the proportion as a fraction. We
    // multiply by 100 to convert to percentage. We subtract 50% from the
    // denominator to ensure that when we round up, we rounds up to the
    // desired value. (Otherwise, a small episilon resulting from the integer
    // division may result in an extra thread running traversals.)
    traversal_pctg = ((100.0 * tthreads) - 50.0) / (nthreads);
  }

  // NB: This setup thread becomes thread ID 0,
  // so start creating worker threads at thread ID 1.
  for (int i = 1; i < nthreads; ++i) {
    // If adding this thread as an elemental will cause the actual traversal
    // thread ratio to fall below the user's desired ratio, add it as a
    // traversal thread.
    // NB: We do this in this somewhat complicated manner to ensure an even
    // spread of traversal and elemental threads across chips and cores.
    if ((100.0 * traversal_threads) / i < traversal_pctg) {
      threads.emplace_back(traversal_task, i);
      ++traversal_threads;
    } else
      threads.emplace_back(elemental_task, i);
  }

  // Finally, put this thread to work (with ID 0).
  if ((100.0 * traversal_threads) / nthreads < traversal_pctg) {
    ++traversal_threads;
    traversal_task(0);
  } else
    elemental_task(0);

  // Once the this thread is done working, join the other threads.
  for (int i = 0; i < nthreads - 1; ++i)
    threads[i].join();

  if (!MAP.verify()) {
    std::cout << "Warning: map failed final self-validation!" << std::endl;
    MAP.dump();
  }

  if (cfg->verbose)
    MAP.verbose_analysis();

  // Report statistics from the experiment
  exp.report(cfg);
}
