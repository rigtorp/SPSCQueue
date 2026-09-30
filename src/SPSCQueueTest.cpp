/*
Copyright (c) 2018 Erik Rigtorp <erik@rigtorp.se>

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

#undef NDEBUG

#include <cassert>
#include <chrono>
#include <iostream>
#include <rigtorp/SPSCQueue.h>
#include <set>
#include <thread>

// TestType tracks correct usage of constructors and destructors
struct TestType {
  static std::set<const TestType *> constructed;
  TestType() noexcept {
    assert(constructed.count(this) == 0);
    constructed.insert(this);
  };
  TestType(const TestType &other) noexcept {
    assert(constructed.count(this) == 0);
    assert(constructed.count(&other) == 1);
    constructed.insert(this);
  };
  TestType(TestType &&other) noexcept {
    assert(constructed.count(this) == 0);
    assert(constructed.count(&other) == 1);
    constructed.insert(this);
  };
  TestType &operator=(const TestType &other) noexcept {
    assert(constructed.count(this) == 1);
    assert(constructed.count(&other) == 1);
    return *this;
  };
  TestType &operator=(TestType &&other) noexcept {
    assert(constructed.count(this) == 1);
    assert(constructed.count(&other) == 1);
    return *this;
  }
  ~TestType() noexcept {
    assert(constructed.count(this) == 1);
    constructed.erase(this);
  };
};

std::set<const TestType *> TestType::constructed;

// Record allocation requests without attempting enormous real allocations.
template <typename T> struct RecordingAllocator {
  using value_type = T;
  size_t *requested;

  T *allocate(size_t n) {
    *requested = n;
    throw std::bad_alloc();
  }

  void deallocate(T *, size_t) {}
};

#if defined(__cpp_if_constexpr) && defined(__cpp_lib_void_t)
template <typename T>
struct RecordingAtLeastAllocator : RecordingAllocator<T> {
  struct AllocationResult {
    T *ptr;
    size_t count;
  };

  AllocationResult allocate_at_least(size_t n) {
    return {this->allocate(n), n};
  }
};
#endif

template <typename Allocator> void testCapacityOverflow() {
  // char elements require one cache line's worth of padding at each end.
  const size_t padding = alignof(rigtorp::SPSCQueue<char>);
  const size_t maxCapacity = SIZE_MAX - 2 * padding - 1;
  size_t requested = 0;
  Allocator allocator;
  allocator.requested = &requested;

  for (size_t capacity : {maxCapacity + 1, SIZE_MAX - 1, SIZE_MAX}) {
    bool throws = false;
    try {
      rigtorp::SPSCQueue<char, Allocator> q(capacity, allocator);
    } catch (const std::length_error &) {
      throws = true;
    }
    assert(throws);
    assert(requested == 0);
  }

  // Requests that fit must reach the allocator unchanged, including the
  // largest representable allocation and zero's minimum-capacity adjustment.
  for (size_t capacity : {size_t(0), size_t(1), maxCapacity - 1, maxCapacity}) {
    requested = 0;
    bool throws = false;
    try {
      rigtorp::SPSCQueue<char, Allocator> q(capacity, allocator);
    } catch (const std::bad_alloc &) {
      throws = true;
    }
    assert(throws);
    assert(requested == (capacity == 0 ? 1 : capacity) + 1 + 2 * padding);
  }
}

int main(int argc, char *argv[]) {
  (void)argc, (void)argv;

  using namespace rigtorp;

  testCapacityOverflow<RecordingAllocator<char>>();
#if defined(__cpp_if_constexpr) && defined(__cpp_lib_void_t)
  testCapacityOverflow<RecordingAtLeastAllocator<char>>();
#endif

  // Functionality test
  {
    SPSCQueue<TestType> q(10);
    assert(q.front() == nullptr);
    assert(q.size() == 0);
    assert(q.empty() == true);
    assert(q.capacity() == 10);
    for (int i = 0; i < 10; i++) {
      q.emplace();
    }
    assert(q.front() != nullptr);
    assert(q.size() == 10);
    assert(q.empty() == false);
    assert(TestType::constructed.size() == 10);
    assert(q.try_emplace() == false);
    q.pop();
    assert(q.size() == 9);
    assert(TestType::constructed.size() == 9);
    q.pop();
    assert(q.try_emplace() == true);
    assert(TestType::constructed.size() == 9);
  }
  assert(TestType::constructed.size() == 0);

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
    // lvalue
    // auto v = std::unique_ptr<int>(new int(1));
    // q.emplace(v);
    // q.try_emplace(v);
    // q.push(v);
    // q.try_push(v);
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

  // capacity < 1
  {
    SPSCQueue<int> q(0);
    assert(q.capacity() == 1);
  }

  // Minimum capacity still supports repeated full/empty transitions.
  for (size_t capacity : {size_t(0), size_t(1)}) {
    SPSCQueue<int> q(capacity);
    for (int value = 0; value < 10; ++value) {
      assert(q.empty());
      assert(q.try_push(value));
      assert(!q.try_push(value + 1));
      assert(q.size() == 1);
      assert(*q.front() == value);
      q.pop();
      assert(q.front() == nullptr);
    }
  }

  // Check that padding doesn't overflow capacity
  {
    bool throws = false;
    try {
      SPSCQueue<int> q(SIZE_MAX - 1);
    } catch (...) {
      throws = true;
    }
    assert(throws);
  }

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

    assert(q.front() == nullptr);
    assert(sum == iter * (iter - 1) / 2);

    producer.join();

    std::cout << duration.count() / iter << " ns/iter" << std::endl;
  }

  return 0;
}
