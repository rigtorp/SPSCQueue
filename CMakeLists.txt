cmake_minimum_required(VERSION 2.8)
project(SPSCQueue)

set(CMAKE_EXPORT_COMPILE_COMMANDS "true")
set(CMAKE_CXX_FLAGS "-std=c++11 -Wall")

link_libraries(pthread rt)

add_executable(SPSCQueueBenchmark SPSCQueueBenchmark.cpp)
add_executable(SPSCQueueExample SPSCQueueExample.cpp)
add_executable(SPSCQueueTest SPSCQueueTest.cpp)

enable_testing()
add_test(SPSCQueueTest SPSCQueueTest)
