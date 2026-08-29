add_library(reconclave_protocol STATIC
  src/protocol.cpp
  src/node_registry.cpp
)

target_include_directories(reconclave_protocol PUBLIC include)
target_compile_features(reconclave_protocol PUBLIC cxx_std_17)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(reconclave_protocol PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
