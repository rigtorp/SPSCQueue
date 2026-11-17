// SPDX-FileCopyrightText: 2020 Erik Rigtorp <erik@rigtorp.se>
// SPDX-License-Identifier: MIT

#ifndef RIGTORP_SPSCQUEUE_H
#define RIGTORP_SPSCQUEUE_H

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>  // std::invoke
#include <memory>      // std::allocator, std::construct_at, std::destroy_at
#include <type_traits> // std::is_*_v
#include <utility>     // std::forward

// Force-inline hot producer/consumer paths so compilers do not outline them
// when a translation unit hits per-TU inlining limits across many call sites.
#if defined(__GNUC__) || defined(__clang__)
#define RIGTORP_SPSCQUEUE_INLINE [[gnu::always_inline]] inline
#elif defined(_MSC_VER)
#define RIGTORP_SPSCQUEUE_INLINE __forceinline
#else
#define RIGTORP_SPSCQUEUE_INLINE inline
#endif

namespace rigtorp {

/// @brief Fixed-capacity queue for one producer and one consumer.
///
/// Nonblocking operations use bounded atomic loads and stores without retry
/// loops. Their queue synchronization is wait-free (and thus lock-free) when
/// those atomic operations are wait-free; lock-free std::atomic<size_t> is
/// necessary but alone does not establish that guarantee. Allocation and user
/// element operations are excluded. push() and emplace() busy-wait while full
/// and are not wait-free.
///
/// Enqueue operations belong to the producer; front(), pop(), try_pop(), and
/// try_consume() belong to the consumer. These two roles may run concurrently.
/// Transferring a role between threads requires external synchronization.
/// Construction must finish before concurrent access; destruction requires all
/// other access to have stopped. The queue is neither copyable nor movable.
///
/// @tparam T Element type; must be nothrow destructible.
/// @tparam Allocator Controls buffer allocation and deallocation only. Element
/// construction/destruction bypass allocator hooks and do not automatically
/// propagate this allocator into elements.
///
/// User constructors, assignment operators, destructors, and callbacks execute
/// on the calling thread and may themselves block.
template <typename T, typename Allocator = std::allocator<T>>
  requires std::is_nothrow_destructible_v<T>
class SPSCQueue {
public:
  /// @brief Allocate the queue's storage.
  /// @param capacity Requested capacity; zero becomes one. Excessive values are
  /// clamped to leave room for padding and a slack slot.
  /// @param allocator Allocator to copy and use for storage management.
  /// @note allocate_at_least may increase capacity; query capacity() for the
  /// actual value. Allocation and allocator-copy exceptions propagate.
  explicit SPSCQueue(const size_t capacity,
                     const Allocator &allocator = Allocator())
      : writer_{0, 0, nullptr, capacity, allocator} {
    // The queue needs at least one element
    if (writer_.capacity < 1) {
      writer_.capacity = 1;
    }
    // Prevent overflowing size_t
    if (writer_.capacity > SIZE_MAX - 2 * kPadding - 1) {
      writer_.capacity = SIZE_MAX - 2 * kPadding - 1;
    }
    writer_.capacity++; // Needs one slack element

    if constexpr (requires {
                    writer_.allocator.allocate_at_least(writer_.capacity +
                                                           2 * kPadding);
                  }) {
      auto res = writer_.allocator.allocate_at_least(writer_.capacity +
                                                        2 * kPadding);
      writer_.slots = res.ptr;
      writer_.capacity = res.count - 2 * kPadding;
    } else {
      writer_.slots = std::allocator_traits<Allocator>::allocate(
          writer_.allocator, writer_.capacity + 2 * kPadding);
    }

    reader_.slots = writer_.slots;
    reader_.capacity = writer_.capacity;

    static_assert(alignof(SPSCQueue<T>) == kFalseSharingRange);
    static_assert(sizeof(SPSCQueue<T>) >= 3 * kFalseSharingRange);
    assert(reinterpret_cast<char *>(&readIdx_) -
               reinterpret_cast<char *>(&writeIdx_) >=
           static_cast<std::ptrdiff_t>(kFalseSharingRange));
  }

  /// @brief Destroy remaining elements and release storage.
  /// @pre All other access to the queue has stopped.
  ~SPSCQueue() {
    while (front()) {
      pop();
    }
    std::allocator_traits<Allocator>::deallocate(
        writer_.allocator, writer_.slots,
        writer_.capacity + 2 * kPadding);
  }

  SPSCQueue(const SPSCQueue &) = delete;
  SPSCQueue &operator=(const SPSCQueue &) = delete;

  /// @brief Producer: construct an item in place, busy-waiting while full.
  /// @note May wait indefinitely without consumer progress; does not sleep.
  /// T must be constructible from the forwarded arguments. If construction
  /// throws, no item is published, but arguments may have been modified.
  template <typename... Args>
    requires std::is_constructible_v<T, Args &&...>
  RIGTORP_SPSCQUEUE_INLINE void emplace(Args &&...args) noexcept(
      std::is_nothrow_constructible_v<T, Args &&...>) {
    auto const writeIdx = writer_.writeIdx;
    auto nextWriteIdx = writeIdx + 1;
    // Keep the common straight-line path hot; wrap-around and cache-miss
    // reloads are rare in steady-state streaming.
    if (nextWriteIdx == writer_.capacity) [[unlikely]] {
      nextWriteIdx = 0;
    }
    while (nextWriteIdx == writer_.readIdxCache) [[unlikely]] {
      writer_.readIdxCache = readIdx_.load(std::memory_order_acquire);
    }
    std::construct_at(&writer_.slots[writeIdx + kPadding],
                      std::forward<Args>(args)...);
    writer_.writeIdx = nextWriteIdx;
    writeIdx_.store(nextWriteIdx, std::memory_order_release);
  }

  /// @brief Producer: attempt in-place construction without waiting for space.
  /// @return true after insertion; false if full, without constructing an item.
  /// @note T must be constructible from the forwarded arguments. If
  /// construction throws, no item is published, but arguments may have been
  /// modified.
  template <typename... Args>
    requires std::is_constructible_v<T, Args &&...>
  [[nodiscard]] RIGTORP_SPSCQUEUE_INLINE bool
  try_emplace(Args &&...args) noexcept(
      std::is_nothrow_constructible_v<T, Args &&...>) {
    auto const writeIdx = writer_.writeIdx;
    auto nextWriteIdx = writeIdx + 1;
    // Keep the common straight-line path hot; wrap-around and cache-miss
    // reloads are rare in steady-state streaming.
    if (nextWriteIdx == writer_.capacity) [[unlikely]] {
      nextWriteIdx = 0;
    }
    if (nextWriteIdx == writer_.readIdxCache) [[unlikely]] {
      writer_.readIdxCache = readIdx_.load(std::memory_order_acquire);
      if (nextWriteIdx == writer_.readIdxCache) {
        return false;
      }
    }
    std::construct_at(&writer_.slots[writeIdx + kPadding],
                      std::forward<Args>(args)...);
    writer_.writeIdx = nextWriteIdx;
    writeIdx_.store(nextWriteIdx, std::memory_order_release);
    return true;
  }

  /// @brief Producer: copy an item, busy-waiting while full.
  /// @note Requires copy construction; shares emplace()'s waiting and exception
  /// behavior.
  RIGTORP_SPSCQUEUE_INLINE void
  push(const T &v) noexcept(std::is_nothrow_copy_constructible_v<T>)
    requires std::is_copy_constructible_v<T>
  {
    emplace(v);
  }

  /// @brief Producer: construct from a forwarded value, busy-waiting while
  /// full.
  /// @note T must be constructible from P&&; shares emplace()'s waiting and
  /// exception behavior.
  template <typename P>
    requires std::is_constructible_v<T, P &&>
  RIGTORP_SPSCQUEUE_INLINE void
  push(P &&v) noexcept(std::is_nothrow_constructible_v<T, P &&>) {
    emplace(std::forward<P>(v));
  }

  /// @brief Producer: attempt to copy an item without waiting for space.
  /// @return true after insertion; false if full, without copying the argument.
  /// @note Requires copy construction; shares try_emplace()'s exception
  /// behavior.
  [[nodiscard]] RIGTORP_SPSCQUEUE_INLINE bool
  try_push(const T &v) noexcept(std::is_nothrow_copy_constructible_v<T>)
    requires std::is_copy_constructible_v<T>
  {
    return try_emplace(v);
  }

  /// @brief Producer: attempt construction from a forwarded value without
  /// waiting.
  /// @return true after insertion; false if full, without copying or moving
  /// from the argument.
  /// @note T must be constructible from P&&; shares try_emplace()'s exception
  /// behavior.
  template <typename P>
    requires std::is_constructible_v<T, P &&>
  [[nodiscard]] RIGTORP_SPSCQUEUE_INLINE bool
  try_push(P &&v) noexcept(std::is_nothrow_constructible_v<T, P &&>) {
    return try_emplace(std::forward<P>(v));
  }

  /// @brief Consumer: borrow the front item without removing it.
  /// @return Pointer to the front item, or nullptr if empty.
  /// @note The pointer remains valid until that item is consumed or the queue
  /// is destroyed. Producer activity does not invalidate it.
  [[nodiscard]] RIGTORP_SPSCQUEUE_INLINE T *front() noexcept {
    auto const readIdx = reader_.readIdx;
    if (readIdx == reader_.writeIdxCache) [[unlikely]] {
      reader_.writeIdxCache = writeIdx_.load(std::memory_order_acquire);
      if (reader_.writeIdxCache == readIdx) {
        return nullptr;
      }
    }
    return &reader_.slots[readIdx + kPadding];
  }

  /// @brief Consumer: destroy and remove the front item.
  /// @pre front() returned a non-null pointer and that item has not since been
  /// consumed. Calling pop() on an empty queue violates this precondition.
  RIGTORP_SPSCQUEUE_INLINE void pop() noexcept {
    auto const readIdx = reader_.readIdx;
    // A successful front() has already acquired and cached the write index.
    assert(reader_.writeIdxCache != readIdx &&
           "Can only call pop() after front() has returned a non-nullptr");
    std::destroy_at(&reader_.slots[readIdx + kPadding]);
    auto nextReadIdx = readIdx + 1;
    if (nextReadIdx == reader_.capacity) [[unlikely]] {
      nextReadIdx = 0;
    }
    reader_.readIdx = nextReadIdx;
    readIdx_.store(nextReadIdx, std::memory_order_release);
  }

  /// @brief Consumer: process the front item in place, then destroy and remove
  /// it.
  /// @return true after consumption; false if empty, without invoking fn.
  /// @param fn Callable invoked through std::invoke with T&; its result is
  /// ignored.
  /// @pre fn must not recursively consume from this queue or destroy it, or
  /// retain the element reference for use after successful consumption.
  /// @note If invocation throws, the item remains queued but may be modified;
  /// callback side effects are not rolled back. Supports immovable elements and
  /// is noexcept when invocation is nothrow.
  template <typename Fn>
    requires std::is_invocable_v<Fn &&, T &>
  [[nodiscard]] RIGTORP_SPSCQUEUE_INLINE bool
  try_consume(Fn &&fn) noexcept(std::is_nothrow_invocable_v<Fn &&, T &>) {
    auto const readIdx = reader_.readIdx;
    if (readIdx == reader_.writeIdxCache) [[unlikely]] {
      reader_.writeIdxCache = writeIdx_.load(std::memory_order_acquire);
      if (reader_.writeIdxCache == readIdx) {
        return false;
      }
    }
    std::invoke(std::forward<Fn>(fn), reader_.slots[readIdx + kPadding]);
    std::destroy_at(&reader_.slots[readIdx + kPadding]);
    auto nextReadIdx = readIdx + 1;
    if (nextReadIdx == reader_.capacity) [[unlikely]] {
      nextReadIdx = 0;
    }
    reader_.readIdx = nextReadIdx;
    readIdx_.store(nextReadIdx, std::memory_order_release);
    return true;
  }

  /// @brief Consumer: assign the front item to v, then destroy and remove it.
  /// @param v Existing output object; unchanged if the queue is empty.
  /// @return true after consumption; false if empty.
  /// @note Uses assignment from an rvalue when T is move assignable, otherwise
  /// copy assignment. At least one must be supported. If assignment throws, the
  /// item remains queued, but both it and v may be modified. This operation is
  /// noexcept when the selected assignment is nothrow.
  [[nodiscard]] RIGTORP_SPSCQUEUE_INLINE bool
  try_pop(T &v) noexcept(std::is_move_assignable_v<T>
                             ? std::is_nothrow_move_assignable_v<T>
                             : std::is_nothrow_copy_assignable_v<T>)
    requires(std::is_move_assignable_v<T> || std::is_copy_assignable_v<T>)
  {
    return try_consume(
        [&v](T &slot) noexcept(std::is_move_assignable_v<T>
                                   ? std::is_nothrow_move_assignable_v<T>
                                   : std::is_nothrow_copy_assignable_v<T>) {
          if constexpr (std::is_move_assignable_v<T>) {
            v = std::move(slot);
          } else {
            v = slot;
          }
        });
  }

  /// @brief Observe the number of queued items from any thread.
  /// @note Independently sampled indices are not a consistent concurrent
  /// snapshot. The result may be stale; it does not reserve items or space.
  [[nodiscard]] size_t size() const noexcept {
    auto const writeIdx = writeIdx_.load(std::memory_order_acquire);
    auto const readIdx = readIdx_.load(std::memory_order_acquire);
    return writeIdx >= readIdx ? writeIdx - readIdx
                               : writer_.capacity - readIdx + writeIdx;
  }

  /// @brief Observe whether the queue is empty from any thread.
  /// @note Independently sampled indices are not a consistent concurrent
  /// snapshot. The result may be stale; use front() before pop(), or a try_
  /// operation to check and consume an item together.
  [[nodiscard]] bool empty() const noexcept {
    return writeIdx_.load(std::memory_order_acquire) ==
           readIdx_.load(std::memory_order_acquire);
  }

  /// @brief Return the actual item capacity, excluding padding and the slack
  /// slot.
  /// @note Fixed after construction; may be read by any thread while alive.
  [[nodiscard]] size_t capacity() const noexcept {
    return writer_.capacity - 1;
  }

private:
#if defined(__s390x__)
  static constexpr size_t kFalseSharingRange = 256;
#elif defined(__x86_64__) || defined(_M_X64) || defined(__aarch64__) ||        \
    defined(_M_ARM64) || defined(__powerpc64__)
  static constexpr size_t kFalseSharingRange = 128;
#else
  static constexpr size_t kFalseSharingRange = 64;
#endif

  // Aligned elements already put both allocation boundaries on separate
  // false-sharing ranges. Otherwise pad both ends against adjacent allocations.
  static constexpr size_t kPadding =
      alignof(T) >= kFalseSharingRange
          ? 0
          : (kFalseSharingRange - 1) / sizeof(T) + 1;

private:
  // Isolate the shared atomic indices (writeIdx_, readIdx_) on their own
  // kFalseSharingRange-aligned lines so peer polling / spatial prefetching
  // cannot drag the owning thread's private line into Shared state.
  alignas(kFalseSharingRange) std::atomic<size_t> writeIdx_ = {0};
  struct alignas(kFalseSharingRange) {
    // Private copy of writeIdx avoids loading writeIdx_ on enqueue after a
    // spinning reader has pulled writeIdx_ into Shared state (reduces RTT).
    size_t writeIdx = 0;
    // Cached peer read index reduces cross-core cache coherency traffic on
    // readIdx_ until the queue appears full.
    size_t readIdxCache = 0;
    // Local copies of slots and capacity ride on the same private cache line
    // as the index cache to avoid extra cold-cache fetches of a shared header.
    T *slots = nullptr;
    size_t capacity = 0;
#if defined(__has_cpp_attribute) && __has_cpp_attribute(no_unique_address)
    Allocator allocator [[no_unique_address]];
#else
    Allocator allocator;
#endif
  } writer_;
  alignas(kFalseSharingRange) std::atomic<size_t> readIdx_ = {0};
  struct alignas(kFalseSharingRange) {
    // Private copy of readIdx avoids loading readIdx_ on the reader fast path
    // (shaving ~2 ns off RTT), at the cost of an extra store per pop (~4%
    // lower peak throughput).
    size_t readIdx = 0;
    // Cached peer write index reduces cross-core cache coherency traffic on
    // writeIdx_ until the queue appears empty.
    size_t writeIdxCache = 0;
    T *slots = nullptr;
    size_t capacity = 0;
  } reader_;
};
} // namespace rigtorp

#undef RIGTORP_SPSCQUEUE_INLINE

#endif // RIGTORP_SPSCQUEUE_H
