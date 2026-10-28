# Reproducible Linux tooling for the native presets and FreeRTOS POSIX simulator.
FROM ubuntu:24.04
ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential ca-certificates clang-format-18 cmake git ninja-build \
    python3 gdb libgrpc++-dev libprotobuf-dev protobuf-compiler-grpc \
    && rm -rf /var/lib/apt/lists/*

# Opt in to the larger GNU Arm/newlib and QEMU packages for Cortex-M4 firmware.
ARG RTX_EMBEDDED=0
RUN if [ "$RTX_EMBEDDED" = 1 ]; then \
    apt-get update && apt-get install -y --no-install-recommends \
    gcc-arm-none-eabi libstdc++-arm-none-eabi-newlib qemu-system-arm \
    && rm -rf /var/lib/apt/lists/*; fi
WORKDIR /workspace
CMD ["bash"]
