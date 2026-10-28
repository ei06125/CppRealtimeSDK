# Bare-metal Arm Cortex-M cross toolchain for the FreeRTOS backend.
# Use through the freertos-arm-* presets; override the CPU with -DRTSDK_ARM_CPU=cortex-m7 etc.
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(CMAKE_C_COMPILER arm-none-eabi-gcc)
set(CMAKE_CXX_COMPILER arm-none-eabi-g++)

set(RTSDK_ARM_CPU "cortex-m4" CACHE STRING "-mcpu value")
set(_arch "-mcpu=${RTSDK_ARM_CPU} -mthumb -mfloat-abi=hard -mfpu=auto")
set(CMAKE_C_FLAGS_INIT "${_arch} -ffunction-sections -fdata-sections")
set(CMAKE_CXX_FLAGS_INIT "${_arch} -ffunction-sections -fdata-sections -fno-exceptions -fno-rtti")

# No host executables can run, so only build static libraries during compiler checks.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
