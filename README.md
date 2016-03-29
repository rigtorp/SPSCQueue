# SPSCQueue.h

A single producer single consumer wait-free and lock-free fixed size
queue written in C++11.

## Example

```C++
rigtorp::SPSCQueue<int> q(2);
std::thread t([&] {
  while (!q.front());
  std::cout << *q.front() << std::endl;
  q.pop();
});
q.push(1);
```
