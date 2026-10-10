/*
Copyright (c) 2020 Erik Rigtorp <erik@rigtorp.se>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
 */

#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <memory> // std::allocator
#include <new>    // std::hardware_destructive_interference_size
#include <stdexcept>
#include <type_traits> // std::is_*_constructible_v

#ifdef __has_cpp_attribute
#if __has_cpp_attribute(nodiscard)
#define RIGTORP_NODISCARD [[nodiscard]]
#endif
#endif
#ifndef RIGTORP_NODISCARD
#define RIGTORP_NODISCARD
#endif

// C++20 feature detection macros
#if __cplusplus >= 202002L
#define RIGTORP_HAS_CONCEPTS 1
#define RIGTORP_LIKELY [[likely]]
#define RIGTORP_UNLIKELY [[unlikely]]
#else
#define RIGTORP_HAS_CONCEPTS 0
#define RIGTORP_LIKELY
#define RIGTORP_UNLIKELY
#endif

namespace rigtorp {

#if RIGTORP_HAS_CONCEPTS
// C++20 Concepts for allocator validation
template <typename Alloc>
concept HasAllocateAtLeast = requires(Alloc a, size_t n) {
  { a.allocate_at_least(n) };
};
#endif

template <typename T, typename Allocator = std::allocator<T>> class SPSCQueue {

#if !RIGTORP_HAS_CONCEPTS && defined(__cpp_if_constexpr) && defined(__cpp_lib_void_t)
  template <typename Alloc2, typename = void>
  struct has_allocate_at_least : std::false_type {};

  template <typename Alloc2>
  struct has_allocate_at_least<
      Alloc2, std::void_t<typename Alloc2::value_type,
                          decltype(std::declval<Alloc2 &>().allocate_at_least(
                              size_t{}))>> : std::true_type {};
#endif

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

#if RIGTORP_HAS_CONCEPTS
    if constexpr (HasAllocateAtLeast<Allocator>) {
      auto res = allocator_.allocate_at_least(capacity_ + 2 * kPadding);
      slots_ = res.ptr;
      capacity_ = res.count - 2 * kPadding;
    } else {
      slots_ = std::allocator_traits<Allocator>::allocate(
          allocator_, capacity_ + 2 * kPadding);
    }
#elif defined(__cpp_if_constexpr) && defined(__cpp_lib_void_t)
    if constexpr (has_allocate_at_least<Allocator>::value) {
      auto res = allocator_.allocate_at_least(capacity_ + 2 * kPadding);
      slots_ = res.ptr;
      capacity_ = res.count - 2 * kPadding;
    } else {
      slots_ = std::allocator_traits<Allocator>::allocate(
          allocator_, capacity_ + 2 * kPadding);
    }
#else
    slots_ = std::allocator_traits<Allocator>::allocate(
        allocator_, capacity_ + 2 * kPadding);
#endif

    static_assert(alignof(SPSCQueue<T>) == kCacheLineSize, "");
    static_assert(sizeof(SPSCQueue<T>) >= 3 * kCacheLineSize, "");
    assert(reinterpret_cast<char *>(&readIdx_) -
               reinterpret_cast<char *>(&writeIdx_) >=
           static_cast<std::ptrdiff_t>(kCacheLineSize));
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
    while (nextWriteIdx == readIdxCache_) RIGTORP_UNLIKELY {
      readIdxCache_ = readIdx_.load(std::memory_order_acquire);
    }
    new (&slots_[writeIdx + kPadding]) T(std::forward<Args>(args)...);
    writeIdx_.store(nextWriteIdx, std::memory_order_release);
  }

  template <typename... Args>
  RIGTORP_NODISCARD bool try_emplace(Args &&...args) noexcept(
      std::is_nothrow_constructible_v<T, Args &&...>) {
    static_assert(std::is_constructible_v<T, Args &&...>,
                  "T must be constructible with Args&&...");
    auto const writeIdx = writeIdx_.load(std::memory_order_relaxed);
    auto nextWriteIdx = writeIdx + 1;
    if (nextWriteIdx == capacity_) {
      nextWriteIdx = 0;
    }
    if (nextWriteIdx == readIdxCache_) RIGTORP_UNLIKELY {
      readIdxCache_ = readIdx_.load(std::memory_order_acquire);
      if (nextWriteIdx == readIdxCache_) RIGTORP_UNLIKELY {
        return false;
      }
    }
    new (&slots_[writeIdx + kPadding]) T(std::forward<Args>(args)...);
    writeIdx_.store(nextWriteIdx, std::memory_order_release);
    return true;
  }

  void push(const T &v) noexcept(std::is_nothrow_copy_constructible_v<T>) {
    static_assert(std::is_copy_constructible_v<T>,
                  "T must be copy constructible");
    emplace(v);
  }

#if RIGTORP_HAS_CONCEPTS
  template <typename P>
    requires std::is_constructible_v<T, P &&>
  void push(P &&v) noexcept(std::is_nothrow_constructible_v<T, P &&>) {
    emplace(std::forward<P>(v));
  }
#else
  template <typename P, typename = typename std::enable_if<
                            std::is_constructible_v<T, P &&>>::type>
  void push(P &&v) noexcept(std::is_nothrow_constructible_v<T, P &&>) {
    emplace(std::forward<P>(v));
  }
#endif

  RIGTORP_NODISCARD bool
  try_push(const T &v) noexcept(std::is_nothrow_copy_constructible_v<T>) {
    static_assert(std::is_copy_constructible_v<T>,
                  "T must be copy constructible");
    return try_emplace(v);
  }

#if RIGTORP_HAS_CONCEPTS
  template <typename P>
    requires std::is_constructible_v<T, P &&>
  RIGTORP_NODISCARD bool
  try_push(P &&v) noexcept(std::is_nothrow_constructible_v<T, P &&>) {
    return try_emplace(std::forward<P>(v));
  }
#else
  template <typename P, typename = typename std::enable_if<
                            std::is_constructible_v<T, P &&>>::type>
  RIGTORP_NODISCARD bool
  try_push(P &&v) noexcept(std::is_nothrow_constructible_v<T, P &&>) {
    return try_emplace(std::forward<P>(v));
  }
#endif

  RIGTORP_NODISCARD T *front() noexcept {
    auto const readIdx = readIdx_.load(std::memory_order_relaxed);
    if (readIdx == writeIdxCache_) RIGTORP_UNLIKELY {
      writeIdxCache_ = writeIdx_.load(std::memory_order_acquire);
      if (writeIdxCache_ == readIdx) RIGTORP_UNLIKELY {
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
    slots_[readIdx + kPadding].~T();
    auto nextReadIdx = readIdx + 1;
    if (nextReadIdx == capacity_) {
      nextReadIdx = 0;
    }
    readIdx_.store(nextReadIdx, std::memory_order_release);
  }

  RIGTORP_NODISCARD size_t size() const noexcept {
    std::ptrdiff_t diff = writeIdx_.load(std::memory_order_acquire) -
                          readIdx_.load(std::memory_order_acquire);
    if (diff < 0) {
      diff += capacity_;
    }
    return static_cast<size_t>(diff);
  }

  RIGTORP_NODISCARD bool empty() const noexcept {
    return writeIdx_.load(std::memory_order_acquire) ==
           readIdx_.load(std::memory_order_acquire);
  }

  RIGTORP_NODISCARD size_t capacity() const noexcept { return capacity_ - 1; }

private:
#ifdef __cpp_lib_hardware_interference_size
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winterference-size"
  static constexpr size_t kCacheLineSize =
      std::hardware_destructive_interference_size;
#pragma GCC diagnostic pop
#else
  static constexpr size_t kCacheLineSize = 64;
#endif

  // Padding to avoid false sharing between slots_ and adjacent allocations
  static constexpr size_t kPadding = (kCacheLineSize - 1) / sizeof(T) + 1;

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
  alignas(kCacheLineSize) std::atomic<size_t> writeIdx_ = {0};
  alignas(kCacheLineSize) size_t readIdxCache_ = 0;
  alignas(kCacheLineSize) std::atomic<size_t> readIdx_ = {0};
  alignas(kCacheLineSize) size_t writeIdxCache_ = 0;
};
} // namespace rigtorp
