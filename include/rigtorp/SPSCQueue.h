// SPDX-FileCopyrightText: 2020 Erik Rigtorp <erik@rigtorp.se>
// SPDX-License-Identifier: MIT

#ifndef RIGTORP_SPSCQUEUE_H
#define RIGTORP_SPSCQUEUE_H

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>      // std::allocator, std::construct_at, std::destroy_at
#include <type_traits> // std::is_*_v
#include <utility>     // std::forward

namespace rigtorp {

template <typename T, typename Allocator = std::allocator<T>> class SPSCQueue {
public:
  explicit SPSCQueue(const size_t capacity,
                     const Allocator &allocator = Allocator())
      : capacity_(capacity), allocator_(allocator) {
    // The queue needs at least one element
    if (capacity_ < 1) {
      capacity_ = 1;
    }
    // Prevent overflowing size_t
    if (capacity_ > SIZE_MAX - 2 * kPadding - 1) {
      capacity_ = SIZE_MAX - 2 * kPadding - 1;
    }
    capacity_++; // Needs one slack element

    if constexpr (requires {
                    allocator_.allocate_at_least(capacity_ + 2 * kPadding);
                  }) {
      auto res = allocator_.allocate_at_least(capacity_ + 2 * kPadding);
      slots_ = res.ptr;
      capacity_ = res.count - 2 * kPadding;
    } else {
      slots_ = std::allocator_traits<Allocator>::allocate(
          allocator_, capacity_ + 2 * kPadding);
    }

    static_assert(alignof(SPSCQueue<T>) == kFalseSharingRange);
    static_assert(sizeof(SPSCQueue<T>) >= 3 * kFalseSharingRange);
    assert(reinterpret_cast<char *>(&readIdx_) -
               reinterpret_cast<char *>(&writeIdx_) >=
           static_cast<std::ptrdiff_t>(kFalseSharingRange));
  }

  ~SPSCQueue() {
    while (front()) {
      pop();
    }
    std::allocator_traits<Allocator>::deallocate(allocator_, slots_,
                                                 capacity_ + 2 * kPadding);
  }

  // non-copyable and non-movable
  SPSCQueue(const SPSCQueue &) = delete;
  SPSCQueue &operator=(const SPSCQueue &) = delete;

  template <typename... Args>
  void emplace(Args &&...args) noexcept(
      std::is_nothrow_constructible_v<T, Args &&...>) {
    static_assert(std::is_constructible_v<T, Args &&...>,
                  "T must be constructible with Args&&...");
    auto const writeIdx = writeIdx_.load(std::memory_order_relaxed);
    auto nextWriteIdx = writeIdx + 1;
    if (nextWriteIdx == capacity_) {
      nextWriteIdx = 0;
    }
    while (nextWriteIdx == readIdxCache_) {
      readIdxCache_ = readIdx_.load(std::memory_order_acquire);
    }
    std::construct_at(&slots_[writeIdx + kPadding],
                      std::forward<Args>(args)...);
    writeIdx_.store(nextWriteIdx, std::memory_order_release);
  }

  template <typename... Args>
  [[nodiscard]] bool try_emplace(Args &&...args) noexcept(
      std::is_nothrow_constructible_v<T, Args &&...>) {
    static_assert(std::is_constructible_v<T, Args &&...>,
                  "T must be constructible with Args&&...");
    auto const writeIdx = writeIdx_.load(std::memory_order_relaxed);
    auto nextWriteIdx = writeIdx + 1;
    if (nextWriteIdx == capacity_) {
      nextWriteIdx = 0;
    }
    if (nextWriteIdx == readIdxCache_) {
      readIdxCache_ = readIdx_.load(std::memory_order_acquire);
      if (nextWriteIdx == readIdxCache_) {
        return false;
      }
    }
    std::construct_at(&slots_[writeIdx + kPadding],
                      std::forward<Args>(args)...);
    writeIdx_.store(nextWriteIdx, std::memory_order_release);
    return true;
  }

  void push(const T &v) noexcept(std::is_nothrow_copy_constructible_v<T>) {
    static_assert(std::is_copy_constructible_v<T>,
                  "T must be copy constructible");
    emplace(v);
  }

  template <typename P>
    requires std::is_constructible_v<T, P &&>
  void push(P &&v) noexcept(std::is_nothrow_constructible_v<T, P &&>) {
    emplace(std::forward<P>(v));
  }

  [[nodiscard]] bool
  try_push(const T &v) noexcept(std::is_nothrow_copy_constructible_v<T>) {
    static_assert(std::is_copy_constructible_v<T>,
                  "T must be copy constructible");
    return try_emplace(v);
  }

  template <typename P>
    requires std::is_constructible_v<T, P &&>
  [[nodiscard]] bool
  try_push(P &&v) noexcept(std::is_nothrow_constructible_v<T, P &&>) {
    return try_emplace(std::forward<P>(v));
  }

  [[nodiscard]] T *front() noexcept {
    auto const readIdx = readIdx_.load(std::memory_order_relaxed);
    if (readIdx == writeIdxCache_) {
      writeIdxCache_ = writeIdx_.load(std::memory_order_acquire);
      if (writeIdxCache_ == readIdx) {
        return nullptr;
      }
    }
    return &slots_[readIdx + kPadding];
  }

  void pop() noexcept {
    static_assert(std::is_nothrow_destructible_v<T>,
                  "T must be nothrow destructible");
    auto const readIdx = readIdx_.load(std::memory_order_relaxed);
    assert(writeIdx_.load(std::memory_order_acquire) != readIdx &&
           "Can only call pop() after front() has returned a non-nullptr");
    std::destroy_at(&slots_[readIdx + kPadding]);
    auto nextReadIdx = readIdx + 1;
    if (nextReadIdx == capacity_) {
      nextReadIdx = 0;
    }
    readIdx_.store(nextReadIdx, std::memory_order_release);
  }

  template <typename Fn>
  [[nodiscard]] bool try_consume(Fn &&fn) noexcept(
      noexcept(std::forward<Fn>(fn)(std::declval<T &>()))) {
    static_assert(std::is_nothrow_destructible_v<T>,
                  "T must be nothrow destructible");
    static_assert(std::is_invocable_v<Fn &&, T &>,
                  "Fn must be invocable with T&");
    auto const readIdx = readIdx_.load(std::memory_order_relaxed);
    if (readIdx == writeIdxCache_) {
      writeIdxCache_ = writeIdx_.load(std::memory_order_acquire);
      if (writeIdxCache_ == readIdx) {
        return false;
      }
    }
    std::forward<Fn>(fn)(slots_[readIdx + kPadding]);
    std::destroy_at(&slots_[readIdx + kPadding]);
    auto nextReadIdx = readIdx + 1;
    if (nextReadIdx == capacity_) {
      nextReadIdx = 0;
    }
    readIdx_.store(nextReadIdx, std::memory_order_release);
    return true;
  }

  [[nodiscard]] bool try_pop(T &v) noexcept(
      std::is_move_assignable_v<T> ? std::is_nothrow_move_assignable_v<T>
                                   : std::is_nothrow_copy_assignable_v<T>) {
    static_assert(std::is_move_assignable_v<T> || std::is_copy_assignable_v<T>,
                  "T must be move or copy assignable");
    return try_consume([&v](T &slot) noexcept(
        std::is_move_assignable_v<T> ? std::is_nothrow_move_assignable_v<T>
                                     : std::is_nothrow_copy_assignable_v<T>) {
      if constexpr (std::is_move_assignable_v<T>) {
        v = std::move(slot);
      } else {
        v = slot;
      }
    });
  }

  [[nodiscard]] size_t size() const noexcept {
    auto const writeIdx = writeIdx_.load(std::memory_order_acquire);
    auto const readIdx = readIdx_.load(std::memory_order_acquire);
    return writeIdx >= readIdx ? writeIdx - readIdx
                               : capacity_ - readIdx + writeIdx;
  }

  [[nodiscard]] bool empty() const noexcept {
    return writeIdx_.load(std::memory_order_acquire) ==
           readIdx_.load(std::memory_order_acquire);
  }

  [[nodiscard]] size_t capacity() const noexcept { return capacity_ - 1; }

private:
#if defined(__s390x__)
  static constexpr size_t kFalseSharingRange = 256;
#elif defined(__x86_64__) || defined(_M_X64) || defined(__aarch64__) ||        \
    defined(_M_ARM64) || defined(__powerpc64__)
  static constexpr size_t kFalseSharingRange = 128;
#else
  static constexpr size_t kFalseSharingRange = 64;
#endif

  // Padding to avoid false sharing between slots_ and adjacent allocations
  static constexpr size_t kPadding = (kFalseSharingRange - 1) / sizeof(T) + 1;

private:
  size_t capacity_;
  T *slots_;
#if defined(__has_cpp_attribute) && __has_cpp_attribute(no_unique_address)
  Allocator allocator_ [[no_unique_address]];
#else
  Allocator allocator_;
#endif

  // Align to cache line size in order to avoid false sharing
  // readIdxCache_ and writeIdxCache_ is used to reduce the amount of cache
  // coherency traffic
  alignas(kFalseSharingRange) std::atomic<size_t> writeIdx_ = {0};
  alignas(kFalseSharingRange) size_t readIdxCache_ = 0;
  alignas(kFalseSharingRange) std::atomic<size_t> readIdx_ = {0};
  alignas(kFalseSharingRange) size_t writeIdxCache_ = 0;
};
} // namespace rigtorp

#endif // RIGTORP_SPSCQUEUE_H
