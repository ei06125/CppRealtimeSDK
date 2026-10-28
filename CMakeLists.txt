# Mirrors the Bazel build; both are supported, CMake is the one to use with Yocto.
cmake_minimum_required(VERSION 3.16)
project(rtsdk VERSION 0.1.0 LANGUAGES CXX)

# Build configurations (selected by CMakePresets.json):
#   Debug        -O0 -g, assertions on: stepping and sanitizer runs
#   Development  = RelWithDebInfo, -O2 -g with assertions kept: realistic timing plus symbols
#                  (daily work, CI perf budget). Stock RelWithDebInfo is used on purpose so
#                  imported packages (gRPC, protobuf) resolve without a custom config.
#   Release      -O3, NDEBUG: what ships to the ECU
if(CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_CONFIGURATION_TYPES "Debug;RelWithDebInfo;Release" CACHE STRING "" FORCE)
elseif(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Debug, RelWithDebInfo (Development) or Release" FORCE)
endif()
if(NOT MSVC)
  set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-O2 -g")  # unlike the default, keep assert() active
endif()

option(RTSDK_BUILD_TESTS     "Build unit tests"                         ON)
option(RTSDK_BUILD_BENCH     "Build benchmarks"                         ON)
option(RTSDK_BUILD_EXAMPLES  "Build examples"                           ON)
option(RTSDK_WITH_POSIX      "POSIX/Yocto backend (pthreads, shm_open)" ON)
option(RTSDK_WITH_FREERTOS   "FreeRTOS backend (needs RTSDK_FREERTOS_INCLUDE_DIRS)" OFF)
option(RTSDK_ENABLE_GRPC     "gRPC control plane (needs gRPC + protobuf)" OFF)
option(RTSDK_WARNINGS_AS_ERRORS "Treat warnings as errors"              ON)
set(RTSDK_SANITIZE "" CACHE STRING "Sanitizers, e.g. address,undefined or thread")

# Shared warning/sanitizer flags, applied to every target via linkage.
add_library(rtsdk_options INTERFACE)
if(MSVC)
  target_compile_options(rtsdk_options INTERFACE /W4)
  if(RTSDK_WARNINGS_AS_ERRORS)
    target_compile_options(rtsdk_options INTERFACE /WX)
  endif()
else()
  target_compile_options(rtsdk_options INTERFACE -Wall -Wextra -Wpedantic)
  if(RTSDK_WARNINGS_AS_ERRORS)
    target_compile_options(rtsdk_options INTERFACE -Werror)
  endif()
endif()
if(RTSDK_SANITIZE)
  target_compile_options(rtsdk_options INTERFACE -fsanitize=${RTSDK_SANITIZE} -fno-omit-frame-pointer)
  target_link_options(rtsdk_options INTERFACE -fsanitize=${RTSDK_SANITIZE})
endif()

# Platform-agnostic core; backends are appended below by option.
set(RTSDK_CORE_SRC
  src/comms.cpp src/executor.cpp src/heartbeat.cpp src/launch.cpp
  src/manager.cpp src/runner.cpp src/watchdog.cpp)

if(RTSDK_WITH_POSIX)
  list(APPEND RTSDK_CORE_SRC src/platform/posix/posix_platform.cpp)
endif()
if(RTSDK_WITH_FREERTOS)
  list(APPEND RTSDK_CORE_SRC src/platform/freertos/freertos_platform.cpp)
endif()

add_library(rtsdk ${RTSDK_CORE_SRC})
add_library(rtsdk::rtsdk ALIAS rtsdk)
target_include_directories(rtsdk PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                                          $<INSTALL_INTERFACE:include>)
target_link_libraries(rtsdk PRIVATE rtsdk_options)
if(RTSDK_WITH_FREERTOS)
  target_include_directories(rtsdk PRIVATE ${RTSDK_FREERTOS_INCLUDE_DIRS})
endif()
if(RTSDK_WITH_POSIX)
  find_package(Threads REQUIRED)
  target_link_libraries(rtsdk PUBLIC Threads::Threads)
  if(NOT APPLE)
    target_link_libraries(rtsdk PUBLIC rt)
  endif()
endif()

if(RTSDK_ENABLE_GRPC)
  # Prefer upstream CMake packages (vcpkg/Conan/source builds); fall back to
  # distro packages (Ubuntu/Debian/Yocto) that only ship FindProtobuf + pkg-config.
  find_package(Protobuf CONFIG QUIET)
  if(NOT Protobuf_FOUND)
    find_package(Protobuf REQUIRED)
  endif()
  find_package(gRPC CONFIG QUIET)
  if(gRPC_FOUND)
    set(_grpc_lib gRPC::grpc++)
    set(_grpc_plugin "$<TARGET_FILE:gRPC::grpc_cpp_plugin>")
  else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GRPCPP REQUIRED IMPORTED_TARGET grpc++)
    find_program(_grpc_plugin grpc_cpp_plugin REQUIRED)
    set(_grpc_lib PkgConfig::GRPCPP)
  endif()
  add_library(rtsdk_grpc src/grpc/control_service.cpp proto/execution_control.proto)
  target_include_directories(rtsdk_grpc PUBLIC ${CMAKE_CURRENT_BINARY_DIR} src/grpc)
  target_link_libraries(rtsdk_grpc PUBLIC rtsdk ${_grpc_lib} protobuf::libprotobuf)
  set(_pd ${CMAKE_CURRENT_SOURCE_DIR}/proto)
  protobuf_generate(TARGET rtsdk_grpc LANGUAGE cpp IMPORT_DIRS ${_pd})
  protobuf_generate(TARGET rtsdk_grpc LANGUAGE grpc GENERATE_EXTENSIONS .grpc.pb.h .grpc.pb.cc IMPORT_DIRS ${_pd}
                    PLUGIN "protoc-gen-grpc=${_grpc_plugin}")
endif()

if(RTSDK_BUILD_TESTS)
  enable_testing()
  file(GLOB RTSDK_TEST_SRC tests/*.cpp)
  add_executable(rtsdk_tests ${RTSDK_TEST_SRC})
  target_link_libraries(rtsdk_tests PRIVATE rtsdk rtsdk_options)
  add_test(NAME rtsdk_tests COMMAND rtsdk_tests)
  # Compile-check the FreeRTOS backend against declaration stubs (no kernel needed).
  add_library(rtsdk_freertos_syntax OBJECT src/platform/freertos/freertos_platform.cpp)
  target_include_directories(rtsdk_freertos_syntax PRIVATE include tests/stubs)
  target_link_libraries(rtsdk_freertos_syntax PRIVATE rtsdk_options)
endif()

if(RTSDK_BUILD_BENCH)
  add_executable(rtsdk_bench bench/bench.cpp)
  target_link_libraries(rtsdk_bench PRIVATE rtsdk rtsdk_options)
  add_test(NAME rtsdk_perf COMMAND rtsdk_bench --check)
endif()

if(RTSDK_BUILD_EXAMPLES)
  add_executable(example_adas examples/adas_demo.cpp)
  target_link_libraries(example_adas PRIVATE rtsdk rtsdk_options)
endif()

if(RTSDK_ENABLE_GRPC AND RTSDK_BUILD_TESTS)
  add_executable(rtsdk_grpc_tests tests_grpc/test_grpc.cpp tests/main.cpp)
  target_link_libraries(rtsdk_grpc_tests PRIVATE rtsdk_grpc rtsdk_options)
  add_test(NAME rtsdk_grpc_tests COMMAND rtsdk_grpc_tests)
endif()
