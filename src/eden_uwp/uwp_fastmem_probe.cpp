// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Fastmem feasibility probe for the Xbox AppContainer. Fastmem needs what the UWP HostMemory path
// avoids today: a 512 GiB placeholder reservation, the emulated DRAM as a file-mapping section, and
// views of that section mapped (aliased) into the placeholder with per-range protection. This runs
// each of those steps on its own, before the emulator starts, logs what the console allows and frees
// everything again. It changes nothing about how the emulator runs.

#include <string>
#include <windows.h>

#include "common/common_types.h"
#include "common/dynamic_library.h"
#include "eden_uwp/uwp_fastmem_probe.h"

#ifndef MEM_RESERVE_PLACEHOLDER
#define MEM_RESERVE_PLACEHOLDER 0x00040000
#endif
#ifndef MEM_REPLACE_PLACEHOLDER
#define MEM_REPLACE_PLACEHOLDER 0x00004000
#endif
#ifndef MEM_PRESERVE_PLACEHOLDER
#define MEM_PRESERVE_PLACEHOLDER 0x00000002
#endif

namespace EdenXbox {
namespace {

using PFN_VirtualAlloc2FromApp = PVOID(WINAPI*)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, void*, ULONG);
using PFN_MapViewOfFile3FromApp = PVOID(WINAPI*)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG,
                                                 ULONG, void*, ULONG);
using PFN_UnmapViewOfFile2 = BOOL(WINAPI*)(HANDLE, PVOID, ULONG);
using PFN_CreateFileMappingFromApp = HANDLE(WINAPI*)(HANDLE, PSECURITY_ATTRIBUTES, ULONG, ULONG64,
                                                     PCWSTR);
using PFN_VirtualProtectFromApp = BOOL(WINAPI*)(PVOID, SIZE_T, ULONG, PULONG);
using PFN_VirtualAllocFromApp = PVOID(WINAPI*)(PVOID, SIZE_T, ULONG, ULONG);

constexpr u64 VirtualReserveSize = 1ULL << 39; // Core::DeviceMemory without NCE
constexpr u64 DramSize = 4ULL << 30;           // the smallest Switch memory layout
constexpr u64 ViewSize = 64ULL << 20;
constexpr u64 AliasOffset = 1ULL << 30;

std::string Error() {
    return " (error " + std::to_string(GetLastError()) + ")";
}

// SEH needs a frame without C++ objects to unwind, hence these two tiny helpers.
bool TryWrite(volatile u8* address, u8 value) {
    __try {
        *address = value;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool TryRead(volatile const u8* address, u8* value) {
    __try {
        *value = *address;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

} // namespace

void ProbeFastmem(const ProbeLog& log, const ProbeMemory& memory) {
    Common::DynamicLibrary kernelbase("Kernelbase");
    PFN_VirtualAlloc2FromApp virtual_alloc2{};
    PFN_MapViewOfFile3FromApp map_view3{};
    PFN_UnmapViewOfFile2 unmap_view2{};
    PFN_CreateFileMappingFromApp create_mapping{};
    PFN_VirtualProtectFromApp virtual_protect{};
    PFN_VirtualAllocFromApp virtual_alloc{};
    if (!kernelbase.IsOpen() || !kernelbase.GetSymbol("VirtualAlloc2FromApp", &virtual_alloc2) ||
        !kernelbase.GetSymbol("MapViewOfFile3FromApp", &map_view3) ||
        !kernelbase.GetSymbol("UnmapViewOfFile2", &unmap_view2) ||
        !kernelbase.GetSymbol("CreateFileMappingFromApp", &create_mapping) ||
        !kernelbase.GetSymbol("VirtualProtectFromApp", &virtual_protect) ||
        !kernelbase.GetSymbol("VirtualAllocFromApp", &virtual_alloc)) {
        log("fastmem probe: FAILED resolving the placeholder APIs in Kernelbase");
        return;
    }
    const HANDLE process = GetCurrentProcess();
    log("fastmem probe: start | " + memory());

    // 1. The address-space placeholder the guest's 39-bit space is mirrored into.
    auto* const arena = static_cast<u8*>(virtual_alloc2(process, nullptr, VirtualReserveSize,
                                                        MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
                                                        PAGE_NOACCESS, nullptr, 0));
    log(arena ? "fastmem probe: 512 GiB placeholder reserved OK"
              : "fastmem probe: 512 GiB placeholder FAILED" + Error());

    // 2. DRAM as a section. SEC_COMMIT charges all 4 GiB up front; SEC_RESERVE would only charge what
    //    gets committed, if the console lets a view of it be committed.
    HANDLE commit_section = create_mapping(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_COMMIT,
                                           DramSize, nullptr);
    log(commit_section ? "fastmem probe: 4 GiB SEC_COMMIT section OK | " + memory()
                       : "fastmem probe: 4 GiB SEC_COMMIT section FAILED" + Error());
    if (commit_section) {
        CloseHandle(commit_section);
    }

    HANDLE reserve_section = create_mapping(INVALID_HANDLE_VALUE, nullptr,
                                            PAGE_READWRITE | SEC_RESERVE, DramSize, nullptr);
    log(reserve_section ? "fastmem probe: 4 GiB SEC_RESERVE section OK"
                        : "fastmem probe: 4 GiB SEC_RESERVE section FAILED" + Error());
    if (reserve_section) {
        auto* const view = static_cast<u8*>(
            map_view3(reserve_section, process, nullptr, 0, ViewSize, 0, PAGE_READWRITE, nullptr, 0));
        if (!view) {
            log("fastmem probe: view of the SEC_RESERVE section FAILED" + Error());
        } else {
            const bool committed = virtual_alloc(view, 64 * 1024, MEM_COMMIT, PAGE_READWRITE) != nullptr;
            const std::string commit_error = committed ? "" : Error();
            const bool written = committed && TryWrite(view, 0x5A);
            log(std::string("fastmem probe: commit on demand in a SEC_RESERVE view: ") +
                (committed ? "commit OK" : "commit FAILED" + commit_error) +
                (committed ? (written ? ", write OK" : ", write FAULTED") : ""));
            unmap_view2(process, view, 0);
        }
        CloseHandle(reserve_section);
    }

    // 3. Two views of one section inside the placeholder, like HostMemory::Map does for fastmem: split
    //    the placeholder, replace a piece with a view, and check that both views share the same pages.
    if (arena) {
        HANDLE section = create_mapping(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_COMMIT,
                                        ViewSize, nullptr);
        u8* view_a = nullptr;
        u8* view_b = nullptr;
        bool split = false;
        if (!section) {
            log("fastmem probe: 64 MiB section FAILED" + Error());
        } else {
            split = VirtualFree(arena, ViewSize, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER) &&
                    VirtualFree(arena + AliasOffset, ViewSize, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
            if (!split) {
                log("fastmem probe: splitting the placeholder FAILED" + Error());
            } else {
                view_a = static_cast<u8*>(map_view3(section, process, arena, 0, ViewSize,
                                                    MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0));
                const std::string error_a = view_a ? "" : Error();
                view_b = static_cast<u8*>(map_view3(section, process, arena + AliasOffset, 0, ViewSize,
                                                    MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0));
                const std::string error_b = view_b ? "" : Error();
                log(std::string("fastmem probe: views into the placeholder: A ") +
                    (view_a ? "OK" : "FAILED" + error_a) + ", B " + (view_b ? "OK" : "FAILED" + error_b));
            }
        }
        if (view_a && view_b) {
            u8 seen = 0;
            const bool aliased = TryWrite(view_a + 0x1234, 0xA5) && TryRead(view_b + 0x1234, &seen) &&
                                 seen == 0xA5;
            log(std::string("fastmem probe: aliasing between views ") + (aliased ? "OK" : "FAILED"));

            ULONG old_protect = 0;
            const bool protected_ok = virtual_protect(view_b, 4096, PAGE_READONLY, &old_protect) != 0;
            const std::string protect_error = protected_ok ? "" : Error();
            const bool write_faulted = protected_ok && !TryWrite(view_b, 1);
            const bool other_view_writable = TryWrite(view_a, 2);
            log(std::string("fastmem probe: VirtualProtectFromApp read-only ") +
                (protected_ok ? "OK" : "FAILED" + protect_error) +
                ", write to it " + (write_faulted ? "faulted and was caught" : "did NOT fault") +
                ", other view " + (other_view_writable ? "still writable" : "NOT writable"));
        }
        if (view_a) {
            unmap_view2(process, view_a, MEM_PRESERVE_PLACEHOLDER);
        }
        if (view_b) {
            unmap_view2(process, view_b, MEM_PRESERVE_PLACEHOLDER);
        }
        if (section) {
            CloseHandle(section);
        }
        // After the splits the placeholder is up to four pieces; release each one.
        if (split) {
            VirtualFree(arena, 0, MEM_RELEASE);
            VirtualFree(arena + ViewSize, 0, MEM_RELEASE);
            VirtualFree(arena + AliasOffset, 0, MEM_RELEASE);
            VirtualFree(arena + AliasOffset + ViewSize, 0, MEM_RELEASE);
        } else {
            VirtualFree(arena, 0, MEM_RELEASE);
        }
    }
    log("fastmem probe: done | " + memory());
}

} // namespace EdenXbox
