cmake_minimum_required(VERSION 3.24)
project(lob VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Debug builds are sanitizer builds unless explicitly turned off.
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  set(_lob_sanitize_default ON)
else()
  set(_lob_sanitize_default OFF)
endif()
option(LOB_SANITIZE "Build everything with AddressSanitizer + UBSan" ${_lob_sanitize_default})
option(LOB_NATIVE "Tune Release/RelWithDebInfo for the host CPU (-march=native)" ON)
option(LOB_WERROR "Treat warnings as errors" OFF)
option(LOB_BUILD_TESTS "Build unit tests" ON)
option(LOB_BUILD_BENCH "Build benchmarks" ON)

include(CheckCXXCompilerFlag)

# -O1 keeps sanitizer builds fast enough for million-operation randomized tests.
set(CMAKE_CXX_FLAGS_DEBUG "-O1 -g")
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-O3 -g -fno-omit-frame-pointer -DNDEBUG")
if(LOB_NATIVE)
  check_cxx_compiler_flag("-march=native" LOB_HAS_MARCH_NATIVE)
  if(LOB_HAS_MARCH_NATIVE)
    string(APPEND CMAKE_CXX_FLAGS_RELEASE " -march=native")
    string(APPEND CMAKE_CXX_FLAGS_RELWITHDEBINFO " -march=native")
  endif()
endif()

if(LOB_SANITIZE)
  # Applied globally so GoogleTest/Benchmark are instrumented consistently.
  add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

# Warnings apply to our own targets only, not third-party code.
add_library(lob_warnings INTERFACE)
target_compile_options(lob_warnings INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Wold-style-cast)
if(LOB_WERROR)
  target_compile_options(lob_warnings INTERFACE -Werror)
endif()

find_package(ZLIB REQUIRED)

add_library(lob
  src/events.cpp
  src/map_order_book.cpp
  src/itch/gz_reader.cpp
)
target_include_directories(lob PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(lob PUBLIC ZLIB::ZLIB PRIVATE lob_warnings)

add_executable(lob_replay tools/replay_main.cpp)
target_link_libraries(lob_replay PRIVATE lob lob_warnings)

include(FetchContent)
set(FETCHCONTENT_QUIET ON)

if(LOB_BUILD_TESTS)
  FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  enable_testing()
  add_executable(lob_tests
    tests/test_order_book.cpp
    tests/test_matching_engine.cpp
    tests/test_randomized.cpp
    tests/test_itch.cpp
    tests/test_replay.cpp
    tests/test_histogram.cpp
  )
  target_include_directories(lob_tests PRIVATE tests)
  target_link_libraries(lob_tests PRIVATE lob lob_warnings GTest::gtest_main)
  include(GoogleTest)
  gtest_discover_tests(lob_tests DISCOVERY_MODE PRE_TEST)
endif()

if(LOB_BUILD_BENCH)
  FetchContent_Declare(benchmark
    URL https://github.com/google/benchmark/archive/refs/tags/v1.9.1.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_INSTALL_DOCS OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(benchmark)

  add_executable(lob_bench bench/bench_book.cpp)
  target_include_directories(lob_bench PRIVATE bench)
  target_link_libraries(lob_bench PRIVATE lob lob_warnings benchmark::benchmark_main)

  add_executable(lob_latency bench/latency_main.cpp)
  target_include_directories(lob_latency PRIVATE bench)
  target_link_libraries(lob_latency PRIVATE lob lob_warnings)
endif()
