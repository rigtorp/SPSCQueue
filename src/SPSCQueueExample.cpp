// SPDX-FileCopyrightText: 2018 Erik Rigtorp <erik@rigtorp.se>
// SPDX-License-Identifier: MIT

#include <iostream>
#include <rigtorp/SPSCQueue.h>
#include <thread>

int main() {
  rigtorp::SPSCQueue<int> queue(16);
  constexpr int count = 100;

  auto consumer = std::thread([&] {
    int sum = 0;
    for (int i = 0; i < count; ++i) {
      int value;
      while (!queue.try_pop(value)) {
        std::this_thread::yield();
      }
      sum += value;
    }
    std::cout << sum << '\n';
  });

  for (int i = 1; i <= count; ++i) {
    queue.push(i);
  }

  consumer.join();

  return 0;
}
