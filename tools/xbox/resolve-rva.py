# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Resolves the eden-uwp.exe+0xRVA (or bare +0xRVA) frames of the diag (crash stacks, alloc watch) to functions and
source lines with the build's PDB, through the Windows SDK's dbghelp.dll.

    python tools/xbox/resolve-rva.py [--exe build-uwp/bin/eden-uwp.exe] [diag.txt | -]

Prints every input line, and under each eden-uwp.exe frame its function and file:line. The PDB must
be the one of the build that wrote the diag.
"""
import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pdb_symbols import Resolver  # noqa: E402

# "eden-uwp.exe+0x..." (crash lines) or a bare "+0x..." frame (stack lists).
FRAME = re.compile(r"(?:eden-uwp\.exe|(?<=\s))\+0x([0-9a-fA-F]+)")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", default=os.path.join("build-uwp", "bin", "eden-uwp.exe"))
    parser.add_argument("input", nargs="?", default="-")
    args = parser.parse_args()

    resolver = Resolver(args.exe)
    source = sys.stdin if args.input == "-" else open(args.input, encoding="utf-8",
                                                       errors="replace")
    with source:
        for text in source:
            print(text.rstrip("\n"))
            for match in FRAME.finditer(text):
                print(f"    0x{match.group(1)}: {resolver.describe(int(match.group(1), 16))}")


if __name__ == "__main__":
    main()
