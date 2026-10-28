# Minimal external CMake consumer used by the package relocation check.
# Select feature checks from targets present in the installed package, not from local build assumptions.
cmake_minimum_required(VERSION 3.20)
project(rtx_consumer LANGUAGES CXX)
find_package(rtx 0.1 CONFIG REQUIRED COMPONENTS core)
add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE rtx::core)
if(TARGET rtx::posix)
 target_link_libraries(consumer PRIVATE rtx::posix)
 target_compile_definitions(consumer PRIVATE HAS_POSIX)
endif()
if(TARGET rtx::grpc)
 target_link_libraries(consumer PRIVATE rtx::grpc)
 target_compile_definitions(consumer PRIVATE HAS_GRPC)
endif()

# CTest resolves platform suffixes, configuration subdirectories and cross emulators.
enable_testing()
add_test(NAME consumer_execution COMMAND consumer)
