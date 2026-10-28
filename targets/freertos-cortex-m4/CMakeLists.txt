# Cross-build a standalone MPS2 Cortex-M4F firmware with the native interrupt port.
# Link board startup and runtime integration explicitly, then require the complete guest test suite.
cmake_minimum_required(VERSION 3.20)
project(rtx_freertos_cortex_m4 LANGUAGES C CXX ASM)
set(FREERTOS_KERNEL_PATH "" CACHE PATH "FreeRTOS-Kernel V11.2.0 checkout")
if(NOT EXISTS "${FREERTOS_KERNEL_PATH}/tasks.c")
 message(FATAL_ERROR "Set FREERTOS_KERNEL_PATH to a configured kernel source checkout")
endif()
add_library(freertos_config INTERFACE)
target_include_directories(freertos_config INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
set(FREERTOS_PORT GCC_ARM_CM4F CACHE STRING "FreeRTOS port")
set(FREERTOS_HEAP 3 CACHE STRING "FreeRTOS heap")
add_subdirectory(${FREERTOS_KERNEL_PATH} kernel)
include(../../cmake/RTXFreeRTOS.cmake)
rtx_add_freertos_library(rtx_cortex_m4 freertos_kernel)
add_executable(rtx_mps2 main.cpp board.cpp task_runtime.cpp startup.S)
target_link_libraries(rtx_mps2 PRIVATE rtx_cortex_m4)
target_link_options(rtx_mps2 PRIVATE --specs=rdimon.specs -nostartfiles
 -T${CMAKE_CURRENT_SOURCE_DIR}/mps2.ld -Wl,--gc-sections -Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/rtx_mps2.map)
set_target_properties(rtx_mps2 PROPERTIES LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/mps2.ld)
include(CTest)
find_program(RTX_QEMU_ARM qemu-system-arm)
if(BUILD_TESTING AND RTX_QEMU_ARM)
 add_test(NAME cortex_m4_execution COMMAND ${RTX_QEMU_ARM}
  -M mps2-an386 -cpu cortex-m4 -nographic -monitor none -serial none
  -semihosting-config enable=on,target=native -kernel $<TARGET_FILE:rtx_mps2>)
 set_tests_properties(cortex_m4_execution PROPERTIES TIMEOUT 20
  PASS_REGULAR_EXPRESSION "PASS: Cortex-M4 full execution suite"
  FAIL_REGULAR_EXPRESSION "FAIL:")
endif()
