#!/usr/bin/env python3
# Format maintained C/C++ and protocol sources with the project's Google preset.
# Git enumerates sources so generated build trees and external kernels are excluded.
"""Run from any directory; pass --check for a non-mutating formatting check."""

import argparse
import os
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    formatter = os.environ.get("CLANG_FORMAT", "clang-format-18")
    version = subprocess.check_output([formatter, "--version"], text=True)
    if "version 18." not in version:
        parser.error("clang-format 18 is required (set CLANG_FORMAT to its executable)")
    tracked = subprocess.check_output(
        ["git", "ls-files", "-z"], cwd=root
    ).decode().split("\0")
    files = [name for name in tracked if Path(name).suffix in
             {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".proto"}]
    options = ["--dry-run", "--Werror"] if args.check else ["-i"]
    subprocess.run([formatter, "--style=file", *options, *files], cwd=root, check=True)
    print(f"{'Checked' if args.check else 'Formatted'} {len(files)} source files.")


if __name__ == "__main__":
    main()
