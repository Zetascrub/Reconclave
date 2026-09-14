add_executable(reconclave_protocol_tests protocol_tests.cpp)
target_link_libraries(reconclave_protocol_tests PRIVATE reconclave_protocol)
target_compile_features(reconclave_protocol_tests PRIVATE cxx_std_17)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(reconclave_protocol_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

add_test(NAME reconclave_protocol_tests COMMAND reconclave_protocol_tests)
