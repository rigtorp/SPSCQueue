/*
Copyright (c) 2020 Erik Rigtorp <erik@rigtorp.se>

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

#include <iostream>
#include <rigtorp/SPSCQueue.h>
#include <thread>

/**
 * This example demonstrates modern C++20 features with SPSCQueue:
 * - Concepts for template constraints
 * - [[likely]]/[[unlikely]] attributes for branch prediction hints
 * - requires clauses for cleaner template specialization
 * - Type trait _v suffix (std::is_constructible_v instead of ::value)
 * - Structured bindings (where applicable)
 */

struct Message {
  int id;
  const char *data;

  Message() : id(0), data("") {}
  Message(int id_, const char *data_) : id(id_), data(data_) {}
};

int main() {
  // Create a queue with capacity for 100 messages
  rigtorp::SPSCQueue<Message> queue(100);

  // Producer thread that sends messages
  std::thread producer([&queue]() {
    for (int i = 0; i < 10; ++i) {
      Message msg(i, "Hello from producer");
      // Using modern C++20 push with universal references
      // The template specialization now uses 'requires' clauses instead of
      // enable_if
      queue.push(msg);
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  });

  // Consumer thread that receives messages
  std::thread consumer([&queue]() {
    int consumed = 0;
    while (consumed < 10) {
      // Modern [[likely]] branch prediction hints are used internally
      // in front() and try_emplace() for better performance
      if (auto *msg = queue.front()) {
        std::cout << "Received message " << msg->id << ": " << msg->data
                  << std::endl;
        queue.pop();
        ++consumed;
      } else {
        // Queue is empty, wait a bit before retrying
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }
  });

  producer.join();
  consumer.join();

  std::cout << "\n=== C++20 Queue Size Operations ===" << std::endl;
  std::cout << "Queue empty: " << (queue.empty() ? "true" : "false")
            << std::endl;
  std::cout << "Queue size: " << queue.size() << std::endl;
  std::cout << "Queue capacity: " << queue.capacity() << std::endl;

  // Demonstrating try_emplace with modern C++20 features
  std::cout << "\n=== Try Emplace Examples ===" << std::endl;
  Message msg1(42, "Direct emplace test");
  if (queue.try_push(msg1)) {
    std::cout << "Successfully pushed message with ID 42" << std::endl;
  }

  if (auto *front_msg = queue.front()) {
    std::cout << "Front message ID: " << front_msg->id << std::endl;
    queue.pop();
  }

  // Fill queue to demonstrate [[unlikely]] branch in try_emplace
  std::cout << "\n=== Fill Queue Test ===" << std::endl;
  int pushed = 0;
  for (int i = 0; i < 150; ++i) {
    if (queue.try_push(Message(i, "Fill test"))) {
      ++pushed;
    }
  }
  std::cout << "Pushed " << pushed << " messages (capacity is "
            << queue.capacity() << ")" << std::endl;

  std::cout << "\nC++20 modernization complete!" << std::endl;

  return 0;
}
