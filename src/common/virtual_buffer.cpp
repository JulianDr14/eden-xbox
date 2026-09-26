// SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#if defined(_WIN32) && defined(YUZU_UWP_APPCONTAINER)
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>

#include "common/dynamic_library.h"
#include "common/logging.h"
#endif

#include "common/assert.h"
#include "common/virtual_buffer.h"

namespace Common {

#if defined(_WIN32) && defined(YUZU_UWP_APPCONTAINER)
// Xbox/UWP: demand-commit large buffers instead of committing them up front.
//
// The CPU page table for a 39-bit guest address space is 2^27 entries x 32 bytes = 4 GiB. On desktop
// Windows an eager MEM_COMMIT of that succeeds: the pagefile absorbs the commit charge and pages are
// only materialised on first touch. The Xbox has no pagefile, so the commit is charged in full
// against the title budget and fails; the soft ASSERT then returns nullptr and the first MapPages
// access-violates (observed on-console: "assert base", then a write to 0x1018c20).
//
// Same design as the emulated-DRAM backing in host_memory.cpp: reserve the range, and let a vectored
// exception handler commit 64 KiB chunks on first touch. That also covers the JIT, which reads page
// table entries directly: an entry nobody has touched faults once, is committed as zero (== unmapped)
// and the load retries. A homebrew NRO touches a few MiB of the 4 GiB.
namespace {

// Resolved by name, never imported: see the note on PFN_AddVectoredExceptionHandler in
// host_memory.cpp (importing the errorhandling api-set makes the app fail to activate on-console).
using PFN_VirtualAllocFromApp = PVOID(WINAPI*)(PVOID BaseAddress, SIZE_T Size,
                                               ULONG AllocationType, ULONG Protection);
using PFN_AddVectoredExceptionHandler = PVOID(WINAPI*)(ULONG First,
                                                       PVECTORED_EXCEPTION_HANDLER Handler);

// Below this, commit eagerly: not worth a range slot or a fault per chunk.
constexpr std::size_t DEMAND_COMMIT_THRESHOLD = 1024 * 1024;
constexpr std::size_t COMMIT_GRANULARITY = 64 * 1024;

// Fixed-size registry the handler scans lock-free from any thread. Writers serialise on the mutex
// and publish size before base, so a handler that sees a non-zero base also sees its size. A process
// holds a handful of large VirtualBuffers (page tables, device and GPU address maps); a full registry
// falls back to an eager commit rather than failing.
constexpr std::size_t MAX_RANGES = 64;
struct Range {
    std::atomic<std::uintptr_t> base{0};
    std::atomic<std::size_t> size{0};
};
Range g_ranges[MAX_RANGES];
std::mutex g_ranges_mutex;

PFN_VirtualAllocFromApp g_virtual_alloc_from_app{};

LONG NTAPI DemandCommitHandler(EXCEPTION_POINTERS* ep) {
    const EXCEPTION_RECORD* rec = ep->ExceptionRecord;
    if (rec->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || rec->NumberParameters < 2) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const auto fault = static_cast<std::uintptr_t>(rec->ExceptionInformation[1]);
    for (Range& range : g_ranges) {
        const std::uintptr_t base = range.base.load(std::memory_order_acquire);
        if (base == 0 || fault < base) {
            continue;
        }
        const std::size_t size = range.size.load(std::memory_order_relaxed);
        if (fault - base >= size) {
            continue;
        }
        const std::size_t chunk_offset = (fault - base) & ~(COMMIT_GRANULARITY - 1);
        const std::size_t chunk_len = (std::min)(COMMIT_GRANULARITY, size - chunk_offset);
        if (g_virtual_alloc_from_app(reinterpret_cast<void*>(base + chunk_offset), chunk_len,
                                     MEM_COMMIT, PAGE_READWRITE)) {
            return EXCEPTION_CONTINUE_EXECUTION; // committed as zero, retry the access
        }
        return EXCEPTION_CONTINUE_SEARCH; // budget exhausted: crash visibly rather than loop
    }
    return EXCEPTION_CONTINUE_SEARCH; // not ours (e.g. the DRAM backing's own handler)
}

// Resolves the APIs and installs the handler once. False if anything is missing, in which case
// every allocation keeps the eager-commit path.
bool DemandCommitAvailable() noexcept {
    static const bool available = [] {
        static DynamicLibrary kernelbase("Kernelbase");
        PFN_AddVectoredExceptionHandler add_veh{};
        if (!kernelbase.IsOpen() ||
            !kernelbase.GetSymbol("VirtualAllocFromApp", &g_virtual_alloc_from_app) ||
            !kernelbase.GetSymbol("AddVectoredExceptionHandler", &add_veh) ||
            add_veh(/*first=*/1, DemandCommitHandler) == nullptr) {
            LOG_ERROR(HW_Memory, "VirtualBuffer demand-commit unavailable, committing eagerly");
            return false;
        }
        return true;
    }();
    return available;
}

bool RegisterRange(void* base, std::size_t size) noexcept {
    std::scoped_lock lock{g_ranges_mutex};
    for (Range& range : g_ranges) {
        if (range.base.load(std::memory_order_relaxed) == 0) {
            range.size.store(size, std::memory_order_relaxed);
            range.base.store(reinterpret_cast<std::uintptr_t>(base), std::memory_order_release);
            return true;
        }
    }
    return false;
}

void UnregisterRange(void* base) noexcept {
    std::scoped_lock lock{g_ranges_mutex};
    for (Range& range : g_ranges) {
        if (range.base.load(std::memory_order_relaxed) == reinterpret_cast<std::uintptr_t>(base)) {
            range.base.store(0, std::memory_order_release);
            range.size.store(0, std::memory_order_relaxed);
            return;
        }
    }
}

void* ReserveDemandCommitted(std::size_t size) noexcept {
    void* base = g_virtual_alloc_from_app(nullptr, size, MEM_RESERVE, PAGE_READWRITE);
    if (base != nullptr && !RegisterRange(base, size)) {
        LOG_WARNING(HW_Memory, "VirtualBuffer range registry full, committing {} MiB eagerly",
                    size >> 20);
        if (!g_virtual_alloc_from_app(base, size, MEM_COMMIT, PAGE_READWRITE)) {
            VirtualFree(base, 0, MEM_RELEASE);
            base = nullptr;
        }
    }
    return base;
}

} // namespace
#endif

void* AllocateMemoryPages(std::size_t size) noexcept {
#ifdef _WIN32
#ifdef YUZU_UWP_APPCONTAINER
    if (size >= DEMAND_COMMIT_THRESHOLD && DemandCommitAvailable()) {
        void* const reserved = ReserveDemandCommitted(size);
        ASSERT_MSG(reserved, "failed to reserve {} MiB for a VirtualBuffer", size >> 20);
        return reserved;
    }
#endif
    void* base = VirtualAlloc(nullptr, size, MEM_COMMIT, PAGE_READWRITE);
#else
    void* base = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (base == MAP_FAILED)
        base = nullptr;
#endif
    ASSERT(base);
    return base;
}

void FreeMemoryPages(void* base, [[maybe_unused]] std::size_t size) noexcept {
    if (!base)
        return;
#ifdef _WIN32
#ifdef YUZU_UWP_APPCONTAINER
    UnregisterRange(base);
#endif
    ASSERT(VirtualFree(base, 0, MEM_RELEASE));
#else
    ASSERT(munmap(base, size) == 0);
#endif
}

} // namespace Common
