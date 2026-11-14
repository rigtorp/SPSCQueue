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

template <typename Consumer, typename Producer>
static void benchThroughput(int cpu1, int cpu2, int64_t iters,
                            Consumer &&consumer, Producer &&producer) {
  auto run = [&] {
    auto t = std::thread([&] {
      pinThread(cpu1);
      for (int64_t i = 0; i < iters; ++i) {
        consumer(i);
      }
    });

    pinThread(cpu2);

    auto start = std::chrono::steady_clock::now();
    for (int64_t i = 0; i < iters; ++i) {
      producer(i);
    }
    t.join();
    auto stop = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start)
        .count();
  };

  run(); // warmup
  auto ns = run();
  std::cout << iters * 1000000 / ns << " ops/ms" << std::endl;
}

template <typename Consumer, typename Producer>
static void benchRTT(int cpu1, int cpu2, int64_t iters, Consumer &&consumer,
                     Producer &&producer) {
  auto run = [&] {
    auto t = std::thread([&] {
      pinThread(cpu1);
      for (int64_t i = 0; i < iters; ++i) {
        consumer();
      }
    });

    pinThread(cpu2);

    auto start = std::chrono::steady_clock::now();
    for (int64_t i = 0; i < iters; ++i) {
      producer(i);
    }
    auto stop = std::chrono::steady_clock::now();
    t.join();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start)
        .count();
  };

  run(); // warmup
  auto ns = run();
  std::cout << ns / iters << " ns RTT" << std::endl;
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
    benchThroughput(
        cpu1, cpu2, iters,
        [&](int64_t i) {
          while (!q.front())
            ;
          if (*q.front() != i) {
            throw std::runtime_error("");
          }
          q.pop();
        },
        [&](int64_t i) { q.emplace(static_cast<int>(i)); });
  }

  {
    SPSCQueue<int> q1(queueSize), q2(queueSize);
    benchRTT(
        cpu1, cpu2, iters,
        [&] {
          while (!q1.front())
            ;
          q2.emplace(*q1.front());
          q1.pop();
        },
        [&](int64_t i) {
          q1.emplace(static_cast<int>(i));
          while (!q2.front())
            ;
          q2.pop();
        });
  }

#if __has_include(<boost/lockfree/spsc_queue.hpp> )
  std::cout << "boost::lockfree::spsc:" << std::endl;
  {
    boost::lockfree::spsc_queue<int> q(queueSize);
    benchThroughput(
        cpu1, cpu2, iters,
        [&](int64_t i) {
          int val;
          while (!q.pop(val))
            ;
          if (val != i) {
            throw std::runtime_error("");
          }
        },
        [&](int64_t i) {
          while (!q.push(static_cast<int>(i)))
            ;
        });
  }

  {
    boost::lockfree::spsc_queue<int> q1(queueSize), q2(queueSize);
    benchRTT(
        cpu1, cpu2, iters,
        [&] {
          int val;
          while (!q1.pop(val))
            ;
          while (!q2.push(val))
            ;
        },
        [&](int64_t i) {
          while (!q1.push(static_cast<int>(i)))
            ;
          int val;
          while (!q2.pop(val))
            ;
        });
  }
#endif

#if __has_include(<folly/ProducerConsumerQueue.h>)
  std::cout << "folly::ProducerConsumerQueue:" << std::endl;

  {
    folly::ProducerConsumerQueue<int> q(queueSize);
    benchThroughput(
        cpu1, cpu2, iters,
        [&](int64_t i) {
          int val;
          while (!q.read(val))
            ;
          if (val != i) {
            throw std::runtime_error("");
          }
        },
        [&](int64_t i) {
          while (!q.write(static_cast<int>(i)))
            ;
        });
  }

  {
    folly::ProducerConsumerQueue<int> q1(queueSize), q2(queueSize);
    benchRTT(
        cpu1, cpu2, iters,
        [&] {
          int val;
          while (!q1.read(val))
            ;
          q2.write(val);
        },
        [&](int64_t i) {
          while (!q1.write(static_cast<int>(i)))
            ;
          int val;
          while (!q2.read(val))
            ;
        });
  }
#endif

#if __has_include(<rigtorp/MPMCQueue.h>)
  std::cout << "rigtorp::MPMCQueue:" << std::endl;

  {
    MPMCQueue<int> q(queueSize);
    benchThroughput(
        cpu1, cpu2, iters,
        [&](int64_t i) {
          int val;
          q.pop(val);
          if (val != i) {
            throw std::runtime_error("");
          }
        },
        [&](int64_t i) { q.push(static_cast<int>(i)); });
  }

  {
    MPMCQueue<int> q1(queueSize), q2(queueSize);
    benchRTT(
        cpu1, cpu2, iters,
        [&] {
          int val;
          q1.pop(val);
          q2.push(val);
        },
        [&](int64_t i) {
          q1.push(static_cast<int>(i));
          int val;
          q2.pop(val);
        });
  }
#endif

#if __has_include(<readerwriterqueue/readerwriterqueue.h>) ||                    \
    __has_include(<readerwriterqueue.h>)
  std::cout << "moodycamel::ReaderWriterQueue:" << std::endl;

  {
    moodycamel::ReaderWriterQueue<int> q(queueSize);
    benchThroughput(
        cpu1, cpu2, iters,
        [&](int64_t i) {
          int val;
          while (!q.try_dequeue(val))
            ;
          if (val != i) {
            throw std::runtime_error("");
          }
        },
        [&](int64_t i) {
          while (!q.try_enqueue(static_cast<int>(i)))
            ;
        });
  }

  {
    moodycamel::ReaderWriterQueue<int> q1(queueSize), q2(queueSize);
    benchRTT(
        cpu1, cpu2, iters,
        [&] {
          int val;
          while (!q1.try_dequeue(val))
            ;
          q2.enqueue(val);
        },
        [&](int64_t i) {
          while (!q1.try_enqueue(static_cast<int>(i)))
            ;
          int val;
          while (!q2.try_dequeue(val))
            ;
        });
  }
#endif

#if __has_include(<dro/spsc-queue.hpp>)
  std::cout << "dro::SPSCQueue:" << std::endl;

  {
    dro::SPSCQueue<int> q(queueSize);
    benchThroughput(
        cpu1, cpu2, iters,
        [&](int64_t i) {
          int val;
          q.pop(val);
          if (val != i) {
            throw std::runtime_error("");
          }
        },
        [&](int64_t i) { q.emplace(static_cast<int>(i)); });
  }

  {
    dro::SPSCQueue<int> q1(queueSize), q2(queueSize);
    benchRTT(
        cpu1, cpu2, iters,
        [&] {
          int val;
          q1.pop(val);
          q2.emplace(val);
        },
        [&](int64_t i) {
          q1.emplace(static_cast<int>(i));
          int val;
          q2.pop(val);
        });
  }
#endif

#if __has_include(<atomic_queue/atomic_queue.h>)
  std::cout << "atomic_queue::AtomicQueueB (SPSC):" << std::endl;

  {
    atomic_queue::AtomicQueueB<int, std::allocator<int>, -1, true, false, true>
        q(static_cast<unsigned>(queueSize));
    benchThroughput(
        cpu1, cpu2, iters,
        [&](int64_t i) {
          if (q.pop() != i) {
            throw std::runtime_error("");
          }
        },
        [&](int64_t i) { q.push(static_cast<int>(i)); });
  }

  {
    atomic_queue::AtomicQueueB<int, std::allocator<int>, -1, true, false, true>
        q1(static_cast<unsigned>(queueSize)),
        q2(static_cast<unsigned>(queueSize));
    benchRTT(
        cpu1, cpu2, iters, [&] { q2.push(q1.pop()); },
        [&](int64_t i) {
          q1.push(static_cast<int>(i));
          (void)q2.pop();
        });
  }

  std::cout << "atomic_queue::AtomicQueueB2 (SPSC):" << std::endl;

  {
    atomic_queue::AtomicQueueB2<int, std::allocator<int>, true, false, true> q(
        static_cast<unsigned>(queueSize));
    benchThroughput(
        cpu1, cpu2, iters,
        [&](int64_t i) {
          if (q.pop() != i) {
            throw std::runtime_error("");
          }
        },
        [&](int64_t i) { q.push(static_cast<int>(i)); });
  }

  {
    atomic_queue::AtomicQueueB2<int, std::allocator<int>, true, false, true>
        q1(static_cast<unsigned>(queueSize)),
        q2(static_cast<unsigned>(queueSize));
    benchRTT(
        cpu1, cpu2, iters, [&] { q2.push(q1.pop()); },
        [&](int64_t i) {
          q1.push(static_cast<int>(i));
          (void)q2.pop();
        });
  }
#endif

  return 0;
}
