<!-- Keep supported presets aligned with platform backends and CI coverage. -->

# Build presets and development environments

Presets require CMake 3.23 or newer, Ninja, and a C++20 compiler. Direct CMake
configuration still supports the project's existing CMake 3.20 minimum.
Each preset has a separate build directory, configure step, build step, and test
step. `development` maps to `RelWithDebInfo` (optimized code with debug symbols);
`debug` and `release` map to CMake's `Debug` and `Release` configurations.

| Host / target | Preset prefix | Components / prerequisites |
| --- | --- | --- |
| Linux | `linux` | Core and Linux runtime; GCC or Clang, pthreads |
| macOS | `macos` | Portable core; Xcode command line tools, Ninja |
| Windows | `windows` | Portable core; Visual Studio 2022 C++ tools and Ninja |
| Linux FreeRTOS simulator | `freertos-sim` | Real GCC POSIX kernel port; GCC, external FreeRTOS checkout |
| Cortex-M4 FreeRTOS firmware | `cortex-m4` | GNU Arm Embedded, newlib C++, external FreeRTOS checkout; QEMU for tests |

For Windows, run from an x64 Native Tools command prompt (or an equivalent MSVC
developer shell). Linux-only execution and shared-memory APIs are disabled in
the macOS and Windows presets. gRPC is off by default so hosted presets need no
third-party packages; on Linux, install protobuf/gRPC and configure with
`-DRTX_GRPC=ON` to include the management services.

From the repository root, substitute your platform and desired configuration:

```sh
cmake --list-presets
cmake --preset linux-development
cmake --build --preset linux-development --parallel 4
ctest --preset linux-development
```

Use `macos-debug`, `windows-release`, etc. on those hosts. The Linux FIFO test can
skip when scheduling privileges are unavailable. Hosted OS scheduling is not a
hard realtime guarantee.

## Embedded presets

Embedded reference apps have their own source trees and preset files. Set
`FREERTOS_KERNEL_PATH` to a FreeRTOS-Kernel V11.2.0 checkout (tested commit
`0adc196d4bd52a2d91102b525b0aafc1e14a2386`). No kernel download occurs during configure.

```sh
export FREERTOS_KERNEL_PATH=/absolute/path/to/FreeRTOS-Kernel
(cd targets/freertos-sim &&
 cmake --preset freertos-sim-development &&
 cmake --build --preset freertos-sim-development --parallel 4 &&
 ctest --preset freertos-sim-development)
(cd targets/freertos-cortex-m4 &&
 cmake --preset cortex-m4-debug &&
 cmake --build --preset cortex-m4-debug --parallel 4 &&
 ctest --preset cortex-m4-debug)
```

Both targets also provide Debug, Development and Release presets. Cortex-M4
uses the existing MPS2 reference toolchain, not a generic board support package.
Install `qemu-system-arm` to register its execution test; test presets fail if
no tests are registered. On Windows, set the kernel environment variable in the
developer shell and run equivalent commands from the target directory. The GNU
Arm exception-runtime integration remains subject to its documented ABI limits.
See the [Cortex-M4 notes](../targets/freertos-cortex-m4/README.md).

Yocto builds use BitBake's SDK/toolchain and recipes, rather than a host preset;
see [Yocto instructions](../yocto/README.md). Other RTOSes require a dedicated
backend and board integration before meaningful presets can be supplied.

## Docker and VS Code

Build and enter the Linux environment with a mounted checkout:

```sh
docker build -t rtx-dev .
docker run --rm -it -v "$PWD:/workspace" rtx-dev
# Inside the container:
cmake --preset linux-development
cmake --build --preset linux-development --parallel 4
ctest --preset linux-development
```

Docker runs the Linux presets even on macOS or Windows hosts. Use a separate
checkout if also building on the host: build caches contain environment-specific
paths. Containers run as root by default, so bind-mounted outputs may be owned by
root on Linux; pass `--user "$(id -u):$(id -g)"` if desired.
For Cortex-M4 tooling, build with `--build-arg RTX_EMBEDDED=1`; mount your kernel
checkout and set `FREERTOS_KERNEL_PATH` to its path inside the container.
The default image does not grant FIFO scheduling privileges.

In VS Code, choose **Dev Containers: Reopen in Container**. The supplied
`.devcontainer/devcontainer.json` uses the same Dockerfile and CMake presets.
Set its `RTX_EMBEDDED` build argument to `1` if you need the firmware toolchain.

Keep machine-specific compiler paths and optional settings in an ignored
`CMakeUserPresets.json` inheriting one of the supplied presets.
