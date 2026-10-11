# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Resolves eden-uwp.exe RVAs to functions and source lines with the build's PDB, through the
Windows SDK's dbghelp.dll. Shared by resolve-rva.py and profile-report.py."""
import ctypes
import ctypes.wintypes as wt
import os
import struct
import sys

DBGHELP_CANDIDATES = [
    r"C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\dbghelp.dll",
    "dbghelp.dll",
]
BASE = 0x10000000
MAX_NAME = 1024


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


def pe_timestamp(path):
    """The link time stamp of a PE file (IMAGE_FILE_HEADER.TimeDateStamp)."""
    with open(path, "rb") as file:
        header = file.read(4096)
    pe_offset = struct.unpack_from("<I", header, 0x3C)[0]
    return struct.unpack_from("<I", header, pe_offset + 8)[0]


class Resolver:
    """Function and file:line of an RVA in the executable, cached per RVA."""

    def __init__(self, exe):
        self.dbghelp = load_dbghelp()
        self.dbghelp.SymSetOptions(0x2 | 0x10)  # SYMOPT_UNDNAME | SYMOPT_LOAD_LINES
        self.process = wt.HANDLE(0x1234)  # any unique value: no live process is attached
        exe = os.path.abspath(exe)
        if not self.dbghelp.SymInitialize(self.process, os.path.dirname(exe).encode(), False):
            sys.exit("SymInitialize failed")
        self.dbghelp.SymLoadModuleEx.restype = ctypes.c_uint64
        self.dbghelp.SymLoadModuleEx.argtypes = [
            wt.HANDLE, wt.HANDLE, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint64, wt.DWORD,
            ctypes.c_void_p, wt.DWORD]
        if not self.dbghelp.SymLoadModuleEx(self.process, None, exe.encode(), None, BASE, 0,
                                            None, 0):
            sys.exit(f"cannot load symbols for {exe}")
        self.dbghelp.SymFromAddr.argtypes = [
            wt.HANDLE, ctypes.c_uint64, ctypes.POINTER(ctypes.c_uint64),
            ctypes.POINTER(SymbolInfo)]
        self.dbghelp.SymGetLineFromAddr64.argtypes = [
            wt.HANDLE, ctypes.c_uint64, ctypes.POINTER(wt.DWORD), ctypes.POINTER(LineInfo)]
        self.cache = {}

    def symbol(self, rva):
        """(function name or None, displacement, file or None, line)."""
        cached = self.cache.get(rva)
        if cached is not None:
            return cached
        address = BASE + rva
        symbol = SymbolInfo(SizeOfStruct=ctypes.sizeof(SymbolInfo) - MAX_NAME,
                            MaxNameLen=MAX_NAME)
        displacement = ctypes.c_uint64()
        name, offset = None, 0
        if self.dbghelp.SymFromAddr(self.process, address, ctypes.byref(displacement),
                                    ctypes.byref(symbol)):
            name, offset = symbol.Name.decode(errors="replace"), displacement.value
        line = LineInfo(SizeOfStruct=ctypes.sizeof(LineInfo))
        line_displacement = wt.DWORD()
        where, number = None, 0
        if self.dbghelp.SymGetLineFromAddr64(self.process, address,
                                             ctypes.byref(line_displacement), ctypes.byref(line)):
            where, number = line.FileName.decode(errors="replace"), line.LineNumber
        result = (name, offset, where, number)
        self.cache[rva] = result
        return result

    def describe(self, rva):
        name, offset, where, number = self.symbol(rva)
        text = f"{name}+0x{offset:x}" if name else "?"
        return text + (f"  {where}:{number}" if where else "")
