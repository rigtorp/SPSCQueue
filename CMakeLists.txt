cmake_minimum_required(VERSION 3.24...4.3)

project(SPSCQueue
  VERSION 2.0.1
  HOMEPAGE_URL "https://github.com/rigtorp/SPSCQueue"
  LANGUAGES CXX
)

# -----------------------------------------------------------------------------
# Library Target
# -----------------------------------------------------------------------------
add_library(${PROJECT_NAME} INTERFACE)
add_library(${PROJECT_NAME}::${PROJECT_NAME} ALIAS ${PROJECT_NAME})

target_compile_features(${PROJECT_NAME} INTERFACE cxx_std_20)
target_sources(${PROJECT_NAME} INTERFACE
  FILE_SET HEADERS
  BASE_DIRS include
  FILES include/rigtorp/SPSCQueue.h
)

# -----------------------------------------------------------------------------
# Tests, Examples & Benchmarks (Top-Level Only)
# -----------------------------------------------------------------------------
include(CTest)

if(PROJECT_IS_TOP_LEVEL AND BUILD_TESTING)
  find_package(Threads REQUIRED)

  function(spscqueue_executable target source)
    add_executable(${target} ${source})
    target_link_libraries(${target} PRIVATE
      ${PROJECT_NAME}::${PROJECT_NAME}
      Threads::Threads
    )
    set_target_properties(${target} PROPERTIES
      CXX_EXTENSIONS OFF
      COMPILE_WARNING_AS_ERROR ON
    )
    target_compile_options(${target} PRIVATE
      "$<$<CXX_COMPILER_ID:MSVC>:/permissive-;/W4;/wd4324;/w14242>"
      "$<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra;-Wpedantic;-Wshadow;-Wconversion;-Wsign-conversion;-Wold-style-cast;-Wundef;-Wcast-align;-Wzero-as-null-pointer-constant;-Wformat=2>"
    )
  endfunction()

  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    spscqueue_executable(SPSCQueueBenchmark src/SPSCQueueBenchmark.cpp)
    find_library(RTE_RING_LIBRARY rte_ring)
    if(RTE_RING_LIBRARY)
      target_link_libraries(SPSCQueueBenchmark PRIVATE ${RTE_RING_LIBRARY})
    endif()
    spscqueue_executable(SPSCQueueExampleHugepages src/SPSCQueueExampleHugepages.cpp)
  endif()

  spscqueue_executable(SPSCQueueExample src/SPSCQueueExample.cpp)
  spscqueue_executable(SPSCQueueTest src/SPSCQueueTest.cpp)
  add_test(NAME SPSCQueueTest COMMAND SPSCQueueTest)

  option(${PROJECT_NAME}_COVERAGE "Check MC/DC coverage with gcov" OFF)
  if(${PROJECT_NAME}_COVERAGE)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    if(CMAKE_CXX_COMPILER MATCHES "g\\+\\+(-[0-9]+)?$")
      find_program(GCOV_EXECUTABLE NAMES "gcov${CMAKE_MATCH_1}" gcov REQUIRED)
    else()
      find_program(GCOV_EXECUTABLE NAMES gcov REQUIRED)
    endif()
    add_test(
      NAME SPSCQueueMCDC
      COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/src/check_mcdc.py"
        --gcno "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/SPSCQueueTest.dir/src/SPSCQueueTest.cpp.gcno"
        --gcov "${GCOV_EXECUTABLE}"
    )
    set_tests_properties(SPSCQueueMCDC PROPERTIES DEPENDS SPSCQueueTest)
  endif()
endif()

# -----------------------------------------------------------------------------
# Installation & Package Export
# -----------------------------------------------------------------------------
if(PROJECT_IS_TOP_LEVEL)
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

  install(TARGETS ${PROJECT_NAME} EXPORT ${PROJECT_NAME}Config FILE_SET HEADERS)

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
