cmake_minimum_required(VERSION 3.20)
project(reconclave VERSION 0.1.0 LANGUAGES CXX)

option(RECONCLAVE_BUILD_TESTS "Build native protocol tests" ON)
add_subdirectory(common/protocol)
add_subdirectory(common/identity)

if(RECONCLAVE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
