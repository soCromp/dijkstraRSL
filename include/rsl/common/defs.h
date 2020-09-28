/// defs.h defines any simple types or constants used by other code in the
/// common/ folder

#pragma once

/// We count a number of events, and we do so by incrementing integers in
/// arrays.  This enum maps the events to integers that can be used as array
/// indices
enum event_types {
  LOOKUP_HIT = 0,
  LOOKUP_MISS = 1,
  INSERT_HIT = 2,
  INSERT_MISS = 3,
  EXMIN_HIT = 4,
  EXMIN_MISS = 5,
  REMOVE_HIT = 6,
  REMOVE_MISS = 7,
  FOREACH = 8,
  FOREACH_SUM = 9,
  RANGE = 10,
  RANGE_SUM = 11,
  EARLY_EXIT = 12,
  READONLY = 13,
  COUNT = 14
};

/// A large prime.  We use this to help seed the PRNG for each thread.  Mersenne
/// Twister has an issue where similar seeds lead to similar sequences, so
/// this value helps mitigate that.
const uint64_t LARGE_PRIME = 2654435761;
