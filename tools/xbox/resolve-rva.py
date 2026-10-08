# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Resolves the eden-uwp.exe+0xRVA (or bare +0xRVA) frames of the diag (crash stacks, alloc watch) to functions and
source lines with the build's PDB, through the Windows SDK's dbghelp.dll.

    python tools/xbox/resolve-rva.py [--exe build-uwp/bin/eden-uwp.exe] [diag.txt | -]

Prints every input line, and under each eden-uwp.exe frame its function and file:line. The PDB must
be the one of the build that wrote the diag.
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import os
import re
import sys

# "eden-uwp.exe+0x..." (crash lines) or a bare "+0x..." frame (stack lists).
FRAME = re.compile(r"(?:eden-uwp\.exe|(?<=\s))\+0x([0-9a-fA-F]+)")
DBGHELP_CANDIDATES = [
    r"C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\dbghelp.dll",
    "dbghelp.dll",
]
BASE = 0x10000000
MAX_NAME = 512


class SymbolInfo(ctypes.Structure):
    _fields_ = [
        ("SizeOfStruct", wt.ULONG), ("TypeIndex", wt.ULONG), ("Reserved", ctypes.c_uint64 * 2),
        ("Index", wt.ULONG), ("Size", wt.ULONG), ("ModBase", ctypes.c_uint64),
        ("Flags", wt.ULONG), ("Value", ctypes.c_uint64), ("Address", ctypes.c_uint64),
        ("Register", wt.ULONG), ("Scope", wt.ULONG), ("Tag", wt.ULONG), ("NameLen", wt.ULONG),
        ("MaxNameLen", wt.ULONG), ("Name", ctypes.c_char * MAX_NAME),
    ]


class LineInfo(ctypes.Structure):
    _fields_ = [
        ("SizeOfStruct", wt.DWORD), ("Key", ctypes.c_void_p), ("LineNumber", wt.DWORD),
        ("FileName", ctypes.c_char_p), ("Address", ctypes.c_uint64),
    ]


def load_dbghelp():
    for path in DBGHELP_CANDIDATES:
        try:
            return ctypes.WinDLL(path)
        except OSError:
            continue
    sys.exit("dbghelp.dll not found")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", default=os.path.join("build-uwp", "bin", "eden-uwp.exe"))
    parser.add_argument("input", nargs="?", default="-")
    args = parser.parse_args()

    dbghelp = load_dbghelp()
    dbghelp.SymSetOptions(0x2 | 0x10)  # SYMOPT_UNDNAME | SYMOPT_LOAD_LINES
    process = wt.HANDLE(0x1234)  # any unique value: no live process is attached
    exe = os.path.abspath(args.exe)
    if not dbghelp.SymInitialize(process, os.path.dirname(exe).encode(), False):
        sys.exit("SymInitialize failed")
    dbghelp.SymLoadModuleEx.restype = ctypes.c_uint64
    dbghelp.SymLoadModuleEx.argtypes = [wt.HANDLE, wt.HANDLE, ctypes.c_char_p, ctypes.c_char_p,
                                        ctypes.c_uint64, wt.DWORD, ctypes.c_void_p, wt.DWORD]
    if not dbghelp.SymLoadModuleEx(process, None, exe.encode(), None, BASE, 0, None, 0):
        sys.exit(f"cannot load symbols for {exe}")
    dbghelp.SymFromAddr.argtypes = [wt.HANDLE, ctypes.c_uint64, ctypes.POINTER(ctypes.c_uint64),
                                    ctypes.POINTER(SymbolInfo)]
    dbghelp.SymGetLineFromAddr64.argtypes = [wt.HANDLE, ctypes.c_uint64, ctypes.POINTER(wt.DWORD),
                                             ctypes.POINTER(LineInfo)]

    def resolve(rva):
        address = BASE + rva
        symbol = SymbolInfo(SizeOfStruct=ctypes.sizeof(SymbolInfo) - MAX_NAME,
                            MaxNameLen=MAX_NAME)
        displacement = ctypes.c_uint64()
        name = "?"
        if dbghelp.SymFromAddr(process, address, ctypes.byref(displacement), ctypes.byref(symbol)):
            name = f"{symbol.Name.decode(errors='replace')}+0x{displacement.value:x}"
        line = LineInfo(SizeOfStruct=ctypes.sizeof(LineInfo))
        line_displacement = wt.DWORD()
        where = ""
        if dbghelp.SymGetLineFromAddr64(process, address, ctypes.byref(line_displacement),
                                        ctypes.byref(line)):
            where = f"  {line.FileName.decode(errors='replace')}:{line.LineNumber}"
        return name + where

    source = sys.stdin if args.input == "-" else open(args.input, encoding="utf-8",
                                                       errors="replace")
    with source:
        for text in source:
            print(text.rstrip("\n"))
            for match in FRAME.finditer(text):
                print(f"    0x{match.group(1)}: {resolve(int(match.group(1), 16))}")


if __name__ == "__main__":
    main()
