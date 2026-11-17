// SPDX-FileCopyrightText: 2018 Erik Rigtorp <erik@rigtorp.se>
// SPDX-License-Identifier: MIT

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <rigtorp/SPSCQueue.h>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

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

#if __has_include(<rte_ring.h>) && __has_include(<rte_ring_elem.h>)
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wzero-as-null-pointer-constant"
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wcast-align"
#pragma GCC diagnostic ignored "-Wshadow"
#endif
extern "C" {
#include <rte_ring.h>
#include <rte_ring_elem.h>
}
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
#endif

void pinThread(int cpu) {
  if (cpu < 0) {
    return;
  }
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(static_cast<size_t>(cpu), &cpuset);
  const int error =
      pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
  if (error != 0) {
    std::cerr << "pthread_setaffinity_np: " << std::strerror(error)
              << std::endl;
    std::exit(EXIT_FAILURE);
  }
}

template <typename Consumer, typename Producer>
static int64_t runSynced(int cpu1, int cpu2, Consumer &&consumer,
                         Producer &&producer) {
  std::atomic<bool> consumerReady{false};
  std::atomic<bool> go{false};
  auto t = std::thread([&] {
    pinThread(cpu1);
    consumerReady.store(true, std::memory_order_release);
    while (!go.load(std::memory_order_acquire))
      ;
    consumer();
  });

  pinThread(cpu2);
  while (!consumerReady.load(std::memory_order_acquire))
    ;

  auto start = std::chrono::steady_clock::now();
  go.store(true, std::memory_order_release);
  producer();
  t.join();
  auto stop = std::chrono::steady_clock::now();
  return std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start)
      .count();
}

template <typename Consumer, typename Producer>
static int64_t runTPSingle(int cpu1, int cpu2, int64_t iters,
                           Consumer &&consumer, Producer &&producer) {
  constexpr int64_t prefill = 4096;
  auto run = [&] {
    for (int64_t i = 0; i < prefill; ++i) {
      producer(i);
    }
    auto ns = runSynced(
        cpu1, cpu2,
        [&] {
          for (int64_t i = 0; i < iters; ++i) {
            consumer(i);
          }
        },
        [&] {
          for (int64_t i = prefill; i < iters + prefill; ++i) {
            producer(i);
          }
        });
    for (int64_t i = iters; i < iters + prefill; ++i) {
      consumer(i);
    }
    return ns;
  };

  run(); // warmup
  return iters * 1000000 / run();
}

template <typename Consumer, typename Producer>
static int64_t runRTTSingle(int cpu1, int cpu2, int64_t iters,
                            Consumer &&consumer, Producer &&producer) {
  auto run = [&] {
    return runSynced(
        cpu1, cpu2,
        [&] {
          for (int64_t i = 0; i < iters; ++i) {
            consumer();
          }
        },
        [&] {
          for (int64_t i = 0; i < iters; ++i) {
            producer(i);
          }
        });
  };

  run(); // warmup
  return run() / iters;
}

template <typename MakeQ, typename Pop, typename Push>
static int64_t runColdSingle(int cpu1, int cpu2, size_t numQueues,
                             int64_t coldRounds, MakeQ &&makeQ, Pop &&pop,
                             Push &&push) {
  std::vector<decltype(makeQ())> qs1, qs2;
  qs1.reserve(numQueues);
  qs2.reserve(numQueues);
  for (size_t i = 0; i < numQueues; ++i) {
    qs1.emplace_back(makeQ());
    qs2.emplace_back(makeQ());
  }

  auto run = [&] {
    return runSynced(
        cpu1, cpu2,
        [&] {
          for (int64_t r = 0; r < coldRounds; ++r) {
            for (size_t q = 0; q < numQueues; ++q) {
              pop(*qs1[q], static_cast<int>(q));
              push(*qs2[q], static_cast<int>(q));
            }
          }
        },
        [&] {
          for (int64_t r = 0; r < coldRounds; ++r) {
            for (size_t q = 0; q < numQueues; ++q) {
              push(*qs1[q], static_cast<int>(q));
              pop(*qs2[q], static_cast<int>(q));
            }
          }
        });
  };

  run(); // warmup
  return run() / (coldRounds * static_cast<int64_t>(numQueues));
}

struct BenchEntry {
  std::string name;
  std::function<int64_t(size_t, int64_t)> runTP;
  std::function<int64_t()> runRTT;
  std::function<int64_t()> runCold;
  std::vector<int64_t> tpDecoupledResults{};
  std::vector<int64_t> tpBoundedResults{};
  std::vector<int64_t> rttResults{};
  std::vector<int64_t> coldResults{};
};

static void printSummary(std::vector<int64_t> &v, const char *label,
                         const char *unit, double scale = 1.0,
                         int precision = 0) {
  std::sort(v.begin(), v.end());
  std::cout << std::fixed << std::setprecision(precision) << "  " << label
            << ": min=" << static_cast<double>(v.front()) * scale
            << " median=" << static_cast<double>(v[v.size() / 2]) * scale
            << " max=" << static_cast<double>(v.back()) * scale << " " << unit
            << std::endl;
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

  const size_t decoupledCap = 1u << 21; // 2,097,152 (8 MiB for 4B slots)
  const int64_t decoupledIters = 2000000;
  const size_t boundedCap = 1u << 16; // 65,536
  const int64_t boundedIters = 50000000;
  const size_t queueSize = boundedCap;
  const int64_t rttIters = 2000000;
  const size_t coldQueues = 262144;
  const int64_t coldRounds = 4;

  std::vector<BenchEntry> entries;

  entries.push_back({"SPSCQueue:",
                     [&](size_t cap, int64_t iters) {
                       SPSCQueue<int> q(cap);
                       return runTPSingle(
                           cpu1, cpu2, iters,
                           [&](int64_t i) {
                             int *p;
                             while (!(p = q.front()))
                               ;
                             if (*p != i) {
                               throw std::runtime_error("");
                             }
                             q.pop();
                           },
                           [&](int64_t i) { q.emplace(static_cast<int>(i)); });
                     },
                     [&] {
                       SPSCQueue<int> q1(queueSize), q2(queueSize);
                       return runRTTSingle(
                           cpu1, cpu2, rttIters,
                           [&] {
                             int *p;
                             while (!(p = q1.front()))
                               ;
                             q2.emplace(*p);
                             q1.pop();
                           },
                           [&](int64_t i) {
                             q1.emplace(static_cast<int>(i));
                             while (!q2.front())
                               ;
                             q2.pop();
                           });
                     },
                     [&] {
                       return runColdSingle(
                           cpu1, cpu2, coldQueues, coldRounds,
                           [] { return std::make_unique<SPSCQueue<int>>(8); },
                           [](SPSCQueue<int> &q, int expected) {
                             int *p;
                             while (!(p = q.front()))
                               ;
                             if (*p != expected) {
                               throw std::runtime_error("");
                             }
                             q.pop();
                           },
                           [](SPSCQueue<int> &q, int val) { q.emplace(val); });
                     }});

  entries.push_back({"SPSCQueue (try_pop):",
                     [&](size_t cap, int64_t iters) {
                       SPSCQueue<int> q(cap);
                       return runTPSingle(
                           cpu1, cpu2, iters,
                           [&](int64_t i) {
                             int val;
                             while (!q.try_pop(val))
                               ;
                             if (val != i) {
                               throw std::runtime_error("");
                             }
                           },
                           [&](int64_t i) { q.emplace(static_cast<int>(i)); });
                     },
                     [&] {
                       SPSCQueue<int> q1(queueSize), q2(queueSize);
                       return runRTTSingle(
                           cpu1, cpu2, rttIters,
                           [&] {
                             int val;
                             while (!q1.try_pop(val))
                               ;
                             q2.emplace(val);
                           },
                           [&](int64_t i) {
                             q1.emplace(static_cast<int>(i));
                             int val;
                             while (!q2.try_pop(val))
                               ;
                           });
                     },
                     [&] {
                       return runColdSingle(
                           cpu1, cpu2, coldQueues, coldRounds,
                           [] { return std::make_unique<SPSCQueue<int>>(8); },
                           [](SPSCQueue<int> &q, int expected) {
                             int val;
                             while (!q.try_pop(val))
                               ;
                             if (val != expected) {
                               throw std::runtime_error("");
                             }
                           },
                           [](SPSCQueue<int> &q, int val) { q.emplace(val); });
                     }});

  entries.push_back(
      {"SPSCQueue (try_consume):",
       [&](size_t cap, int64_t iters) {
         SPSCQueue<int> q(cap);
         return runTPSingle(
             cpu1, cpu2, iters,
             [&](int64_t i) {
               while (!q.try_consume([&](int &val) {
                 if (val != i) {
                   throw std::runtime_error("");
                 }
               }))
                 ;
             },
             [&](int64_t i) { q.emplace(static_cast<int>(i)); });
       },
       [&] {
         SPSCQueue<int> q1(queueSize), q2(queueSize);
         return runRTTSingle(
             cpu1, cpu2, rttIters,
             [&] {
               while (!q1.try_consume([&](int &val) { q2.emplace(val); }))
                 ;
             },
             [&](int64_t i) {
               q1.emplace(static_cast<int>(i));
               while (!q2.try_consume([](int &) {}))
                 ;
             });
       },
       [&] {
         return runColdSingle(
             cpu1, cpu2, coldQueues, coldRounds,
             [] { return std::make_unique<SPSCQueue<int>>(8); },
             [](SPSCQueue<int> &q, int expected) {
               while (!q.try_consume([&](int &val) {
                 if (val != expected) {
                   throw std::runtime_error("");
                 }
               }))
                 ;
             },
             [](SPSCQueue<int> &q, int val) { q.emplace(val); });
       }});

#if __has_include(<boost/lockfree/spsc_queue.hpp> )
  entries.push_back(
      {"boost::lockfree::spsc:",
       [&](size_t cap, int64_t iters) {
         boost::lockfree::spsc_queue<int> q(cap);
         return runTPSingle(
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
       },
       [&] {
         boost::lockfree::spsc_queue<int> q1(queueSize), q2(queueSize);
         return runRTTSingle(
             cpu1, cpu2, rttIters,
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
       },
       [&] {
         return runColdSingle(
             cpu1, cpu2, coldQueues, coldRounds,
             [] {
               return std::make_unique<boost::lockfree::spsc_queue<int>>(8);
             },
             [](boost::lockfree::spsc_queue<int> &q, int expected) {
               int val;
               while (!q.pop(val))
                 ;
               if (val != expected) {
                 throw std::runtime_error("");
               }
             },
             [](boost::lockfree::spsc_queue<int> &q, int val) {
               while (!q.push(val))
                 ;
             });
       }});
#endif

#if __has_include(<folly/ProducerConsumerQueue.h>)
  entries.push_back(
      {"folly::ProducerConsumerQueue:",
       [&](size_t cap, int64_t iters) {
         folly::ProducerConsumerQueue<int> q(static_cast<uint32_t>(cap));
         return runTPSingle(
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
       },
       [&] {
         folly::ProducerConsumerQueue<int> q1(queueSize), q2(queueSize);
         return runRTTSingle(
             cpu1, cpu2, rttIters,
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
       },
       [&] {
         return runColdSingle(
             cpu1, cpu2, coldQueues, coldRounds,
             [] {
               return std::make_unique<folly::ProducerConsumerQueue<int>>(8);
             },
             [](folly::ProducerConsumerQueue<int> &q, int expected) {
               int val;
               while (!q.read(val))
                 ;
               if (val != expected) {
                 throw std::runtime_error("");
               }
             },
             [](folly::ProducerConsumerQueue<int> &q, int val) {
               while (!q.write(val))
                 ;
             });
       }});
#endif

#if __has_include(<rigtorp/MPMCQueue.h>)
  entries.push_back({"rigtorp::MPMCQueue:",
                     [&](size_t cap, int64_t iters) {
                       MPMCQueue<int> q(cap);
                       return runTPSingle(
                           cpu1, cpu2, iters,
                           [&](int64_t i) {
                             int val;
                             q.pop(val);
                             if (val != i) {
                               throw std::runtime_error("");
                             }
                           },
                           [&](int64_t i) { q.push(static_cast<int>(i)); });
                     },
                     [&] {
                       MPMCQueue<int> q1(queueSize), q2(queueSize);
                       return runRTTSingle(
                           cpu1, cpu2, rttIters,
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
                     },
                     [&] {
                       return runColdSingle(
                           cpu1, cpu2, coldQueues, coldRounds,
                           [] { return std::make_unique<MPMCQueue<int>>(8); },
                           [](MPMCQueue<int> &q, int expected) {
                             int val;
                             q.pop(val);
                             if (val != expected) {
                               throw std::runtime_error("");
                             }
                           },
                           [](MPMCQueue<int> &q, int val) { q.push(val); });
                     }});
#endif

#if __has_include(<readerwriterqueue/readerwriterqueue.h>) ||                    \
    __has_include(<readerwriterqueue.h>)
  entries.push_back(
      {"moodycamel::ReaderWriterQueue:",
       [&](size_t cap, int64_t iters) {
         moodycamel::ReaderWriterQueue<int> q(cap);
         return runTPSingle(
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
       },
       [&] {
         moodycamel::ReaderWriterQueue<int> q1(queueSize), q2(queueSize);
         return runRTTSingle(
             cpu1, cpu2, rttIters,
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
       },
       [&] {
         return runColdSingle(
             cpu1, cpu2, coldQueues, coldRounds,
             [] {
               return std::make_unique<moodycamel::ReaderWriterQueue<int>>(8);
             },
             [](moodycamel::ReaderWriterQueue<int> &q, int expected) {
               int val;
               while (!q.try_dequeue(val))
                 ;
               if (val != expected) {
                 throw std::runtime_error("");
               }
             },
             [](moodycamel::ReaderWriterQueue<int> &q, int val) {
               while (!q.try_enqueue(val))
                 ;
             });
       }});
#endif

#if __has_include(<dro/spsc-queue.hpp>)
  entries.push_back(
      {"dro::SPSCQueue:",
       [&](size_t cap, int64_t iters) {
         dro::SPSCQueue<int> q(cap);
         return runTPSingle(
             cpu1, cpu2, iters,
             [&](int64_t i) {
               int val;
               q.pop(val);
               if (val != i) {
                 throw std::runtime_error("");
               }
             },
             [&](int64_t i) { q.emplace(static_cast<int>(i)); });
       },
       [&] {
         dro::SPSCQueue<int> q1(queueSize), q2(queueSize);
         return runRTTSingle(
             cpu1, cpu2, rttIters,
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
       },
       [&] {
         return runColdSingle(
             cpu1, cpu2, coldQueues, coldRounds,
             [] { return std::make_unique<dro::SPSCQueue<int>>(8); },
             [](dro::SPSCQueue<int> &q, int expected) {
               int val;
               q.pop(val);
               if (val != expected) {
                 throw std::runtime_error("");
               }
             },
             [](dro::SPSCQueue<int> &q, int val) { q.emplace(val); });
       }});
#endif

#if __has_include(<atomic_queue/atomic_queue.h>)
  entries.push_back({"atomic_queue::AtomicQueueB (SPSC):",
                     [&](size_t cap, int64_t iters) {
                       atomic_queue::AtomicQueueB<int, std::allocator<int>, -1,
                                                  true, false, true>
                           q(static_cast<unsigned>(cap));
                       return runTPSingle(
                           cpu1, cpu2, iters,
                           [&](int64_t i) {
                             if (q.pop() != i) {
                               throw std::runtime_error("");
                             }
                           },
                           [&](int64_t i) { q.push(static_cast<int>(i)); });
                     },
                     [&] {
                       atomic_queue::AtomicQueueB<int, std::allocator<int>, -1,
                                                  true, false, true>
                           q1(static_cast<unsigned>(queueSize)),
                           q2(static_cast<unsigned>(queueSize));
                       return runRTTSingle(
                           cpu1, cpu2, rttIters, [&] { q2.push(q1.pop()); },
                           [&](int64_t i) {
                             q1.push(static_cast<int>(i));
                             (void)q2.pop();
                           });
                     },
                     [&] {
                       using AQB =
                           atomic_queue::AtomicQueueB<int, std::allocator<int>,
                                                      -1, true, false, true>;
                       return runColdSingle(
                           cpu1, cpu2, coldQueues, coldRounds,
                           [] { return std::make_unique<AQB>(8); },
                           [](AQB &q, int expected) {
                             if (q.pop() != expected) {
                               throw std::runtime_error("");
                             }
                           },
                           [](AQB &q, int val) { q.push(val); });
                     }});

  entries.push_back({"atomic_queue::AtomicQueueB2 (SPSC):",
                     [&](size_t cap, int64_t iters) {
                       atomic_queue::AtomicQueueB2<int, std::allocator<int>,
                                                   true, false, true>
                           q(static_cast<unsigned>(cap));
                       return runTPSingle(
                           cpu1, cpu2, iters,
                           [&](int64_t i) {
                             if (q.pop() != i) {
                               throw std::runtime_error("");
                             }
                           },
                           [&](int64_t i) { q.push(static_cast<int>(i)); });
                     },
                     [&] {
                       atomic_queue::AtomicQueueB2<int, std::allocator<int>,
                                                   true, false, true>
                           q1(static_cast<unsigned>(queueSize)),
                           q2(static_cast<unsigned>(queueSize));
                       return runRTTSingle(
                           cpu1, cpu2, rttIters, [&] { q2.push(q1.pop()); },
                           [&](int64_t i) {
                             q1.push(static_cast<int>(i));
                             (void)q2.pop();
                           });
                     },
                     [&] {
                       using AQB2 =
                           atomic_queue::AtomicQueueB2<int, std::allocator<int>,
                                                       true, false, true>;
                       return runColdSingle(
                           cpu1, cpu2, coldQueues, coldRounds,
                           [] { return std::make_unique<AQB2>(8); },
                           [](AQB2 &q, int expected) {
                             if (q.pop() != expected) {
                               throw std::runtime_error("");
                             }
                           },
                           [](AQB2 &q, int val) { q.push(val); });
                     }});
#endif

#if __has_include(<rte_ring.h>) && __has_include(<rte_ring_elem.h>)
  {
    auto makeRing = [](const char *name, unsigned int count) {
      ssize_t sz = rte_ring_get_memsize_elem(sizeof(int), count);
      if (sz < 0) {
        throw std::runtime_error("rte_ring_get_memsize_elem failed");
      }
      void *mem = std::aligned_alloc(64, static_cast<size_t>(sz));
      if (!mem) {
        throw std::bad_alloc();
      }
      auto *r = static_cast<struct rte_ring *>(mem);
      if (rte_ring_init(r, name, count, RING_F_SP_ENQ | RING_F_SC_DEQ) != 0) {
        std::free(mem);
        throw std::runtime_error("rte_ring_init failed");
      }
      return r;
    };
    const unsigned int ringCount = 1u << 16;

    entries.push_back(
        {"DPDK rte_ring (SP/SC):",
         [&, makeRing](size_t cap, int64_t iters) {
           struct rte_ring *r = makeRing("tp", static_cast<unsigned int>(cap));
           auto res = runTPSingle(
               cpu1, cpu2, iters,
               [&](int64_t i) {
                 int val;
                 while (rte_ring_sc_dequeue_elem(r, &val, sizeof(int)) != 0)
                   ;
                 if (val != i) {
                   throw std::runtime_error("");
                 }
               },
               [&](int64_t i) {
                 int val = static_cast<int>(i);
                 while (rte_ring_sp_enqueue_elem(r, &val, sizeof(int)) != 0)
                   ;
               });
           std::free(r);
           return res;
         },
         [&, makeRing] {
           struct rte_ring *r1 = makeRing("rtt1", ringCount);
           struct rte_ring *r2 = makeRing("rtt2", ringCount);
           auto res = runRTTSingle(
               cpu1, cpu2, rttIters,
               [&] {
                 int val;
                 while (rte_ring_sc_dequeue_elem(r1, &val, sizeof(int)) != 0)
                   ;
                 while (rte_ring_sp_enqueue_elem(r2, &val, sizeof(int)) != 0)
                   ;
               },
               [&](int64_t i) {
                 int val = static_cast<int>(i);
                 while (rte_ring_sp_enqueue_elem(r1, &val, sizeof(int)) != 0)
                   ;
                 while (rte_ring_sc_dequeue_elem(r2, &val, sizeof(int)) != 0)
                   ;
               });
           std::free(r1);
           std::free(r2);
           return res;
         },
         [&, makeRing] {
           return runColdSingle(
               cpu1, cpu2, coldQueues, coldRounds,
               [&] {
                 return std::unique_ptr<struct rte_ring, void (*)(void *)>(
                     makeRing("c", 8), std::free);
               },
               [](struct rte_ring &r, int expected) {
                 int val;
                 while (rte_ring_sc_dequeue_elem(&r, &val, sizeof(int)) != 0)
                   ;
                 if (val != expected) {
                   throw std::runtime_error("");
                 }
               },
               [](struct rte_ring &r, int val) {
                 while (rte_ring_sp_enqueue_elem(&r, &val, sizeof(int)) != 0)
                   ;
               });
         }});
  }
#endif

  // Balanced Latin square: each entry runs once in every position 0..n-1.
  const size_t n = entries.size();
  std::vector<size_t> baseOrder(n);
  for (size_t j = 0; j < n; ++j) {
    if (j == 0) {
      baseOrder[j] = 0;
    } else if (j % 2 == 1) {
      baseOrder[j] = (j + 1) / 2;
    } else {
      baseOrder[j] = n - j / 2;
    }
  }
  for (size_t r = 0; r < n; ++r) {
    for (size_t idx = 0; idx < n; ++idx) {
      size_t i = (baseOrder[idx] + r) % n;
      entries[i].tpDecoupledResults.push_back(
          entries[i].runTP(decoupledCap, decoupledIters));
      entries[i].tpBoundedResults.push_back(
          entries[i].runTP(boundedCap, boundedIters));
      entries[i].rttResults.push_back(entries[i].runRTT());
      entries[i].coldResults.push_back(entries[i].runCold());
    }
  }

  for (auto &entry : entries) {
    std::cout << entry.name << std::endl;
    printSummary(entry.tpDecoupledResults, "decoupled", "Mops/s", 1e-3, 1);
    printSummary(entry.tpBoundedResults, "bounded  ", "Mops/s", 1e-3, 1);
    printSummary(entry.rttResults, "latency  ", "ns RTT");
    printSummary(entry.coldResults, "cold     ", "ns RTT");
  }

  return 0;
}
