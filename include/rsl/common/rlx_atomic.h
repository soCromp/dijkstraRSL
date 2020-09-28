#pragma once

#include <atomic>

/// rlx_atomic: Relaxed Atomic
/// An atomic which is always accessed by memory_order_relaxed.
/// Syntactic sugar for primitives accessed by sequence locks.
template <typename T> struct rlx_atomic {
  static constexpr std::memory_order relaxed = std::memory_order_relaxed;

  // The contained atomic T.
  std::atomic<T> t;

  // Constructors as std::atomic
  rlx_atomic() noexcept = default;
  constexpr rlx_atomic(T desired) noexcept : t(desired) {}
  rlx_atomic(const rlx_atomic &) = delete;

  /// Operator = performs an implicit relaxed atomic store.
  T operator=(T desired) noexcept {
    t.store(desired, relaxed);
    return desired;
  }

  rlx_atomic &operator=(const rlx_atomic) noexcept = delete;

  /// An explicit call to .store() also performs a relaxed store.
  void store(T desired) noexcept { t.store(desired, relaxed); }

  /// Operator T performs an implicit relaxed atomic load.
  operator T() const noexcept { return t.load(relaxed); }

  /// An explicit call to .load() also performs a relaxed load.
  T load() const noexcept { return t.load(relaxed); }
};

/// Template specialization for int, offering atomic operators
template <> struct rlx_atomic<int> {
  static constexpr std::memory_order relaxed = std::memory_order_relaxed;

  // The contained atomic int.
  std::atomic<int> t;

  // Constructors as std::atomic
  rlx_atomic() noexcept = default;
  constexpr rlx_atomic(int desired) noexcept : t(desired) {}
  rlx_atomic(const rlx_atomic &) = delete;

  /// Operator = performs an implicit relaxed atomic store.
  int operator=(int desired) noexcept {
    t.store(desired, relaxed);
    return desired;
  }

  rlx_atomic &operator=(const rlx_atomic) noexcept = delete;

  /// An explicit call to .store() also performs a relaxed store.
  void store(int desired) noexcept { t.store(desired, relaxed); }

  /// Operator int performs an implicit relaxed atomic load.
  operator int() const noexcept { return t.load(relaxed); }

  /// An explicit call to .load() also performs a relaxed load.
  int load() const noexcept { return t.load(relaxed); }

  int operator+=(int rhs) noexcept { return t.fetch_add(rhs, relaxed); }
  int operator-=(int rhs) noexcept { return t.fetch_sub(rhs, relaxed); }
  int operator&=(int rhs) noexcept { return t.fetch_and(rhs, relaxed); }
  int operator|=(int rhs) noexcept { return t.fetch_or(rhs, relaxed); }
  int operator^=(int rhs) noexcept { return t.fetch_xor(rhs, relaxed); }
  int operator++() noexcept { return t.fetch_add(1, relaxed) + 1; }
  int operator++(int) noexcept { return t.fetch_add(1, relaxed); }
  int operator--() noexcept { return t.fetch_sub(1, relaxed) - 1; }
  int operator--(int) noexcept { return t.fetch_sub(1, relaxed); }
};

/// Template specialization for pointer, allowing -> operator
template <typename T> struct rlx_atomic<T *> {
  static constexpr std::memory_order relaxed = std::memory_order_relaxed;

  // The contained atomic T*.
  std::atomic<T *> t;

  // Constructors as std::atomic
  rlx_atomic() noexcept = default;
  constexpr rlx_atomic(T *desired) noexcept : t(desired) {}
  rlx_atomic(const rlx_atomic &) = delete;

  /// Operator = performs an implicit relaxed atomic store.
  T *operator=(T *desired) noexcept {
    t.store(desired, relaxed);
    return desired;
  }

  rlx_atomic &operator=(const rlx_atomic) noexcept = delete;

  /// An explicit call to .store() also performs a relaxed store.
  void store(T *desired) noexcept { t.store(desired, relaxed); }

  /// Operator T* performs an implicit relaxed atomic load.
  operator T *() const noexcept { return t.load(relaxed); }

  /// An explicit call to .load() also performs a relaxed load.
  T *load() const noexcept { return t.load(relaxed); }

  /// Overload -> operator to perform implicit relaxed load.
  T *operator->() const { return t.load(relaxed); }
};