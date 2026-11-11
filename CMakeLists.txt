cmake_minimum_required(VERSION 3.21...3.30)

project(SPSCQueue
  VERSION 1.1
  HOMEPAGE_URL "https://github.com/rigtorp/SPSCQueue"
  LANGUAGES CXX
)

# -----------------------------------------------------------------------------
# Library Target
# -----------------------------------------------------------------------------
add_library(${PROJECT_NAME} INTERFACE)
add_library(${PROJECT_NAME}::${PROJECT_NAME} ALIAS ${PROJECT_NAME})

target_compile_features(${PROJECT_NAME} INTERFACE cxx_std_11)

target_include_directories(${PROJECT_NAME} INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

# -----------------------------------------------------------------------------
# Tests, Examples & Benchmarks (Top-Level Only)
# -----------------------------------------------------------------------------
include(CTest)

if(PROJECT_IS_TOP_LEVEL AND BUILD_TESTING)
  option(${PROJECT_NAME}_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

  add_library(${PROJECT_NAME}_warnings INTERFACE)
  if(MSVC)
    target_compile_options(${PROJECT_NAME}_warnings INTERFACE
      /permissive-
      /W4
      /wd4324 # structure was padded due to alignment specifier
    )
    if(${PROJECT_NAME}_WARNINGS_AS_ERRORS)
      target_compile_options(${PROJECT_NAME}_warnings INTERFACE /WX)
    endif()
  else()
    target_compile_options(${PROJECT_NAME}_warnings INTERFACE
      -Wall
      -Wextra
      -Wpedantic
      -Wno-c++17-extensions
    )
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 12.0)
      target_compile_options(${PROJECT_NAME}_warnings INTERFACE -Wno-interference-size)
    endif()
    if(${PROJECT_NAME}_WARNINGS_AS_ERRORS)
      target_compile_options(${PROJECT_NAME}_warnings INTERFACE -Werror)
    endif()
  endif()

  find_package(Threads REQUIRED)

  if(UNIX AND NOT APPLE)
    add_executable(SPSCQueueBenchmark src/SPSCQueueBenchmark.cpp)
    target_link_libraries(SPSCQueueBenchmark PRIVATE
      ${PROJECT_NAME}
      ${PROJECT_NAME}_warnings
      Threads::Threads
    )

    add_executable(SPSCQueueExampleHugepages src/SPSCQueueExampleHugepages.cpp)
    target_link_libraries(SPSCQueueExampleHugepages PRIVATE
      ${PROJECT_NAME}
      ${PROJECT_NAME}_warnings
      Threads::Threads
    )
    target_compile_features(SPSCQueueExampleHugepages PRIVATE cxx_std_17)
  endif()

  add_executable(SPSCQueueExample src/SPSCQueueExample.cpp)
  target_link_libraries(SPSCQueueExample PRIVATE
    ${PROJECT_NAME}
    ${PROJECT_NAME}_warnings
    Threads::Threads
  )

  add_executable(SPSCQueueTest src/SPSCQueueTest.cpp)
  target_link_libraries(SPSCQueueTest PRIVATE
    ${PROJECT_NAME}
    ${PROJECT_NAME}_warnings
    Threads::Threads
  )

  add_test(NAME SPSCQueueTest COMMAND SPSCQueueTest)
endif()

# -----------------------------------------------------------------------------
# Installation & Package Export
# -----------------------------------------------------------------------------
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

export(
  TARGETS ${PROJECT_NAME}
  NAMESPACE ${PROJECT_NAME}::
  FILE "${PROJECT_NAME}Config.cmake"
)

write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}ConfigVersion.cmake"
  COMPATIBILITY SameMajorVersion
  ARCH_INDEPENDENT
)

if(PROJECT_IS_TOP_LEVEL)
  install(
    DIRECTORY include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  )

  install(
    TARGETS ${PROJECT_NAME}
    EXPORT ${PROJECT_NAME}Config
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  )

  install(
    EXPORT ${PROJECT_NAME}Config
    NAMESPACE ${PROJECT_NAME}::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}"
  )

  install(
    FILES "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}ConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}"
  )
endif()
