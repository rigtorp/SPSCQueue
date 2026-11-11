// SPDX-FileCopyrightText: 2018 Erik Rigtorp <erik@rigtorp.se>
// SPDX-License-Identifier: MIT

#include <iostream>
#include <rigtorp/SPSCQueue.h>
#include <thread>

int main(int argc, char *argv[]) {
  (void)argc, (void)argv;

  using namespace rigtorp;

  SPSCQueue<int> q(1);
  auto t = std::thread([&] {
    while (!q.front())
      ;
    std::cout << *q.front() << std::endl;
    q.pop();
  });
  q.push(1);
  t.join();

  return 0;
}
