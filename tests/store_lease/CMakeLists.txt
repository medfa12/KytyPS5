cmake_minimum_required(VERSION 3.21)
project(StoreLeaseTests LANGUAGES CXX)
enable_testing()
add_library(lease_component STATIC
  ../../src/loader/runtimeChildProtocol.cpp
  ../../src/loader/runtimeChildInput.cpp
  ../../src/loader/runtimeChildFrames.cpp
  ../../src/loader/runtimeChildClient.cpp
  ../../src/loader/runtimeChildProcess.cpp)
target_include_directories(lease_component PUBLIC ../../src)
target_compile_features(lease_component PUBLIC cxx_std_20)
target_compile_definitions(lease_component PUBLIC KYTY_SUBPROCESS_TESTS NOMINMAX)
add_executable(store_lease_tests lease_tests.cpp)
target_link_libraries(store_lease_tests PRIVATE lease_component)
add_test(NAME store_lease_lifecycle COMMAND store_lease_tests)
set_tests_properties(store_lease_lifecycle PROPERTIES TIMEOUT 60)
