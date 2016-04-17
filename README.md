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

Only a single writer thread can perform enqueue operations and only a
single reader thread can perform dequeue operations. Any other usage
is invalid.

### Template parameters

- `T`. Type of element the queue will hold.

### Member funtions

```cpp
SPSCQueue<T>(size_t capacity);
```

Create a `SPSCqueue` holding items of type `T` with capacity
`capacity`. Capacity need to be greater than 2.

```cpp
void emplace(Args &&... args);
```

Enqueue an item using inplace construction. Blocks if queue is full.

```cpp
bool try_emplace(Args &&... args);
```

Try to enqueue an item using inplace construction. Returns `true` on
success and `false` if queue is full.

```cpp
T *front();
```

Return pointer to front of queue. Returns `nullptr` if queue is
empty.

```cpp
pop();
```

Dequeue first elment of queue. Invalid to call if queue is empty.

## Implementation

The underlying implementation is a
[ring buffer](https://en.wikipedia.org/wiki/Circular_buffer). 

Care has been taken to make sure to avoid any issues with
[false sharing](https://en.wikipedia.org/wiki/False_sharing). The head
and tail pointers are aligned and padded to the false sharing range
(currently hard coded to 128 bytes). The slots buffer is padded with
the false sharing range at the beginning and end. See memory layout:

![Memory layout](https://github.com/rigtorp/SPSCQueue/blob/master/spsc.png)

References:

- *Intel*. [Avoiding and Identifying False Sharing Among Threads](https://software.intel.com/en-us/articles/avoiding-and-identifying-false-sharing-among-threads).
- *Wikipedia*. [Ring buffer](https://en.wikipedia.org/wiki/Circular_buffer).
- *Wikipedia*. [False sharing](https://en.wikipedia.org/wiki/False_sharing).

## About

This project was created by [Erik Rigtorp](http://rigtorp.se)
<[erik@rigtorp.se](mailto:erik@rigtorp.se)>.
