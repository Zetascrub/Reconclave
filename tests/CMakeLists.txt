add_executable(reconclave_protocol_tests protocol_tests.cpp)
target_link_libraries(reconclave_protocol_tests PRIVATE reconclave_protocol)
target_compile_features(reconclave_protocol_tests PRIVATE cxx_std_17)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(reconclave_protocol_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

add_test(NAME reconclave_protocol_tests COMMAND reconclave_protocol_tests)

add_executable(reconclave_rf_history_tests rf_history_tests.cpp)
target_compile_features(reconclave_rf_history_tests PRIVATE cxx_std_17)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(reconclave_rf_history_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
add_test(NAME reconclave_rf_history_tests COMMAND reconclave_rf_history_tests)

if(UNIX)
add_executable(reconclave_cardputer_scan_tests
  cardputer_scan_tests.cpp
  ../devices/cardputer-adv/src/network_host_scan_service.cpp
  ../devices/cardputer-adv/src/network_port_scan_service.cpp)
target_include_directories(reconclave_cardputer_scan_tests PRIVATE cardputer_mocks)
target_compile_features(reconclave_cardputer_scan_tests PRIVATE cxx_std_17)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(reconclave_cardputer_scan_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
add_test(NAME reconclave_cardputer_scan_tests COMMAND reconclave_cardputer_scan_tests)
endif()

add_executable(reconclave_checked_file_tests checked_file_print_tests.cpp)
target_include_directories(reconclave_checked_file_tests PRIVATE cardputer_mocks)
target_compile_features(reconclave_checked_file_tests PRIVATE cxx_std_17)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(reconclave_checked_file_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
add_test(NAME reconclave_checked_file_tests COMMAND reconclave_checked_file_tests)

add_executable(reconclave_ndef_content_tests ndef_content_tests.cpp)
target_compile_features(reconclave_ndef_content_tests PRIVATE cxx_std_17)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(reconclave_ndef_content_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
add_test(NAME reconclave_ndef_content_tests COMMAND reconclave_ndef_content_tests)
