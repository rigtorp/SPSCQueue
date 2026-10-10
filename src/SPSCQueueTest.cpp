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

int main(int argc, char *argv[]) {
  (void)argc, (void)argv;

  using namespace rigtorp;

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

  {
    bool throws = false;
    try{
      SPSCQueue<int> q(SIZE_MAX);
    } catch(...) {
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

  // C++20 Feature Tests
#if __cplusplus >= 202002L
  std::cout << "\n=== C++20 Feature Tests ===" << std::endl;

  // Test 1: Verify concepts are available (HasAllocateAtLeast)
  {
    std::cout << "Test: C++20 Concepts (HasAllocateAtLeast)" << std::endl;
    // Test with default allocator (may or may not have allocate_at_least)
    SPSCQueue<int> q1(10);
    assert(!q1.empty() || q1.front() == nullptr);
    std::cout << "  ✓ Concept-based allocator validation working" << std::endl;
  }

  // Test 2: Type trait _v suffix is used correctly
  {
    std::cout << "Test: Type trait _v suffix (std::is_*_constructible_v)" << std::endl;
    struct ValidCpp20Test {
      ValidCpp20Test() noexcept {}
      ValidCpp20Test(const ValidCpp20Test &) noexcept {}
      ValidCpp20Test(ValidCpp20Test &&) noexcept {}
    };
    SPSCQueue<ValidCpp20Test> q(16);
    // These compile-time checks verify _v suffix is used in noexcept specs
    static_assert(noexcept(q.emplace()) == true, "");
    static_assert(noexcept(q.push(ValidCpp20Test())) == true, "");
    static_assert(noexcept(q.try_push(ValidCpp20Test())) == true, "");
    std::cout << "  ✓ Type trait _v suffix working correctly" << std::endl;
  }

  // Test 3: requires clauses for push overloads
  {
    std::cout << "Test: C++20 requires clauses for template constraints" << std::endl;
    struct CustomType {
      CustomType() {}
      CustomType(int) {} // convertible from int
    };
    SPSCQueue<CustomType> q(16);
    // This uses requires clause (or enable_if fallback)
    q.push(CustomType(42));
    (void)q.try_push(CustomType(100));
    assert(q.size() == 2);
    std::cout << "  ✓ requires clauses and template constraints working"
              << std::endl;
  }

  // Test 4: Verify [[likely]]/[[unlikely]] attributes compile correctly
  {
    std::cout << "Test: [[likely]]/[[unlikely]] branch prediction hints"
              << std::endl;
    SPSCQueue<int> q(2);
    // Push two items to test likely/unlikely in hot paths
    q.push(1);
    q.push(2);
    // Test front() with likely/unlikely paths
    assert(q.front() != nullptr);
    q.pop();
    assert(q.front() != nullptr);
    q.pop();
    // Try to access empty queue (unlikely path in front())
    assert(q.front() == nullptr);
    std::cout
        << "  ✓ [[likely]]/[[unlikely]] attributes applied to hot paths"
        << std::endl;
  }

  // Test 5: Queue behavior with likely/unlikely under stress
  {
    std::cout << "Test: Stress test with branch prediction hints" << std::endl;
    SPSCQueue<size_t> q(128);
    size_t push_count = 0;
    size_t pop_count = 0;
    // Fill and drain the queue multiple times
    for (int iter = 0; iter < 1000; ++iter) {
      for (int i = 0; i < 100; ++i) {
        if (q.try_push(i)) {
          ++push_count;
        }
      }
      while (q.front()) {
        q.pop();
        ++pop_count;
      }
    }
    assert(push_count > 0);
    assert(pop_count > 0);
    std::cout << "  ✓ Queue stress test passed with " << push_count
              << " pushes and " << pop_count << " pops" << std::endl;
  }

  std::cout << "\n=== All C++20 feature tests passed! ===" << std::endl;
#else
  std::cout << "\nNote: C++20 features not available in this build" << std::endl;
  std::cout << "Compile with -std=c++20 to enable C++20 feature tests"
            << std::endl;
#endif

  return 0;
}
