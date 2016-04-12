# SPSCQueue.h

[![Build Status](https://travis-ci.org/rigtorp/SPSCQueue.svg?branch=master)](https://travis-ci.org/rigtorp/SPSCQueue)
[![License](https://img.shields.io/badge/license-MIT-blue.svg)](https://raw.githubusercontent.com/rigtorp/SPSCQueue/master/LICENSE)

A single producer single consumer wait-free and lock-free fixed size
queue written in C++11.

## Example

```C++
SPSCQueue<int> q(2);
auto t = std::thread([&] {
  while (!q.front());
  std::cout << *q.front() << std::endl;
  q.pop();
});
q.push(1);
t.join();
```

## Usage

Create a `SPSCqueue` holding items of type `T` with capacity `capacity`:

```cpp
SPSCQueue<T>(size_t capacity);
```

Enqueue an item using inplace construction, blocks if queue is full:

```cpp
void emplace(Args &&... args);
```

Try to enqueue an item using inplace construction, returns `false` if
queue is full:

```cpp
bool try_emplace(Args &&... args);
```

Return pointer to front of queue, returns `nullptr` if queue is empty:

```cpp
T *front();
```

Dequeue first elment of queue, invalid to call of queue is empty:

```cpp
Pop();
```

## About

This project was created by [Erik Rigtorp](http://rigtorp.se)
<[erik@rigtorp.se](mailto:erik@rigtorp.se)>.
