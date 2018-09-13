cmake_minimum_required(VERSION 3.2)
project(SPSCQueue CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS "true")
set(CMAKE_CXX_STANDARD 11)

add_library(SPSCQueue INTERFACE)
target_include_directories(SPSCQueue INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)
target_compile_options(SPSCQueue INTERFACE -Wall -Wextra -Wpedantic -Werror)

find_package(Threads REQUIRED)

add_executable(SPSCQueueBenchmark src/SPSCQueueBenchmark.cpp)
target_link_libraries(SPSCQueueBenchmark SPSCQueue Threads::Threads)

add_executable(SPSCQueueExample src/SPSCQueueExample.cpp)
target_link_libraries(SPSCQueueExample SPSCQueue Threads::Threads)

add_executable(SPSCQueueTest src/SPSCQueueTest.cpp)
target_link_libraries(SPSCQueueTest SPSCQueue Threads::Threads)

enable_testing()
add_test(SPSCQueueTest SPSCQueueTest)
