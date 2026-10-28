<!-- Maintenance: distinguish recipe/image metadata checks from actual target compilation, boot and OEQA execution. -->

# Yocto integration

`meta-rtx` is a development layer using `externalsrc`. It builds the local SDK
checkout without editing the adjacent read-only reference repositories. This is
a development recipe. BitBake parsing and the default dependency/task graph
have been checked with Poky walnascar; target compilation and image execution
remain unverified.
The layer lists recent OE-Core release series as intended compatibility targets.
Use a distribution toolchain with C++20, pthreads and Linux robust mutex support.

Add the layer from an initialized Yocto build environment:

```sh
bitbake-layers add-layer /absolute/path/CppRealtimeSDK-Codex/yocto/meta-rtx
```

In `conf/local.conf`, set:

```bitbake
RTX_SOURCE_DIR = "/absolute/path/CppRealtimeSDK-Codex"
# Optional; requires grpc recipes, commonly supplied by meta-openembedded/meta-oe.
# PACKAGECONFIG:append:pn-rtx = " grpc"
```

Then build `bitbake rtx`. The recipe requests versioned shared libraries, allowing standard runtime and
development package splitting. Native shared-library build/install tests pass;
BitBake package QA execution and target image execution remain unverified. For a target development image, add `rtx rtx-dev` to IMAGE_INSTALL.
The recipe intentionally uses `LICENSE = "CLOSED"` because this checkout has no
project license grant. Choose the project's license and metadata before release.
This does not relicense the source. Examples are excluded. Tests are built and packaged when the distro enables ptest.

With gRPC enabled, target grpc/protobuf libraries and native grpc/protobuf tools
are dependencies. CMake accepts `RTX_PROTOC_EXECUTABLE` and
`RTX_GRPC_PLUGIN_EXECUTABLE`; the recipe points these at native sysroot tools so
target executables are never used for code generation. Host tool overrides have
been build-tested natively; the gRPC-enabled recipe dependency graph has also
passed a BitBake dry run. Cross-gRPC compilation and recipe packaging remain unverified.
Production distribution should replace externalsrc with an immutable source URI,
revision and appropriate license checksum once a source release exists.

# Cross-compilation check without BitBake

On an ARM Ubuntu/Debian host:

```sh
sudo apt-get install g++-x86-64-linux-gnu qemu-user
cmake -S . -B build-cross-x86 -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/linux-x86_64.cmake \
  -DCMAKE_BUILD_TYPE=Release -DRTX_GRPC=OFF
cmake --build build-cross-x86
ctest --test-dir build-cross-x86 -R 'rtx_(time|fault|launch)_tests' --output-on-failure
qemu-x86_64 -L /usr/x86_64-linux-gnu ./build-cross-x86/adas_demo configurations/adas.launch
```

This verifies foreign-architecture compilation and selected execution under user
emulation, not a Yocto image or realtime timing. A Yocto SDK instead supplies its
own CMake toolchain and sysroot through its environment setup script. Do not use
the Ubuntu toolchain file as a substitute for that SDK.

# BitBake metadata validation

Parsing all 924 recipes and `bitbake -n rtx` succeeded with Poky walnascar commit
`d0b46a6624ec9c61c47270745dd0b2d5abbe6ac1`, MACHINE=qemuarm64, and gRPC disabled.
The dry run resolved 842 tasks. The evaluated variables and warnings are recorded
in [the validation report](../docs/measurements/yocto-validation.json).

After adding the layer and setting RTX_SOURCE_DIR in a dedicated Yocto build:

```sh
bitbake -p
bitbake -e rtx > rtx-environment.txt
bitbake -n rtx
```

This check used BB_NO_NETWORK=1, so uninative fetching was disabled with warnings;
Ubuntu 26.04 also produced an unsupported-host warning. A real build should use a
validated host, allow required source downloads, run `bitbake rtx` through package
QA, and install/run the resulting packages in a target image. Metadata dry-run
success does not establish compilation or image support. Other listed layer series
remain intended compatibility targets, not tested releases.

## Optional gRPC dependency graph

With the matching meta-openembedded `meta-oe` layer added, the gRPC-enabled recipe
also passed `bitbake -n rtx`: 1097 tasks resolved successfully. The evaluated
PACKAGECONFIG enables gRPC and DEPENDS includes grpc, protobuf, grpc-native and
protobuf-native. Layer revisions, resolved CMake options and warnings are in
[the gRPC validation report](../docs/measurements/yocto-grpc-validation.json).

```sh
bitbake-layers add-layer /absolute/path/meta-openembedded/meta-oe
# Add PACKAGECONFIG:append:pn-rtx = " grpc" to conf/local.conf.
bitbake -e rtx > rtx-grpc-environment.txt
bitbake -n rtx
```

The checked meta-oe recipes use gRPC 1.71.0 and protobuf 5.29.5. The native gRPC
recipe's default `cpp` option supplies grpc_cpp_plugin; preserve it when overriding
that recipe's PACKAGECONFIG. A resolved dependency graph verifies provider
availability and configuration expansion, not that these versions compile with
the SDK or pass target tests. That still requires a real Yocto build and image.

## Target behavior tests with ptest

Enable Yocto's standard ptest feature and add the runtime tests to your image:

```bitbake
DISTRO_FEATURES:append = " ptest"
IMAGE_INSTALL:append = " rtx rtx-ptest ptest-runner"
```

Build the image through your usual image recipe, boot it, and run:

```sh
ptest-runner rtx
# Optional longer worker/mailbox stress sample:
RTX_STRESS_MS=10000 ptest-runner rtx
```

The recipe enables BUILD_TESTING only with PTEST_ENABLED=1, and installs the
behavior executables into the standard PTEST_PATH. It includes gRPC round-trip
tests when PACKAGECONFIG enables grpc. CTest, CMake, the compiler, and the
host-side installed-consumer test are not needed on the image. The runner emits
PASS/FAIL/SKIP records, treats FIFO permission denial (exit 77) as a skip, and
returns failure if any executable fails or no tests were installed. Ensure the
image mounts writable /dev/shm and supports robust process-shared pthread mutexes.

Poky walnascar metadata expansion and the gRPC+ptest dry run pass. Running the
same ptest script against native shared Release binaries produced 17 passes and
one privileged FIFO skip. Failure/skip and empty-install reporting were checked
separately. This does not establish target execution: the attempted real Yocto
build stopped in dependency fetching because BB_NO_NETWORK=1 and the download
cache is incomplete, before RTX compilation or package QA. Allow required source
downloads or populate the cache on a validated build host, then build and run the
image. See [the ptest report](../docs/measurements/yocto-ptest-validation.json).

A subsequent core-only build retry enabled networking through a temporary
BitBake post-read configuration. It also stopped before RTX compilation: four
dependency fetches failed (quilt, m4, gettext-minimal and gnu-config). A direct
request to the m4 source returned HTTP 403 with `Approval required for
ftp.gnu.org:443` from the sandbox network proxy. Source access or a populated
source mirror is still needed; changing BB_NO_NETWORK alone did not unblock it.

## Bootable validation image and automated guest tests

The layer provides `rtx-validation-image` for an isolated development target.
It includes RTX, its ptests, ptest-runner and SSH access, and requires ptest in
DISTRO_FEATURES. Keep the RTX_SOURCE_DIR setting above and configure:

```bitbake
MACHINE = "qemuarm64"
DISTRO_FEATURES:append = " ptest"
# Optional gRPC coverage, with meta-oe in BBLAYERS:
PACKAGECONFIG:append:pn-rtx = " grpc"
```

On a validated Yocto build host with source access and QEMU testimage networking:

```sh
bitbake rtx-validation-image
bitbake -c testimage rtx-validation-image
```

The image requests a single virtual CPU and 1 GiB RAM. Its testimage suite checks
ping/SSH and runs the layer's RTX runtime test. That test discovers installed RTX
executables, requires the complete 18-test base set, runs `ptest-runner -t 120 rtx`,
and requires exactly one PASS for every installed executable. The optional gRPC
tests are required to pass when installed. FIFO permission skips, missing or
duplicate results and failures all fail the image test. OEQA captures its output
through normal testimage results/logging. This makes a green image run stronger
than a host run that skips privileged scheduling checks.

The image deliberately permits passwordless root SSH for automated tests. Use it
only as an isolated validation image; it is not a production ECU image. It does
not add third-party source mirrors or change the build's network policy.

Poky walnascar metadata expansion and the image dependency dry run are checked
locally; the result validator was exercised with recorded guest-test passes and
negative skip/fail/missing/duplicate/empty cases. The image has not been built or
booted: upstream source fetching remains blocked in this sandbox. The standalone
Linux VM results in docs/realtime-validation.md remain separate evidence and do
not substitute for this Yocto image test.
