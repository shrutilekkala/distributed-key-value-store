cmake_minimum_required(VERSION 3.16)
project(cppkv CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CXX_FLAGS_RELEASE "-O2")
add_compile_options(-Wall -Wextra -pthread)
add_link_options(-pthread)

include_directories(include)

# ---- core library (store + threadpool + protocol) ----
add_library(cppkv_core
  src/kv_store.cpp
  src/threadpool.cpp
  src/protocol.cpp
)

# ---- server binary ----
add_executable(cppkv_server
  src/server.cpp
  src/main.cpp
)
target_link_libraries(cppkv_server PRIVATE cppkv_core pthread)

# ---- CLI client binary ----
add_executable(cppkv_client
  client/client.cpp
)
target_link_libraries(cppkv_client PRIVATE pthread)

# ---- benchmark binary ----
add_executable(cppkv_bench
  benchmark/benchmark.cpp
)
target_link_libraries(cppkv_bench PRIVATE pthread)

# ---- tests ----
enable_testing()
add_executable(cppkv_tests
  tests/test_kv_store.cpp
)
target_link_libraries(cppkv_tests PRIVATE cppkv_core pthread)
add_test(NAME cppkv_tests COMMAND cppkv_tests)
