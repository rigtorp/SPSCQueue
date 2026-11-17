// SPDX-FileCopyrightText: 2018 Erik Rigtorp <erik@rigtorp.se>
// SPDX-License-Identifier: MIT

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <rigtorp/SPSCQueue.h>
#include <set>
#include <stdexcept>
#include <thread>
#include <type_traits>

#if (defined(__unix__) || defined(__APPLE__)) && !defined(NDEBUG)
#include <csignal>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#define CHECK(expr)                                                            \
  do {                                                                         \
    if (!(expr)) {                                                             \
      std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #expr     \
                << std::endl;                                                  \
      std::abort();                                                            \
    }                                                                          \
  } while (false)

// TestType tracks correct usage of constructors and destructors
struct TestType {
  static std::set<const TestType *> constructed;
  TestType() noexcept {
    CHECK(constructed.count(this) == 0);
    constructed.insert(this);
  };
  TestType(const TestType &other) noexcept {
    CHECK(constructed.count(this) == 0);
    CHECK(constructed.count(&other) == 1);
    constructed.insert(this);
  };
  TestType(TestType &&other) noexcept {
    CHECK(constructed.count(this) == 0);
    CHECK(constructed.count(&other) == 1);
    constructed.insert(this);
  };
  TestType &operator=(const TestType &other) noexcept {
    CHECK(constructed.count(this) == 1);
    CHECK(constructed.count(&other) == 1);
    return *this;
  };
  TestType &operator=(TestType &&other) noexcept {
    CHECK(constructed.count(this) == 1);
    CHECK(constructed.count(&other) == 1);
    return *this;
  }
  ~TestType() noexcept {
    CHECK(constructed.count(this) == 1);
    constructed.erase(this);
  };
};

std::set<const TestType *> TestType::constructed;

template <typename T> struct TrackingAllocator {
  using value_type = T;

  struct State {
    size_t allocations = 0;
    size_t deallocations = 0;
    T *lastPtr = nullptr;
    size_t lastCount = 0;
  };

  State *state;

  explicit TrackingAllocator(State *s) noexcept : state(s) {}

  template <typename U>
  TrackingAllocator(const TrackingAllocator<U> &other) noexcept
      : state(other.state) {}

  T *allocate(size_t n) {
    ++state->allocations;
    state->lastCount = n;
    state->lastPtr = static_cast<T *>(::operator new(n * sizeof(T)));
    return state->lastPtr;
  }

  void deallocate(T *p, size_t n) noexcept {
    CHECK(p == state->lastPtr);
    CHECK(n == state->lastCount);
    ++state->deallocations;
    ::operator delete(p);
  }
};

template <typename T> struct OversizeAllocator {
  using value_type = T;

  struct AllocationResult {
    T *ptr;
    size_t count;
  };

  struct State {
    size_t extra = 0;
    size_t requested = 0;
    size_t allocated = 0;
    size_t deallocated = 0;
  };

  State *state;

  explicit OversizeAllocator(State *s) noexcept : state(s) {}

  AllocationResult allocate_at_least(size_t n) {
    state->requested = n;
    state->allocated = n + state->extra;
    return {static_cast<T *>(::operator new(state->allocated * sizeof(T))),
            state->allocated};
  }

  void deallocate(T *p, size_t n) noexcept {
    state->deallocated = n;
    ::operator delete(p);
  }
};

static void testFunctionality() {
  using namespace rigtorp;

  {
    SPSCQueue<TestType> q(10);
    CHECK(q.front() == nullptr);
    CHECK(q.size() == 0);
    CHECK(q.empty() == true);
    CHECK(q.capacity() == 10);
    for (int i = 0; i < 10; i++) {
      q.emplace();
    }
    CHECK(q.front() != nullptr);
    CHECK(q.size() == 10);
    CHECK(q.empty() == false);
    CHECK(TestType::constructed.size() == 10);
    CHECK(q.try_emplace() == false);
    q.pop();
    CHECK(q.size() == 9);
    CHECK(TestType::constructed.size() == 9);
    q.pop();
    CHECK(q.size() == 8);
    CHECK(q.try_emplace() == true);
    // writeIdx_ has now wrapped past readIdx_
    CHECK(q.size() == 9);
    CHECK(q.empty() == false);
    CHECK(TestType::constructed.size() == 9);
    CHECK(q.try_emplace() == true);
    CHECK(q.size() == 10);
    CHECK(q.try_emplace() == false);
    CHECK(TestType::constructed.size() == 10);
    q.pop();
    CHECK(q.size() == 9);
    // Destroy non-empty wrapped-around queue
  }
  CHECK(TestType::constructed.size() == 0);

  // Minimal capacity == 1 across multiple wraparounds
  {
    SPSCQueue<int> q(1);
    CHECK(q.capacity() == 1);
    for (int i = 0; i < 5; ++i) {
      CHECK(q.empty());
      CHECK(q.size() == 0);
      CHECK(q.front() == nullptr);
      CHECK(q.try_push(i));
      CHECK(!q.empty());
      CHECK(q.size() == 1);
      CHECK(!q.try_push(i + 1));
      CHECK(q.front() != nullptr);
      CHECK(*q.front() == i);
      q.pop();
    }
    CHECK(q.empty());
    CHECK(q.size() == 0);
  }
}

static void testTypeCategories() {
  using namespace rigtorp;

  // Copyable only type
  {
    struct Test {
      Test() {}
      Test(const Test &) {}
      Test(Test &&) = delete;
    };
    SPSCQueue<Test> q(16);
    // lvalue
    Test v;
    q.emplace(v);
    (void)q.try_emplace(v);
    q.push(v);
    (void)q.try_push(v);
    static_assert(noexcept(q.emplace(v)) == false, "");
    static_assert(noexcept(q.try_emplace(v)) == false, "");
    static_assert(noexcept(q.push(v)) == false, "");
    static_assert(noexcept(q.try_push(v)) == false, "");
    // xvalue
    q.push(Test());
    (void)q.try_push(Test());
    static_assert(noexcept(q.push(Test())) == false, "");
    static_assert(noexcept(q.try_push(Test())) == false, "");
  }

  // Copyable only type (noexcept)
  {
    struct Test {
      Test() noexcept {}
      Test(const Test &) noexcept {}
      Test(Test &&) = delete;
    };
    SPSCQueue<Test> q(16);
    // lvalue
    Test v;
    q.emplace(v);
    (void)q.try_emplace(v);
    q.push(v);
    (void)q.try_push(v);
    static_assert(noexcept(q.emplace(v)) == true, "");
    static_assert(noexcept(q.try_emplace(v)) == true, "");
    static_assert(noexcept(q.push(v)) == true, "");
    static_assert(noexcept(q.try_push(v)) == true, "");
    // xvalue
    q.push(Test());
    (void)q.try_push(Test());
    static_assert(noexcept(q.push(Test())) == true, "");
    static_assert(noexcept(q.try_push(Test())) == true, "");
  }

  // Movable only type
  {
    SPSCQueue<std::unique_ptr<int>> q(16);
    // xvalue
    q.emplace(std::unique_ptr<int>(new int(1)));
    (void)q.try_emplace(std::unique_ptr<int>(new int(1)));
    q.push(std::unique_ptr<int>(new int(1)));
    (void)q.try_push(std::unique_ptr<int>(new int(1)));
    auto v = std::unique_ptr<int>(new int(1));
    static_assert(noexcept(q.emplace(std::move(v))) == true, "");
    static_assert(noexcept(q.try_emplace(std::move(v))) == true, "");
    static_assert(noexcept(q.push(std::move(v))) == true, "");
    static_assert(noexcept(q.try_push(std::move(v))) == true, "");
  }

  // Non-default-constructible, non-copyable, non-movable type with multi-arg
  // emplace
  {
    struct Immovable {
      int a;
      int b;
      Immovable(int x, int y) noexcept : a(x), b(y) {}
      Immovable(const Immovable &) = delete;
      Immovable(Immovable &&) = delete;
    };
    SPSCQueue<Immovable> q(4);
    q.emplace(1, 2);
    CHECK(q.try_emplace(3, 4));
    CHECK(q.front()->a == 1 && q.front()->b == 2);
    q.pop();
    CHECK(q.front()->a == 3 && q.front()->b == 4);
    q.pop();
    CHECK(q.empty());
  }

  // Small (sizeof == 1) and large (sizeof > kFalseSharingRange) element types
  {
    SPSCQueue<uint8_t> smallQ(3);
    CHECK(smallQ.try_push(uint8_t{42}));
    CHECK(*smallQ.front() == 42);
    smallQ.pop();

    struct Large {
      char buf[512];
      explicit Large(char c) noexcept {
        buf[0] = c;
        buf[511] = c;
      }
    };
    SPSCQueue<Large> largeQ(2);
    largeQ.emplace('z');
    CHECK(largeQ.front()->buf[0] == 'z');
    CHECK(largeQ.front()->buf[511] == 'z');
    largeQ.pop();
  }
}

static void testExceptionSafety() {
  using namespace rigtorp;

  struct TrackedThrow {
    int *liveCount;
    int value;
    TrackedThrow(int *live, int v, bool shouldThrow)
        : liveCount(live), value(v) {
      if (shouldThrow) {
        throw std::runtime_error("boom");
      }
      ++(*liveCount);
    }
    ~TrackedThrow() noexcept { --(*liveCount); }
  };

  int live = 0;
  {
    SPSCQueue<TrackedThrow> q(3);
    q.emplace(&live, 10, false);
    CHECK(live == 1);
    CHECK(q.size() == 1);

    bool threw = false;
    try {
      q.emplace(&live, 20, true);
    } catch (const std::runtime_error &) {
      threw = true;
    }
    CHECK(threw);
    CHECK(live == 1);
    CHECK(q.size() == 1);

    threw = false;
    try {
      (void)q.try_emplace(&live, 30, true);
    } catch (const std::runtime_error &) {
      threw = true;
    }
    CHECK(threw);
    CHECK(live == 1);
    CHECK(q.size() == 1);

    q.emplace(&live, 40, false);
    CHECK(live == 2);
    CHECK(q.size() == 2);
    CHECK(q.front()->value == 10);
    q.pop();
    CHECK(q.front()->value == 40);
    q.pop();
    CHECK(q.empty());
  }
  CHECK(live == 0);
}

static void testCustomAllocator() {
  using namespace rigtorp;

  TrackingAllocator<int>::State state;
  {
    SPSCQueue<int, TrackingAllocator<int>> q(5, TrackingAllocator<int>(&state));
    CHECK(state.allocations == 1);
    CHECK(state.deallocations == 0);
    CHECK(q.capacity() == 5);
    for (int i = 0; i < 5; ++i) {
      CHECK(q.try_push(i));
    }
    CHECK(!q.try_push(99));
    for (int i = 0; i < 5; ++i) {
      CHECK(*q.front() == i);
      q.pop();
    }
  }
  CHECK(state.deallocations == 1);

  OversizeAllocator<int>::State oversizeState;
  oversizeState.extra = 3;
  {
    SPSCQueue<int, OversizeAllocator<int>> q(
        4, OversizeAllocator<int>(&oversizeState));
    CHECK(q.capacity() == 7);
    for (int i = 0; i < 7; ++i) {
      CHECK(q.try_push(i));
    }
    CHECK(!q.try_push(100));
    for (int i = 0; i < 7; ++i) {
      CHECK(*q.front() == i);
      q.pop();
    }
  }
  CHECK(oversizeState.deallocated == oversizeState.allocated);
}

static void testCapacityBounds() {
  using namespace rigtorp;

  // capacity < 1
  {
    SPSCQueue<int> q(0);
    CHECK(q.capacity() == 1);
  }

  // Check that padding doesn't overflow capacity
  {
    bool throws = false;
    try {
      SPSCQueue<int> q(SIZE_MAX - 1);
    } catch (...) {
      throws = true;
    }
    CHECK(throws);
  }

  {
    bool throws = false;
    try {
      SPSCQueue<int> q(SIZE_MAX);
    } catch (...) {
      throws = true;
    }
    CHECK(throws);
  }
}

static void testConcurrency() {
  using namespace rigtorp;

  // Fuzz and performance test
  {
    const size_t iter = 100000;
    SPSCQueue<size_t> q(iter / 1000 + 1);
    std::atomic<bool> flag(false);
    std::thread producer([&] {
      while (!flag)
        ;
      for (size_t i = 0; i < iter; ++i) {
        q.emplace(i);
      }
    });

    size_t sum = 0;
    auto start = std::chrono::system_clock::now();
    flag = true;
    for (size_t i = 0; i < iter; ++i) {
      while (!q.front())
        ;
      sum += *q.front();
      q.pop();
    }
    auto end = std::chrono::system_clock::now();
    auto duration =
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);

    CHECK(q.front() == nullptr);
    CHECK(sum == iter * (iter - 1) / 2);

    producer.join();

    std::cout << duration.count() / static_cast<int64_t>(iter) << " ns/iter"
              << std::endl;
  }

  // Contended try_emplace / try_push with multi-word payload and small ring
  {
    struct Payload {
      size_t seq;
      size_t check;
      explicit Payload(size_t s) noexcept : seq(s), check(s ^ 0x5a5a5a5a) {}
    };

    const size_t iter = 50000;
    SPSCQueue<Payload> q(7);
    std::thread producer([&] {
      for (size_t i = 0; i < iter; ++i) {
        while (!q.try_emplace(i)) {
          CHECK(q.size() <= q.capacity());
        }
      }
    });

    for (size_t i = 0; i < iter; ++i) {
      Payload *p = nullptr;
      while ((p = q.front()) == nullptr) {
        CHECK(q.size() <= q.capacity());
      }
      CHECK(p->seq == i);
      CHECK(p->check == (i ^ 0x5a5a5a5a));
      q.pop();
    }

    producer.join();
    CHECK(q.empty());
    CHECK(q.size() == 0);
  }

  // Blocking emplace unblocks when consumer drains a full queue
  {
    SPSCQueue<int> q(2);
    q.push(1);
    q.push(2);

    std::atomic<bool> pushedThird(false);
    std::thread consumer([&] {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      CHECK(*q.front() == 1);
      q.pop();
      CHECK(*q.front() == 2);
      q.pop();
    });

    q.push(3);
    pushedThird.store(true);
    consumer.join();

    CHECK(pushedThird.load());
    CHECK(*q.front() == 3);
    q.pop();
    CHECK(q.empty());
  }
}

static void testDeathPopEmpty() {
#if (defined(__unix__) || defined(__APPLE__)) && !defined(NDEBUG)
  using namespace rigtorp;

  std::cout.flush();
  std::cerr.flush();

  pid_t pid = fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
      dup2(devnull, STDERR_FILENO);
      close(devnull);
    }
    // Exit from the SIGABRT handler so assert() failure does not invoke
    // coredumpctl / ABRT via default SIGABRT termination.
    std::signal(SIGABRT, [](int) { _exit(42); });
    SPSCQueue<int> q(2);
    q.pop(); // Must abort in debug builds
    _exit(0);
  }

  int status = 0;
  CHECK(waitpid(pid, &status, 0) == pid);
  CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 42);
#endif
}

static void testTryPop() {
  using namespace rigtorp;

  // Empty, wrap-around, and cached writeIdx update
  {
    SPSCQueue<int> q(2);
    int v = -1;
    CHECK(!q.try_pop(v));
    CHECK(v == -1);

    for (int i = 0; i < 6; ++i) {
      CHECK(q.try_push(i));
      CHECK(q.try_pop(v));
      CHECK(v == i);
      CHECK(!q.try_pop(v));
    }
  }

  // Move-only type
  {
    SPSCQueue<std::unique_ptr<int>> q(2);
    q.emplace(std::make_unique<int>(99));
    std::unique_ptr<int> out;
    CHECK(q.try_pop(out));
    CHECK(out && *out == 99);
    CHECK(!q.try_pop(out));
  }

  // Copy-only type
  {
    struct CopyOnly {
      int x;
      explicit CopyOnly(int v) noexcept : x(v) {}
      CopyOnly(const CopyOnly &) = default;
      CopyOnly &operator=(const CopyOnly &) = default;
      CopyOnly(CopyOnly &&) = delete;
      CopyOnly &operator=(CopyOnly &&) = delete;
    };
    SPSCQueue<CopyOnly> q(2);
    CopyOnly in(7);
    q.push(in);
    CopyOnly out(0);
    CHECK(q.try_pop(out));
    CHECK(out.x == 7);
    CHECK(!q.try_pop(out));
  }

  // try_consume with immovable and move-only types
  {
    struct Immovable {
      int x;
      explicit Immovable(int v) noexcept : x(v) {}
      Immovable(const Immovable &) = delete;
      Immovable &operator=(const Immovable &) = delete;
      Immovable(Immovable &&) = delete;
      Immovable &operator=(Immovable &&) = delete;
    };
    SPSCQueue<Immovable> q(2);
    int observed = 0;
    CHECK(!q.try_consume([&](Immovable &v) { observed = v.x; }));
    q.emplace(123);
    CHECK(q.try_consume([&](Immovable &v) { observed = v.x; }));
    CHECK(observed == 123);
    CHECK(q.empty());

    SPSCQueue<std::unique_ptr<int>> mq(2);
    mq.emplace(std::make_unique<int>(456));
    std::unique_ptr<int> out;
    CHECK(mq.try_consume(
        [&](std::unique_ptr<int> &p) { out = std::move(p); }));
    CHECK(out && *out == 456);
    CHECK(mq.empty());
  }
}

int main(int argc, char *argv[]) {
  (void)argc, (void)argv;

  testFunctionality();
  testTypeCategories();
  testExceptionSafety();
  testCustomAllocator();
  testCapacityBounds();
  testDeathPopEmpty();
  testTryPop();
  testConcurrency();

  return 0;
}
