# Require one actual pass for every installed RTX executable on the booted Yocto target.
# Keep result validation strict so skipped, missing or duplicate records cannot hide lost coverage.
"""Require actual passes from every installed RTX ptest, including FIFO."""
import re
import shlex

from oeqa.runtime.case import OERuntimeTestCase
from oeqa.core.decorator.depends import OETestDepends
from oeqa.runtime.decorator.package import OEHasPackage


class RtxRuntimeTest(OERuntimeTestCase):
    @staticmethod
    def validate_results(installed, output):
        required = {
            'rtx_mailbox_tests', 'rtx_dispatch_allocation_tests', 'rtx_fault_tests', 'rtx_time_tests',
            'rtx_launch_tests', 'rtx_shm_setup_fault_tests',
            'rtx_posix_clock_fault_tests', 'rtx_runner_ownership_tests',
            'rtx_launch_watchdog_tests', 'rtx_stop_latency_tests', 'rtx_fifo_tests',
            'rtx_runtime_fault_tests', 'rtx_shared_memory_tests', 'rtx_tests',
            'rtx_watchdog_tests', 'rtx_lifecycle_tests', 'rtx_tasks_tests',
            'rtx_stress_tests',
        }
        if not required.issubset(installed):
            raise AssertionError('Missing installed RTX tests: ' + str(sorted(required - installed)))
        records = re.findall(r'^(PASS|FAIL|SKIP): (rtx_\S*tests)(?:[ \t]+[^\r\n]*)?\r?$', output, re.MULTILINE)
        passed = {name for status, name in records if status == 'PASS'}
        bad = [(status, name) for status, name in records if status != 'PASS']
        if bad or passed != installed or len(records) != len(installed):
            raise AssertionError('RTX results incomplete, duplicated, failed or skipped: ' +
                                 str(bad) + '\n' + output)

    @OETestDepends(['ssh.SSHTest.test_ssh'])
    @OEHasPackage('rtx-ptest')
    @OEHasPackage('ptest-runner')
    def test_rtx_ptest(self):
        directory = self.td.get('libdir', '/usr/lib') + '/rtx/ptest'
        status, listing = self.target.run(
            'find ' + shlex.quote(directory) + " -maxdepth 1 -type f -name 'rtx_*tests'", 30)
        self.assertEqual(status, 0, listing)
        installed = {path.rsplit('/', 1)[-1] for path in listing.splitlines() if path.strip()}
        status, output = self.target.run('ptest-runner -t 120 rtx', 150)
        self.assertEqual(status, 0, output)
        self.validate_results(installed, output)
