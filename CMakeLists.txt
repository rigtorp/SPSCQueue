cmake_minimum_required(VERSION 3.6)

project(SPSCQueue LANGUAGES CXX)

add_library(${PROJECT_NAME} INTERFACE)
add_library(${PROJECT_NAME}::${PROJECT_NAME} ALIAS ${PROJECT_NAME})

target_compile_features(${PROJECT_NAME} INTERFACE cxx_std_11)

target_include_directories(SPSCQueue INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)

# Tests and examples
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    if(MSVC)
        add_compile_options(/permissive- /W4)
    else()
        add_compile_options(-Wall -Wextra -Wpedantic)
    endif()

    find_package(Threads REQUIRED)

    if (UNIX AND NOT APPLE)
        add_executable(SPSCQueueBenchmark src/SPSCQueueBenchmark.cpp)
        target_link_libraries(SPSCQueueBenchmark SPSCQueue Threads::Threads)
    endif()

    if (((CMAKE_CXX_COMPILER_ID MATCHES "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 7.0)
        OR (CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 9.0))
        AND UNIX AND NOT APPLE)
        add_executable(SPSCQueueExampleHugepages src/SPSCQueueExampleHugepages.cpp)
        target_link_libraries(SPSCQueueExampleHugepages SPSCQueue Threads::Threads)
        target_compile_features(SPSCQueueExampleHugepages PRIVATE cxx_std_17)
    endif()

    add_executable(SPSCQueueExample src/SPSCQueueExample.cpp)
    target_link_libraries(SPSCQueueExample SPSCQueue Threads::Threads)

    add_executable(SPSCQueueTest src/SPSCQueueTest.cpp)
    target_link_libraries(SPSCQueueTest SPSCQueue Threads::Threads)

    enable_testing()
    add_test(SPSCQueueTest SPSCQueueTest)
endif()