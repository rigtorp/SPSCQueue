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

## About

This project was created by [Erik Rigtorp](http://rigtorp.se)
<[erik@rigtorp.se](mailto:erik@rigtorp.se)>.
