# Select an x86-64 Linux cross compiler and user-mode emulator for host-independent checks.
# This Ubuntu/Debian sysroot is a validation aid, not a replacement for a deployment SDK.
# Ubuntu/Debian cross toolchain, runnable on an ARM Linux development host.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_CXX_COMPILER x86_64-linux-gnu-g++)
find_program(RTX_QEMU_X86_64 qemu-x86_64 REQUIRED)
set(CMAKE_CROSSCOMPILING_EMULATOR "${RTX_QEMU_X86_64};-L;/usr/x86_64-linux-gnu" CACHE STRING "Target emulator")
