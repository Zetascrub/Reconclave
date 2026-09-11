cmake_minimum_required(VERSION 3.20)
project(reconclave VERSION 0.1.0 LANGUAGES CXX)

option(RECONCLAVE_BUILD_TESTS "Build native protocol tests" ON)
option(RECONCLAVE_BUILD_K230 "Build the K230 native app (requires a riscv64 cross toolchain, see devices/k230/toolchain-riscv64-linux-gnu.cmake)" OFF)

add_subdirectory(common/protocol)

if(RECONCLAVE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(RECONCLAVE_BUILD_K230)
  add_subdirectory(devices/k230)
endif()
