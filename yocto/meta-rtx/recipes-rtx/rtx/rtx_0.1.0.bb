# Development externalsrc recipe for shared Linux libraries and optional target ptests.
# Use native protocol generators during cross compilation; no project license grant is implied.
SUMMARY = "ECU node scheduling and execution management library"
DESCRIPTION = "Hosted C++20 scheduler, POSIX execution backend and optional gRPC management service"
LICENSE = "CLOSED"

# Development recipe: build the user's checkout without copying or fetching it.
# Set RTX_SOURCE_DIR in local.conf to the absolute library checkout path.
RTX_SOURCE_DIR ?= ""
EXTERNALSRC = "${RTX_SOURCE_DIR}"
EXTERNALSRC_BUILD = "${WORKDIR}/build"
inherit cmake externalsrc ptest

python __anonymous() {
    if not d.getVar('RTX_SOURCE_DIR'):
        bb.fatal('rtx requires RTX_SOURCE_DIR in local.conf (absolute SDK checkout path)')
}

PACKAGECONFIG ??= ""
PACKAGECONFIG[grpc] = "-DRTX_GRPC=ON,-DRTX_GRPC=OFF,grpc protobuf grpc-native protobuf-native"
EXTRA_OECMAKE += " \
    -DBUILD_SHARED_LIBS=ON \
    -DBUILD_TESTING=${@'ON' if d.getVar('PTEST_ENABLED') == '1' else 'OFF'} \
    -DRTX_EXAMPLES=OFF \
    -DRTX_POSIX=ON \
    -DRTX_PROTOC_EXECUTABLE=${STAGING_BINDIR_NATIVE}/protoc \
    -DRTX_GRPC_PLUGIN_EXECUTABLE=${STAGING_BINDIR_NATIVE}/grpc_cpp_plugin \
"

FILES:${PN}-dev += "${datadir}/rtx/proto"

# Install only target executables; CTest and its host-side relocation check are
# deliberately unnecessary on the image. Keep gRPC tests conditional.
RTX_PTESTS = "rtx_mailbox_tests rtx_dispatch_allocation_tests rtx_fault_tests rtx_time_tests rtx_launch_tests \
    rtx_shm_setup_fault_tests rtx_posix_clock_fault_tests \
    rtx_runner_ownership_tests rtx_launch_watchdog_tests rtx_stop_latency_tests \
    rtx_fifo_tests rtx_runtime_fault_tests rtx_shared_memory_tests rtx_tests \
    rtx_watchdog_tests rtx_lifecycle_tests rtx_tasks_tests rtx_stress_tests"
RTX_PTESTS:append = " ${@bb.utils.contains('PACKAGECONFIG', 'grpc', 'rtx_pipeline_tests rtx_control_tests', '', d)}"

do_install_ptest() {
    install -d ${D}${PTEST_PATH}
    for test in ${RTX_PTESTS}; do
        install -m 0755 ${B}/$test ${D}${PTEST_PATH}/$test
    done
    install -m 0755 ${S}/yocto/meta-rtx/recipes-rtx/rtx/run-ptest ${D}${PTEST_PATH}/run-ptest
}
