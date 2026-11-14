// SPDX-FileCopyrightText: 2018 Erik Rigtorp <erik@rigtorp.se>
// SPDX-License-Identifier: MIT

#include <chrono>
#include <iostream>
#include <rigtorp/SPSCQueue.h>
#include <stdexcept>
#include <thread>

#if __has_include(<boost/lockfree/spsc_queue.hpp> )
#include <boost/lockfree/spsc_queue.hpp>
#endif

#if __has_include(<folly/ProducerConsumerQueue.h>)
#include <folly/ProducerConsumerQueue.h>
#endif

#if __has_include(<rigtorp/MPMCQueue.h>)
#include <rigtorp/MPMCQueue.h>
#endif

#if __has_include(<readerwriterqueue/readerwriterqueue.h>)
#include <readerwriterqueue/readerwriterqueue.h>
#elif __has_include(<readerwriterqueue.h>)
#include <readerwriterqueue.h>
#endif

#if __has_include(<dro/spsc-queue.hpp>)
#include <dro/spsc-queue.hpp>
#endif

#if __has_include(<atomic_queue/atomic_queue.h>)
#include <atomic_queue/atomic_queue.h>
#endif

void pinThread(int cpu) {
  if (cpu < 0) {
    return;
  }
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(static_cast<size_t>(cpu), &cpuset);
  if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) ==
      -1) {
    perror("pthread_setaffinity_np");
    exit(1);
  }
}

int main(int argc, char *argv[]) {
  (void)argc, (void)argv;

  using namespace rigtorp;

  int cpu1 = -1;
  int cpu2 = -1;

  if (argc == 3) {
    cpu1 = std::stoi(argv[1]);
    cpu2 = std::stoi(argv[2]);
  }

  const size_t queueSize = 10000000;
  const int64_t iters = 10000000;

  std::cout << "SPSCQueue:" << std::endl;

  {
    SPSCQueue<int> q(queueSize);
    auto t = std::thread([&] {
      pinThread(cpu1);
      for (int64_t i = 0; i < iters; ++i) {
        while (!q.front())
          ;
        if (*q.front() != i) {
          throw std::runtime_error("");
        }
        q.pop();
      }
    });

    pinThread(cpu2);

    auto start = std::chrono::steady_clock::now();
    for (int64_t i = 0; i < iters; ++i) {
      q.emplace(static_cast<int>(i));
    }
    t.join();
    auto stop = std::chrono::steady_clock::now();
    std::cout << iters * 1000000 /
                     std::chrono::duration_cast<std::chrono::nanoseconds>(stop -
                                                                          start)
                         .count()
              << " ops/ms" << std::endl;
  }

  {
    SPSCQueue<int> q1(queueSize), q2(queueSize);
    auto t = std::thread([&] {
      pinThread(cpu1);
      for (int64_t i = 0; i < iters; ++i) {
        while (!q1.front())
          ;
        q2.emplace(*q1.front());
        q1.pop();
      }
    });

    pinThread(cpu2);

    auto start = std::chrono::steady_clock::now();
    for (int64_t i = 0; i < iters; ++i) {
      q1.emplace(static_cast<int>(i));
      while (!q2.front())
        ;
      q2.pop();
    }
    auto stop = std::chrono::steady_clock::now();
    t.join();
    std::cout << std::chrono::duration_cast<std::chrono::nanoseconds>(stop -
                                                                      start)
                         .count() /
                     iters
              << " ns RTT" << std::endl;
  }

#if __has_include(<boost/lockfree/spsc_queue.hpp> )
  std::cout << "boost::lockfree::spsc:" << std::endl;
  {
    boost::lockfree::spsc_queue<int> q(queueSize);
    auto t = std::thread([&] {
      pinThread(cpu1);
      for (int64_t i = 0; i < iters; ++i) {
        int val;
        while (!q.pop(val))
          ;
        if (val != i) {
          throw std::runtime_error("");
        }
      }
    });

    pinThread(cpu2);

    auto start = std::chrono::steady_clock::now();
    for (int64_t i = 0; i < iters; ++i) {
      while (!q.push(static_cast<int>(i)))
        ;
    }
    t.join();
    auto stop = std::chrono::steady_clock::now();
    std::cout << iters * 1000000 /
                     std::chrono::duration_cast<std::chrono::nanoseconds>(stop -
                                                                          start)
                         .count()
              << " ops/ms" << std::endl;
  }

  {
    boost::lockfree::spsc_queue<int> q1(queueSize), q2(queueSize);
    auto t = std::thread([&] {
      pinThread(cpu1);
      for (int64_t i = 0; i < iters; ++i) {
        int val;
        while (!q1.pop(val))
          ;
        while (!q2.push(val))
          ;
      }
    });

    pinThread(cpu2);

    auto start = std::chrono::steady_clock::now();
    for (int64_t i = 0; i < iters; ++i) {
      while (!q1.push(static_cast<int>(i)))
        ;
      int val;
      while (!q2.pop(val))
        ;
    }
    auto stop = std::chrono::steady_clock::now();
    t.join();
    std::cout << std::chrono::duration_cast<std::chrono::nanoseconds>(stop -
                                                                      start)
                         .count() /
                     iters
              << " ns RTT" << std::endl;
  }
#endif

#if __has_include(<folly/ProducerConsumerQueue.h>)
  std::cout << "folly::ProducerConsumerQueue:" << std::endl;

  {
    folly::ProducerConsumerQueue<int> q(queueSize);
    auto t = std::thread([&] {
      pinThread(cpu1);
      for (int64_t i = 0; i < iters; ++i) {
        int val;
        while (!q.read(val))
          ;
        if (val != i) {
          throw std::runtime_error("");
        }
      }
    });

    pinThread(cpu2);

    auto start = std::chrono::steady_clock::now();
    for (int64_t i = 0; i < iters; ++i) {
      while (!q.write(i))
        ;
    }
    t.join();
    auto stop = std::chrono::steady_clock::now();
    std::cout << iters * 1000000 /
                     std::chrono::duration_cast<std::chrono::nanoseconds>(stop -
                                                                          start)
                         .count()
              << " ops/ms" << std::endl;
  }

  {
    folly::ProducerConsumerQueue<int> q1(queueSize), q2(queueSize);
    auto t = std::thread([&] {
      pinThread(cpu1);
      for (int64_t i = 0; i < iters; ++i) {
        int val;
        while (!q1.read(val))
          ;
        q2.write(val);
      }
    });

    pinThread(cpu2);

    auto start = std::chrono::steady_clock::now();
    for (int64_t i = 0; i < iters; ++i) {
      while (!q1.write(i))
        ;
      int val;
      while (!q2.read(val))
        ;
    }
    auto stop = std::chrono::steady_clock::now();
    t.join();
    std::cout << std::chrono::duration_cast<std::chrono::nanoseconds>(stop -
                                                                      start)
                         .count() /
                     iters
              << " ns RTT" << std::endl;
  }
#endif

#if __has_include(<rigtorp/MPMCQueue.h>)
  std::cout << "rigtorp::MPMCQueue:" << std::endl;

  {
    MPMCQueue<int> q(queueSize);
    auto t = std::thread([&] {
      pinThread(cpu1);
      for (int64_t i = 0; i < iters; ++i) {
        int val;
        q.pop(val);
        if (val != i) {
          throw std::runtime_error("");
        }
      }
    });

    pinThread(cpu2);

    auto start = std::chrono::steady_clock::now();
    for (int64_t i = 0; i < iters; ++i) {
      q.push(static_cast<int>(i));
    }
    t.join();
    auto stop = std::chrono::steady_clock::now();
    std::cout << iters * 1000000 /
                     std::chrono::duration_cast<std::chrono::nanoseconds>(stop -
                                                                          start)
                         .count()
              << " ops/ms" << std::endl;
  }

  {
    MPMCQueue<int> q1(queueSize), q2(queueSize);
    auto t = std::thread([&] {
      pinThread(cpu1);
      for (int64_t i = 0; i < iters; ++i) {
        int val;
        q1.pop(val);
        q2.push(val);
      }
    });

    pinThread(cpu2);

    auto start = std::chrono::steady_clock::now();
    for (int64_t i = 0; i < iters; ++i) {
      q1.push(static_cast<int>(i));
      int val;
      q2.pop(val);
    }
    auto stop = std::chrono::steady_clock::now();
    t.join();
    std::cout << std::chrono::duration_cast<std::chrono::nanoseconds>(stop -
                                                                      start)
                         .count() /
                     iters
              << " ns RTT" << std::endl;
  }
#endif

#if __has_include(<readerwriterqueue/readerwriterqueue.h>) ||                    \
    __has_include(<readerwriterqueue.h>)
  std::cout << "moodycamel::ReaderWriterQueue:" << std::endl;

  {
    moodycamel::ReaderWriterQueue<int> q(queueSize);
    auto t = std::thread([&] {
      pinThread(cpu1);
      for (int64_t i = 0; i < iters; ++i) {
        int val;
        while (!q.try_dequeue(val))
          ;
        if (val != i) {
          throw std::runtime_error("");
        }
      }
    });

    pinThread(cpu2);

    auto start = std::chrono::steady_clock::now();
    for (int64_t i = 0; i < iters; ++i) {
      while (!q.try_enqueue(static_cast<int>(i)))
        ;
    }
    t.join();
    auto stop = std::chrono::steady_clock::now();
    std::cout << iters * 1000000 /
                     std::chrono::duration_cast<std::chrono::nanoseconds>(stop -
                                                                          start)
                         .count()
              << " ops/ms" << std::endl;
  }

  {
    moodycamel::ReaderWriterQueue<int> q1(queueSize), q2(queueSize);
    auto t = std::thread([&] {
      pinThread(cpu1);
      for (int64_t i = 0; i < iters; ++i) {
        int val;
        while (!q1.try_dequeue(val))
          ;
        q2.enqueue(val);
      }
    });

    pinThread(cpu2);

    auto start = std::chrono::steady_clock::now();
    for (int64_t i = 0; i < iters; ++i) {
      while (!q1.try_enqueue(static_cast<int>(i)))
        ;
      int val;
      while (!q2.try_dequeue(val))
        ;
    }
    auto stop = std::chrono::steady_clock::now();
    t.join();
    std::cout << std::chrono::duration_cast<std::chrono::nanoseconds>(stop -
                                                                      start)
                         .count() /
                     iters
              << " ns RTT" << std::endl;
  }
#endif

#if __has_include(<dro/spsc-queue.hpp>)
  std::cout << "dro::SPSCQueue:" << std::endl;

  {
    dro::SPSCQueue<int> q(queueSize);
    auto t = std::thread([&] {
      pinThread(cpu1);
      for (int64_t i = 0; i < iters; ++i) {
        int val;
        q.pop(val);
        if (val != i) {
          throw std::runtime_error("");
        }
      }
    });

    pinThread(cpu2);

    auto start = std::chrono::steady_clock::now();
    for (int64_t i = 0; i < iters; ++i) {
      q.emplace(static_cast<int>(i));
    }
    t.join();
    auto stop = std::chrono::steady_clock::now();
    std::cout << iters * 1000000 /
                     std::chrono::duration_cast<std::chrono::nanoseconds>(stop -
                                                                          start)
                         .count()
              << " ops/ms" << std::endl;
  }

  {
    dro::SPSCQueue<int> q1(queueSize), q2(queueSize);
    auto t = std::thread([&] {
      pinThread(cpu1);
      for (int64_t i = 0; i < iters; ++i) {
        int val;
        q1.pop(val);
        q2.emplace(val);
      }
    });

    pinThread(cpu2);

    auto start = std::chrono::steady_clock::now();
    for (int64_t i = 0; i < iters; ++i) {
      q1.emplace(static_cast<int>(i));
      int val;
      q2.pop(val);
    }
    auto stop = std::chrono::steady_clock::now();
    t.join();
    std::cout << std::chrono::duration_cast<std::chrono::nanoseconds>(stop -
                                                                      start)
                         .count() /
                     iters
              << " ns RTT" << std::endl;
  }
#endif

#if __has_include(<atomic_queue/atomic_queue.h>)
  std::cout << "atomic_queue::AtomicQueueB (SPSC):" << std::endl;

  {
    atomic_queue::AtomicQueueB<int, std::allocator<int>, -1, true, false, true>
        q(static_cast<unsigned>(queueSize));
    auto t = std::thread([&] {
      pinThread(cpu1);
      for (int64_t i = 0; i < iters; ++i) {
        int val = q.pop();
        if (val != i) {
          throw std::runtime_error("");
        }
      }
    });

    pinThread(cpu2);

    auto start = std::chrono::steady_clock::now();
    for (int64_t i = 0; i < iters; ++i) {
      q.push(static_cast<int>(i));
    }
    t.join();
    auto stop = std::chrono::steady_clock::now();
    std::cout << iters * 1000000 /
                     std::chrono::duration_cast<std::chrono::nanoseconds>(stop -
                                                                          start)
                         .count()
              << " ops/ms" << std::endl;
  }

  {
    atomic_queue::AtomicQueueB<int, std::allocator<int>, -1, true, false, true>
        q1(static_cast<unsigned>(queueSize)),
        q2(static_cast<unsigned>(queueSize));
    auto t = std::thread([&] {
      pinThread(cpu1);
      for (int64_t i = 0; i < iters; ++i) {
        q2.push(q1.pop());
      }
    });

    pinThread(cpu2);

    auto start = std::chrono::steady_clock::now();
    for (int64_t i = 0; i < iters; ++i) {
      q1.push(static_cast<int>(i));
      (void)q2.pop();
    }
    auto stop = std::chrono::steady_clock::now();
    t.join();
    std::cout << std::chrono::duration_cast<std::chrono::nanoseconds>(stop -
                                                                      start)
                         .count() /
                     iters
              << " ns RTT" << std::endl;
  }

  std::cout << "atomic_queue::AtomicQueueB2 (SPSC):" << std::endl;

  {
    atomic_queue::AtomicQueueB2<int, std::allocator<int>, true, false, true> q(
        static_cast<unsigned>(queueSize));
    auto t = std::thread([&] {
      pinThread(cpu1);
      for (int64_t i = 0; i < iters; ++i) {
        int val = q.pop();
        if (val != i) {
          throw std::runtime_error("");
        }
      }
    });

    pinThread(cpu2);

    auto start = std::chrono::steady_clock::now();
    for (int64_t i = 0; i < iters; ++i) {
      q.push(static_cast<int>(i));
    }
    t.join();
    auto stop = std::chrono::steady_clock::now();
    std::cout << iters * 1000000 /
                     std::chrono::duration_cast<std::chrono::nanoseconds>(stop -
                                                                          start)
                         .count()
              << " ops/ms" << std::endl;
  }

  {
    atomic_queue::AtomicQueueB2<int, std::allocator<int>, true, false, true>
        q1(static_cast<unsigned>(queueSize)),
        q2(static_cast<unsigned>(queueSize));
    auto t = std::thread([&] {
      pinThread(cpu1);
      for (int64_t i = 0; i < iters; ++i) {
        q2.push(q1.pop());
      }
    });

    pinThread(cpu2);

    auto start = std::chrono::steady_clock::now();
    for (int64_t i = 0; i < iters; ++i) {
      q1.push(static_cast<int>(i));
      (void)q2.pop();
    }
    auto stop = std::chrono::steady_clock::now();
    t.join();
    std::cout << std::chrono::duration_cast<std::chrono::nanoseconds>(stop -
                                                                      start)
                         .count() /
                     iters
              << " ns RTT" << std::endl;
  }
#endif

  return 0;
}
