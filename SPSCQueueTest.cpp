/*
Copyright (c) 2016 Erik Rigtorp <erik@rigtorp.se>

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

#include "SPSCQueue.h"
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>

// TestType has a global reference count and detects incorrect use of placement
// new
struct TestType {
  static int refCount;
  static constexpr uint64_t kMagic = 0x38c8fb6f8207a508;
  uint64_t magic;
  TestType() {
    refCount++;
    assert(magic != kMagic);
    magic = kMagic;
  };
  TestType(const TestType &) {
    refCount++;
    assert(magic != kMagic);
    magic = kMagic;
  };
  TestType(TestType &&) noexcept {
    refCount++;
    assert(magic != kMagic);
    magic = kMagic;
  };
  TestType &operator=(const TestType &other) noexcept {
    assert(magic == kMagic);
    assert(other.magic == kMagic);
    return *this;
  };
  TestType &operator=(TestType &&other) noexcept {
    assert(magic == kMagic);
    assert(other.magic == kMagic);
    return *this;
  }
  ~TestType() noexcept {
    refCount--;
    assert(magic == kMagic);
    magic = 0;
  };
};

int TestType::refCount = 0;

int main(int argc, char *argv[]) {

  using namespace rigtorp;

  // Functionality test
  {
    SPSCQueue<TestType> q(11);
    assert(q.front() == nullptr);
    assert(q.size() == 0);
    assert(q.empty() == true);
    assert(q.capacity() == 11);
    for (int i = 0; i < 10; i++) {
      q.emplace();
    }
    assert(q.front() != nullptr);
    assert(q.size() == 10);
    assert(q.empty() == false);
    assert(TestType::refCount == 10);
    assert(q.try_emplace() == false);
    q.pop();
    assert(q.size() == 9);
    assert(TestType::refCount == 9);
    q.pop();
    assert(q.try_emplace() == true);
    assert(TestType::refCount == 9);
  }
  assert(TestType::refCount == 0);

  // Test we throw when capacity < 2
  {
    bool throws = false;
    try {
      SPSCQueue<int> q(0);
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
