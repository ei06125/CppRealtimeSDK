#!/usr/bin/env python3
# Package trusted native ARM64 tests into a temporary diskless Linux guest.
# Require complete successful result records, including privileged FIFO execution, before returning success.
"""Run ARM64 Linux test executables as guest root in a one-CPU QEMU VM.

Requires trusted native ARM64 binaries, ldd, a static ARM64 BusyBox, QEMU, and
an ARM64 Linux kernel with initramfs/devtmpfs/proc/tmpfs support. No host root
privileges or guest disk/network are needed. Guest execution is not hardware timing.
"""
import argparse
import gzip
import pathlib
import re
import struct
import stat
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kernel', required=True, type=pathlib.Path)
    parser.add_argument('--busybox', default='/usr/bin/busybox', type=pathlib.Path)
    parser.add_argument('--qemu', default='qemu-system-aarch64')
    parser.add_argument('--log', required=True, type=pathlib.Path)
    parser.add_argument('--timeout', default=120, type=int)
    parser.add_argument('tests', nargs='+', type=pathlib.Path)
    args = parser.parse_args()
    files = {}
    for binary in args.tests:
        name = 'tests/' + binary.name
        if name in files:
            parser.error('test basenames must be unique')
        files[name] = binary.read_bytes()
        # ldd is appropriate only for trusted executables built by the caller.
        deps = subprocess.check_output(['ldd', str(binary.resolve())], text=True)
        if 'not found' in deps:
            parser.error('unresolved library dependency for ' + str(binary))
        for dependency in re.findall(r'(/[^\s()]+)', deps):
            files[dependency.lstrip('/')] = pathlib.Path(dependency).read_bytes()
    files['bin/busybox'] = args.busybox.read_bytes()
    commands = '\n'.join(
        f'/tests/{test.name}\nresult=$?\n'
        f'echo "RTX_VM_TEST {test.name} $result"\n'
        '[ "$result" = 0 ] || failed=1' for test in args.tests)
    # Names are emitted into shell source; restrict them to simple basenames.
    if any(not re.fullmatch(r'[A-Za-z0-9_.-]+', test.name) for test in args.tests):
        parser.error('test basenames must contain only letters, digits, _, . or -')
    files['init'] = ('''#!/bin/sh
set -e
/bin/busybox mount -t proc proc /proc
/bin/busybox mount -t devtmpfs devtmpfs /dev
/bin/busybox mkdir -p /dev/shm
/bin/busybox mount -t tmpfs tmpfs /dev/shm
/bin/busybox ip link set lo up
set +e
failed=0
''' + commands + '''
echo "RTX_VM_DONE $failed"
/bin/busybox poweroff -f
''').encode()
    directories = {'bin', 'dev', 'proc', 'sys', 'tmp', 'tests'}
    for name in files:
        directories.update(str(p) for p in pathlib.PurePosixPath(name).parents if str(p) != '.')
    # Emit a newc archive directly, including /dev/console, without mknod/root.
    archive = bytearray()
    def entry(name, mode, data=b'', major=0, minor=0):
        encoded = name.encode() + b'\0'
        fields = [0, mode, 0, 0, 1, 0, len(data), 0, 0, major, minor, len(encoded), 0]
        archive.extend(b'070701' + ''.join(f'{v:08x}' for v in fields).encode())
        archive.extend(encoded)
        archive.extend(b'\0' * (-len(archive) % 4))
        archive.extend(data)
        archive.extend(b'\0' * (-len(archive) % 4))
    for name in sorted(directories):
        entry(name, stat.S_IFDIR | 0o755)
    for name, data in files.items():
        entry(name, stat.S_IFREG | 0o755, data)
    entry('bin/sh', stat.S_IFLNK | 0o777, b'busybox')
    entry('dev/console', stat.S_IFCHR | 0o600, major=5, minor=1)
    entry('TRAILER!!!', 0)
    with tempfile.TemporaryDirectory(prefix='rtx-linux-vm-') as temporary:
        image = pathlib.Path(temporary) / 'initramfs.cpio.gz'
        image.write_bytes(gzip.compress(archive, mtime=0))
        kernel = args.kernel.resolve()
        kernel_data = kernel.read_bytes()
        # Linux EFI zboot stores the compressed payload offset/size in its header.
        # QEMU 10 cannot load zstd zboot directly; extract the bounded payload.
        if kernel_data[4:8] == b'zimg' and kernel_data[24:28] == b'zstd':
            offset, size = struct.unpack_from('<II', kernel_data, 8)
            if offset + size > len(kernel_data):
                raise SystemExit('Truncated EFI zboot payload')
            kernel = pathlib.Path(temporary) / 'Image'
            with kernel.open('wb') as output:
                subprocess.run(['zstd', '-d', '-c'], input=kernel_data[offset:offset + size],
                               stdout=output, check=True)
        command = [args.qemu, '-machine', 'virt', '-cpu', 'cortex-a72', '-smp', '1',
                   '-m', '512', '-nographic', '-no-reboot', '-nic', 'none',
                   '-kernel', str(kernel), '-initrd', str(image),
                   '-append', 'console=ttyAMA0 rdinit=/init panic=-1']
        with args.log.open('w') as output:
            try:
                result = subprocess.run(command, stdout=output, stderr=subprocess.STDOUT,
                                        timeout=args.timeout, check=False)
            except subprocess.TimeoutExpired:
                raise SystemExit('VM timed out; inspect ' + str(args.log))
    log = args.log.read_text(errors='replace')
    records = re.findall(r'^RTX_VM_TEST (\S+) (\d+)\s*$', log, re.MULTILINE)
    for name, code in records:
        print(name + ': exit ' + code)
    expected = [(test.name, '0') for test in args.tests]
    if result.returncode or records != expected or not re.search(r'^RTX_VM_DONE 0\s*$', log, re.MULTILINE):
        raise SystemExit('Guest tests failed or incomplete; inspect ' + str(args.log))
    print('All guest tests passed; log: ' + str(args.log))


if __name__ == '__main__':
    main()
