// SPDX-FileCopyrightText: 2026 Erik Rigtorp <erik@rigtorp.se>
// SPDX-License-Identifier: MIT

#include <cassert>
#include <genmc.h>

// Verify SPSCQueue with NDEBUG enabled so pop() does not inject an extra
// debug-only acquire load on writeIdx_.
#ifndef NDEBUG
#define NDEBUG
#define SPSC_GENMC_DEFINED_NDEBUG
#endif
#include <rigtorp/SPSCQueue.h>
#ifdef SPSC_GENMC_DEFINED_NDEBUG
#undef NDEBUG
#undef SPSC_GENMC_DEFINED_NDEBUG
#endif

#ifndef QUEUE_CAPACITY
#define QUEUE_CAPACITY 1
#endif

#ifndef QUEUE_ITEMS
#define QUEUE_ITEMS (QUEUE_CAPACITY + 2)
#endif

#ifndef QUEUE_COMPLETED_TRANSFERS
#define QUEUE_COMPLETED_TRANSFERS 1
#endif

// Ordinary payload accesses must be ordered by the queue's atomic indices.
// A second word checks publication of the whole object, not just its sequence.
struct Payload {
  int sequence;
  int checksum;

  explicit Payload(int value) noexcept
      : sequence(value), checksum(value ^ 0x55) {}

  ~Payload() noexcept {
    // Destruction writes also exercise synchronization before slot reuse.
    sequence = -1;
    checksum = -1;
  }
};

using Queue = rigtorp::SPSCQueue<Payload>;

struct State {
  Queue queue{QUEUE_CAPACITY};
  int produced = 0;
  int consumed = 0;
};

void *produce(void *argument) {
  auto &state = *static_cast<State *>(argument);
  int next = 0;
  for (int i = 0; i < QUEUE_ITEMS; ++i) {
    bool success = state.queue.try_emplace(next);
#if QUEUE_COMPLETED_TRANSFERS
    // Restrict this scenario to completed transfers, without polling loops.
    // A separate scenario retains failed attempts and partial histories.
    __VERIFIER_assume(success);
#endif
    if (success) {
      ++next;
    }
  }
  state.produced = next;
  return nullptr;
}

void *consume(void *argument) {
  auto &state = *static_cast<State *>(argument);
  int next = 0;
  for (int i = 0; i < QUEUE_ITEMS; ++i) {
    Payload *item = state.queue.front();
#if QUEUE_COMPLETED_TRANSFERS
    __VERIFIER_assume(item != nullptr);
#endif
    if (item) {
      assert(item->sequence == next);
      assert(item->checksum == (next ^ 0x55));
      state.queue.pop();
      ++next;
    }
  }
  state.consumed = next;
  return nullptr;
}

int main() {
  State state;
  auto &queue = state.queue;
  assert(queue.capacity() == QUEUE_CAPACITY);
  assert(queue.front() == nullptr);
  // Start the concurrent phase at the last ring index. capacity + 2 successful
  // transfers then cross two wrap boundaries, with fewer concurrent events.
  for (int i = 0; i < QUEUE_CAPACITY; ++i) {
    queue.emplace(QUEUE_ITEMS + i);
  }
  assert(!queue.try_emplace(0));
  for (int i = 0; i < QUEUE_CAPACITY; ++i) {
    auto item = queue.front();
    assert(item != nullptr);
    assert(item->sequence == QUEUE_ITEMS + i);
    assert(item->checksum == ((QUEUE_ITEMS + i) ^ 0x55));
    queue.pop();
  }
  assert(queue.front() == nullptr);
  auto producer = __VERIFIER_spawn(produce, &state);
  auto consumer = __VERIFIER_spawn(consume, &state);
  __VERIFIER_join(producer);
  __VERIFIER_join(consumer);
  assert(state.consumed <= state.produced);
#if QUEUE_COMPLETED_TRANSFERS
  assert(state.produced == QUEUE_ITEMS);
  assert(state.consumed == QUEUE_ITEMS);
#endif
  // In the attempt scenario, drain items left after the consumer's last
  // attempt.
  for (int i = state.consumed; i < state.produced; ++i) {
    auto item = queue.front();
    assert(item != nullptr);
    assert(item->sequence == i);
    assert(item->checksum == (i ^ 0x55));
    queue.pop();
  }
  assert(queue.front() == nullptr);
  assert(queue.empty());
  assert(queue.size() == 0);
}
