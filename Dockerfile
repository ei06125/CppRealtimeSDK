# Reproducible Linux build/test environment (also used by .devcontainer).
#   docker build -t rtsdk-dev .
#   docker run --rm -v "$PWD":/work -w /work --cap-add=SYS_NICE --ulimit rtprio=99 rtsdk-dev \
#       sh -c 'cmake --preset linux-grpc && cmake --build --preset linux-grpc && ctest --preset linux-grpc'
# SYS_NICE / rtprio let the POSIX backend use SCHED_FIFO; without them it falls back to SCHED_OTHER.
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build pkg-config git ca-certificates \
        clang-format gdb valgrind \
        libgrpc++-dev libprotobuf-dev protobuf-compiler-grpc \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /work
CMD ["bash"]
