// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef _WIN32

#include <cstdio>
#include <iterator>
#include <string>
#include "common/container/unordered_map.h"
#include <boost/icl/separate_interval_set.hpp>
#include <windows.h>
#include <winioctl.h>
#include "common/dynamic_library.h"

#else // ^^^ Windows ^^^ vvv POSIX vvv

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <boost/icl/interval_set.hpp>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include "common/scope_exit.h"

#if defined(__linux__)
#include <sys/random.h>
#elif defined(__APPLE__)
#include <sys/types.h>
#include <sys/random.h>
#include <mach/vm_map.h>
#include <mach/mach.h>
#elif defined(__FreeBSD__)
#include <sys/shm.h>
#elif defined(__OPENORBIS__)
#include <orbis/libkernel.h>
#endif

// FreeBSD
#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif
// Solaris 11 and illumos
#ifndef MAP_ALIGNED_SUPER
#define MAP_ALIGNED_SUPER 0
#endif
// macOS
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

#endif // ^^^ POSIX ^^^

#include <atomic>
#include <array>
#include <mutex>
#include <random>

#include "common/alignment.h"
#include "common/assert.h"
#include "common/free_region_manager.h"
#include "common/host_memory.h"
#include "common/logging.h"
#include "common/settings.h"

#if defined(__ANDROID__) && __ANDROID_API__ < 30
#include <sys/syscall.h>
#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif
static int memfd_create(const char* name, unsigned int flags) {
    return syscall(__NR_memfd_create, name, flags);
}
#endif

namespace Common {

[[maybe_unused]] constexpr size_t PageAlignment = 0x1000;
[[maybe_unused]] constexpr size_t HugePageSize = 0x200000;

namespace {
std::atomic<size_t> g_fastmem_hot_size{size_t{384} << 20};
std::atomic<bool> g_fastmem_force_full{false};
}

void ConfigureHostMemoryFastmem(size_t hot_mib, bool force_full) {
    g_fastmem_hot_size.store(std::clamp(hot_mib, size_t{128}, size_t{448}) << 20,
                             std::memory_order_release);
    g_fastmem_force_full.store(force_full, std::memory_order_release);
}

size_t HostMemoryFastmemHotSize() {
    return g_fastmem_hot_size.load(std::memory_order_acquire);
}

bool HostMemoryFastmemForceFull() {
    return g_fastmem_force_full.load(std::memory_order_acquire);
}

#ifdef _WIN32

// Manually imported for MinGW compatibility
#ifndef MEM_RESERVE_PLACEHOLDER
#define MEM_RESERVE_PLACEHOLDER 0x00040000
#endif
#ifndef MEM_REPLACE_PLACEHOLDER
#define MEM_REPLACE_PLACEHOLDER 0x00004000
#endif
#ifndef MEM_COALESCE_PLACEHOLDERS
#define MEM_COALESCE_PLACEHOLDERS 0x00000001
#endif
#ifndef MEM_PRESERVE_PLACEHOLDER
#define MEM_PRESERVE_PLACEHOLDER 0x00000002
#endif

// On the Xbox/UWP AppContainer the placeholder virtual-memory APIs used for the emulated DRAM must go
// through the sandbox-legal *FromApp variants. The signatures of VirtualAlloc2FromApp and
// MapViewOfFile3FromApp match their base versions (we just resolve a different export); only the file
// mapping differs (CreateFileMappingFromApp has no extended-parameter form, so it takes the page
// protection OR'd with SEC_COMMIT directly). VirtualFree/VirtualFreeEx/UnmapViewOfFile2/CloseHandle
// are permitted in the AppContainer and are left unchanged.
//
// Auto-detected from the Windows SDK app-family partition (a UWP/WindowsStore CMake target defines
// WINAPI_FAMILY == WINAPI_FAMILY_APP); force it for a desktop compile/run check with
// -DYUZU_UWP_APPCONTAINER. The *FromApp exports also exist on desktop Win10+, so the forced build is
// runtime-valid there too.
// VirtualProtectFromApp is resolved dynamically (like the other kernelbase entry points below)
// rather than called directly: a direct call would pull in the UWP/onecore import library, whereas
// GetProcAddress keeps this translation unit free of link-time dependencies on the target's link set.
// The plain desktop path keeps calling VirtualProtect directly, so it is byte-for-byte unchanged.
#if defined(YUZU_UWP_APPCONTAINER) || (defined(WINAPI_FAMILY) && WINAPI_FAMILY == WINAPI_FAMILY_APP)
#define HOST_MEMORY_USE_FROM_APP 1
#define HOST_MEMORY_VIRTUAL_PROTECT pfn_VirtualProtectFromApp
#else
#define HOST_MEMORY_VIRTUAL_PROTECT VirtualProtect
#endif

using PFN_CreateFileMapping2 = _Ret_maybenull_ HANDLE(WINAPI*)(
    _In_ HANDLE File, _In_opt_ SECURITY_ATTRIBUTES* SecurityAttributes, _In_ ULONG DesiredAccess,
    _In_ ULONG PageProtection, _In_ ULONG AllocationAttributes, _In_ ULONG64 MaximumSize,
    _In_opt_ PCWSTR Name,
    _Inout_updates_opt_(ParameterCount) MEM_EXTENDED_PARAMETER* ExtendedParameters,
    _In_ ULONG ParameterCount);

using PFN_VirtualAlloc2 = _Ret_maybenull_ PVOID(WINAPI*)(
    _In_opt_ HANDLE Process, _In_opt_ PVOID BaseAddress, _In_ SIZE_T Size,
    _In_ ULONG AllocationType, _In_ ULONG PageProtection,
    _Inout_updates_opt_(ParameterCount) MEM_EXTENDED_PARAMETER* ExtendedParameters,
    _In_ ULONG ParameterCount);

using PFN_MapViewOfFile3 = _Ret_maybenull_ PVOID(WINAPI*)(
    _In_ HANDLE FileMapping, _In_opt_ HANDLE Process, _In_opt_ PVOID BaseAddress,
    _In_ ULONG64 Offset, _In_ SIZE_T ViewSize, _In_ ULONG AllocationType, _In_ ULONG PageProtection,
    _Inout_updates_opt_(ParameterCount) MEM_EXTENDED_PARAMETER* ExtendedParameters,
    _In_ ULONG ParameterCount);

using PFN_UnmapViewOfFile2 = BOOL(WINAPI*)(_In_ HANDLE Process, _In_ PVOID BaseAddress,
                                           _In_ ULONG UnmapFlags);

// AppContainer-legal file mapping. Unlike CreateFileMapping2 it has no extended-parameter form; the
// page protection carries SEC_COMMIT directly (the same shape as classic CreateFileMapping).
using PFN_CreateFileMappingFromApp = _Ret_maybenull_ HANDLE(WINAPI*)(
    _In_ HANDLE File, _In_opt_ PSECURITY_ATTRIBUTES SecurityAttributes, _In_ ULONG PageProtection,
    _In_ ULONG64 MaximumSize, _In_opt_ PCWSTR Name);

// VirtualProtectFromApp — identical shape to VirtualProtect; resolved by name under the AppContainer.
using PFN_VirtualProtect = BOOL(WINAPI*)(_In_ LPVOID Address, _In_ SIZE_T Size,
                                         _In_ DWORD NewProtection, _Out_ PDWORD OldProtection);

// VirtualAllocFromApp — reserves the private DRAM backing and demand-commits its pages (see below).
using PFN_VirtualAllocFromApp = _Ret_maybenull_ PVOID(WINAPI*)(
    _In_opt_ PVOID BaseAddress, _In_ SIZE_T Size, _In_ ULONG AllocationType, _In_ ULONG Protection);

// Vectored-exception-handler APIs. Resolved by name (not called directly) so the binary does not hard-
// import api-ms-win-core-errorhandling-l1-1-1.dll — an api-set the Xbox AppContainer loader does not
// expose, which would make the app fail to ACTIVATE (no process, no crash dump). Kernelbase exports
// both by name, so they resolve at runtime even though the api-set is absent.
using PFN_AddVectoredExceptionHandler =
    PVOID(WINAPI*)(_In_ ULONG First, _In_ PVECTORED_EXCEPTION_HANDLER Handler);
using PFN_RemoveVectoredExceptionHandler = ULONG(WINAPI*)(_In_ PVOID Handle);

template <typename T>
static void GetFuncAddress(Common::DynamicLibrary& dll, const char* name, T& pfn) {
    if (!dll.GetSymbol(name, &pfn)) {
        LOG_CRITICAL(HW_Memory, "Failed to load {}", name);
    }
}

// Map/MapView also compile on desktop Windows. Their accounting must share the
// same platform scope even though hybrid fastmem is initialized only in UWP.
namespace {
std::atomic<u64> g_fastmem_canonical_bytes{0};
std::atomic<u64> g_fastmem_alias_bytes{0};
std::atomic<u64> g_fastmem_alias_budget{0};
std::atomic<u64> g_fastmem_guest_bytes{0};
std::atomic<u64> g_fastmem_skipped_bytes{0};
std::atomic<u64> g_fastmem_map_failures{0};
std::atomic<u64> g_fastmem_hot_offset{0};
std::atomic<u64> g_fastmem_hot_size_actual{0};
std::atomic<u64> g_fastmem_request_bytes{0};
std::atomic<u64> g_fastmem_request_min{~u64{0}};
std::atomic<u64> g_fastmem_request_max{0};
constexpr size_t FastmemHistogramShift = 26;
constexpr size_t FastmemHistogramBuckets = 64;
std::array<std::atomic<u64>, FastmemHistogramBuckets> g_fastmem_request_histogram{};
} // namespace

#ifdef HOST_MEMORY_USE_FROM_APP
// Demand-commit for the Xbox/UWP backing. The backing is PRIVATE memory reserved (MEM_RESERVE) but
// not eagerly committed — an eager 4 GiB commit busts the Series-S dev-app commit budget, and only
// MEM_PRIVATE pages can be committed on demand (a MEM_MAPPED section view cannot). The first access
// to a reserved-but-uncommitted page faults; this vectored handler commits the faulting page's chunk
// on demand, so only what the guest/kernel actually touches is charged — a tiny homebrew NRO needs a
// few MiB, while the kernel's full 4 GiB memory-pool layout stays valid. (CORE: Series-S memory.)
//
// There is exactly one emulated-DRAM backing (Core::DeviceMemory), so a single file-scope range +
// committer suffices; the handler is allocation-free and re-entrancy-safe (atomic loads + one commit).
//
// With fastmem the backing is instead a SEC_RESERVE section: its views (the linear backing view and
// the ones mapped into the fastmem arena) are MEM_MAPPED, and committing a page through any view
// commits the section page for all of them. The 4 GiB SEC_COMMIT section the desktop path uses does
// not fit the console's commit budget (ERROR_COMMITMENT_LIMIT, measured by the 0.2.51 probe).
namespace {
std::atomic<u8*> g_backing_base{nullptr};
std::atomic<size_t> g_backing_size{0};
std::atomic<u8*> g_arena_base{nullptr};
std::atomic<size_t> g_arena_size{0};
std::atomic<bool> g_backing_is_section{false};
std::atomic<bool> g_backing_is_file{false};
std::atomic<bool> g_backing_file_sparse{false};
PFN_VirtualAllocFromApp g_pfn_virtual_alloc_from_app{nullptr};
PFN_AddVectoredExceptionHandler g_pfn_add_veh{nullptr};
PFN_RemoveVectoredExceptionHandler g_pfn_remove_veh{nullptr};
void* g_backing_veh{nullptr};

constexpr size_t BACKING_COMMIT_GRANULARITY = 64 * 1024; // commit in 64 KiB chunks to limit faults
constexpr uintptr_t HOST_PAGE_SIZE = 4096;

// Section commit accounting for the diag (0.2.55 died with commit far below the app limit, so the
// error the console returns is what is missing). Written from the handler: atomics only.
std::atomic<u64> g_section_commit_bytes{0};
std::atomic<u64> g_commit_failures{0};
std::atomic<u64> g_page_retry_saves{0};
std::atomic<u32> g_last_commit_error{0};
std::atomic<uintptr_t> g_last_failed_address{0};
std::atomic<u64> g_last_failed_length{0};
bool CommitSectionRange(uintptr_t begin, uintptr_t end) {
    if (g_pfn_virtual_alloc_from_app(reinterpret_cast<void*>(begin), end - begin, MEM_COMMIT,
                                     PAGE_READWRITE)) {
        g_section_commit_bytes.fetch_add(end - begin, std::memory_order_relaxed);
        return true;
    }
    g_last_commit_error.store(GetLastError(), std::memory_order_relaxed);
    g_last_failed_address.store(begin, std::memory_order_relaxed);
    g_last_failed_length.store(end - begin, std::memory_order_relaxed);
    g_commit_failures.fetch_add(1, std::memory_order_relaxed);
    return false;
}

// Section mode: commits the reserved pages of the view around the fault. A committed read-write
// page means another thread committed it first, so the access is retried; anything else (an
// unmapped arena placeholder, a page the rasterizer protected) is left to dynarmic's handler.
LONG SectionDemandCommit(u8* fault_addr) {
    const uintptr_t page = reinterpret_cast<uintptr_t>(fault_addr) & ~(HOST_PAGE_SIZE - 1);
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(reinterpret_cast<void*>(page), &info, sizeof(info)) == 0 ||
        info.Type != MEM_MAPPED) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (info.State == MEM_COMMIT) {
        return info.Protect == PAGE_READWRITE ? EXCEPTION_CONTINUE_EXECUTION
                                              : EXCEPTION_CONTINUE_SEARCH;
    }
    if (info.State != MEM_RESERVE) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const uintptr_t region_end = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
    const uintptr_t chunk_end = (page + BACKING_COMMIT_GRANULARITY) & ~(BACKING_COMMIT_GRANULARITY - 1);
    const uintptr_t end = (std::min)(region_end, chunk_end);
    if (CommitSectionRange(page, end)) {
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    // The chunk failed: try the faulting page alone before giving up.
    if (end - page > HOST_PAGE_SIZE && CommitSectionRange(page, page + HOST_PAGE_SIZE)) {
        g_page_retry_saves.fetch_add(1, std::memory_order_relaxed);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

LONG NTAPI BackingDemandCommitHandler(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    u8* const base = g_backing_base.load(std::memory_order_acquire);
    const size_t size = g_backing_size.load(std::memory_order_acquire);
    if (base == nullptr || g_pfn_virtual_alloc_from_app == nullptr) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const auto fault_addr =
        reinterpret_cast<u8*>(ep->ExceptionRecord->ExceptionInformation[1]); // [1] = faulting address
    if (g_backing_is_section.load(std::memory_order_acquire)) {
        u8* const arena = g_arena_base.load(std::memory_order_acquire);
        const size_t arena_size = g_arena_size.load(std::memory_order_acquire);
        const bool in_backing = fault_addr >= base && fault_addr < base + size;
        const bool in_arena = arena != nullptr && fault_addr >= arena && fault_addr < arena + arena_size;
        if (in_backing || in_arena) {
            MEMORY_BASIC_INFORMATION info{};
            if (VirtualQuery(fault_addr, &info, sizeof(info)) != 0 && info.Type == MEM_MAPPED) {
                return SectionDemandCommit(fault_addr);
            }
        }
        // Hybrid backing: its prefix and suffix are private reservations. Arena holes must keep
        // faulting so Dynarmic patches those accesses to the page-table path.
        if (!in_backing) {
            return EXCEPTION_CONTINUE_SEARCH;
        }
    }
    if (fault_addr < base || fault_addr >= base + size) {
        return EXCEPTION_CONTINUE_SEARCH; // not our backing — let other handlers run
    }
    const size_t offset = static_cast<size_t>(fault_addr - base);
    const size_t chunk_offset = offset & ~(BACKING_COMMIT_GRANULARITY - 1);
    const size_t chunk_len = (std::min)(BACKING_COMMIT_GRANULARITY, size - chunk_offset);
    if (g_pfn_virtual_alloc_from_app(base + chunk_offset, chunk_len, MEM_COMMIT, PAGE_READWRITE)) {
        return EXCEPTION_CONTINUE_EXECUTION; // page committed — retry the faulting access
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
} // namespace
#endif

class HostMemory::Impl {
public:
    explicit Impl(size_t backing_size_, size_t virtual_size_, HostMemoryFastmemRegion fastmem_region_)
        : backing_size{backing_size_}
        , virtual_size{virtual_size_}
        , fastmem_region{fastmem_region_}
        , process{GetCurrentProcess()}
        , kernelbase_dll("Kernelbase")
    {}

    bool Init() {
        if (!kernelbase_dll.IsOpen()) {
            LOG_CRITICAL(HW_Memory, "Failed to load Kernelbase.dll");
            return false;
        }
#ifdef HOST_MEMORY_USE_FROM_APP
        GetFuncAddress(kernelbase_dll, "CreateFileMappingFromApp", pfn_CreateFileMappingFromApp);
        GetFuncAddress(kernelbase_dll, "VirtualAlloc2FromApp", pfn_VirtualAlloc2);
        GetFuncAddress(kernelbase_dll, "MapViewOfFile3FromApp", pfn_MapViewOfFile3);
        GetFuncAddress(kernelbase_dll, "VirtualProtectFromApp", pfn_VirtualProtectFromApp);
#else
        GetFuncAddress(kernelbase_dll, "CreateFileMapping2", pfn_CreateFileMapping2);
        GetFuncAddress(kernelbase_dll, "VirtualAlloc2", pfn_VirtualAlloc2);
        GetFuncAddress(kernelbase_dll, "MapViewOfFile3", pfn_MapViewOfFile3);
#endif
        GetFuncAddress(kernelbase_dll, "UnmapViewOfFile2", pfn_UnmapViewOfFile2);

        // Allocate the linear DRAM backing.
#ifdef HOST_MEMORY_USE_FROM_APP
        // Xbox/UWP: back the emulated DRAM with demand-committable PRIVATE memory, not a file-mapping
        // section. The section design exists only to alias the backing into virtual_base for fastmem
        // — which is OFF on UWP (fastmem_arena is always null), so Map()/virtual_base are never used
        // here (every Map/Unmap/VirtualBasePointer caller is fastmem- or NCE-gated, both off on UWP).
        // Critically, ONLY MEM_PRIVATE pages can be committed on demand: VirtualAllocFromApp(MEM_COMMIT)
        // cannot commit pages of a MEM_MAPPED section view in the AppContainer — that is why an earlier
        // SEC_RESERVE section + demand-commit handler access-violated on-console. So reserve private
        // address space now (no eager commit — a 4 GiB SEC_COMMIT busts the Series-S dev-app budget);
        // the handler below commits 64 KiB chunks on first touch, charging only the touched working
        // set while the full 4 GiB stays reserved (kernel memory-pool layout unchanged).
        GetFuncAddress(kernelbase_dll, "VirtualAllocFromApp", g_pfn_virtual_alloc_from_app);
        // Resolve the VEH APIs by name (see typedef note) so we do not hard-import the errorhandling
        // api-set the Xbox loader lacks — a direct call there makes the app fail to activate on-console.
        GetFuncAddress(kernelbase_dll, "AddVectoredExceptionHandler", g_pfn_add_veh);
        GetFuncAddress(kernelbase_dll, "RemoveVectoredExceptionHandler", g_pfn_remove_veh);
        if (Settings::values.cpuopt_fastmem.GetValue()) {
            if (fastmem_region.force_full && InitSection()) {
                return true;
            }
            if (fastmem_region.force_full) {
                LOG_WARNING(HW_Memory, "Full fastmem unavailable, trying the hybrid arena");
                Release();
            }
            if (InitHybridSection()) {
                return true;
            }
            LOG_WARNING(HW_Memory, "Hybrid fastmem unavailable, using the private backing");
            Release();
        }
        backing_base = static_cast<u8*>(
            g_pfn_virtual_alloc_from_app(nullptr, backing_size, MEM_RESERVE, PAGE_READWRITE));
        if (backing_base == nullptr) {
            LOG_CRITICAL(HW_Memory, "Failed to reserve {} MiB of backing memory", backing_size >> 20);
            return false;
        }
        g_backing_base.store(backing_base, std::memory_order_release);
        g_backing_size.store(backing_size, std::memory_order_release);
        g_backing_veh = g_pfn_add_veh(/*first=*/1, BackingDemandCommitHandler);
        if (g_backing_veh == nullptr) {
            Release();
            LOG_CRITICAL(HW_Memory, "Failed to install backing demand-commit handler");
            return false;
        }
        // No file-mapping section and no fastmem arena on UWP. VirtualBasePointer() returns null;
        // that is tolerated by all callers (fastmem disabled).
        backing_handle = nullptr;
        virtual_base = nullptr;
        return true;
#else
        if (!pfn_CreateFileMapping2 || !pfn_VirtualAlloc2 || !pfn_MapViewOfFile3 || !pfn_UnmapViewOfFile2) {
            LOG_CRITICAL(HW_Memory, "Failed to find functions for virtual allocs");
            return false;
        }

        // Allocate backing file map
        backing_handle = pfn_CreateFileMapping2(INVALID_HANDLE_VALUE, nullptr, FILE_MAP_WRITE | FILE_MAP_READ, PAGE_READWRITE, SEC_COMMIT, backing_size, nullptr, nullptr, 0);
        if (!backing_handle) {
            LOG_CRITICAL(HW_Memory, "Failed to allocate {} MiB of backing memory", backing_size >> 20);
            return false;
        }
        // Allocate a virtual memory for the backing file map as placeholder
        backing_base = static_cast<u8*>(pfn_VirtualAlloc2(process, nullptr, backing_size, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0));
        if (!backing_base) {
            Release();
            LOG_CRITICAL(HW_Memory, "Failed to reserve {} MiB of virtual memory", backing_size >> 20);
            return false;
        }
        // Map backing placeholder
        void* const ret = pfn_MapViewOfFile3(backing_handle, process, backing_base, 0, backing_size, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0);
        if (ret != backing_base) {
            Release();
            LOG_CRITICAL(HW_Memory, "Failed to map {} MiB of virtual memory", backing_size >> 20);
            return false;
        }
        // Allocate virtual address placeholder
        virtual_base = static_cast<u8*>(pfn_VirtualAlloc2(process, nullptr, virtual_size, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0));
        if (!virtual_base) {
            Release();
            LOG_CRITICAL(HW_Memory, "Failed to reserve {} GiB of virtual memory", virtual_size >> 30);
            return false;
        }
        return true;
#endif
    }

    ~Impl() {
        Release();
    }

#ifdef HOST_MEMORY_USE_FROM_APP
    /// Xbox fastmem: only a bounded hot slice of the application pool is section-backed. The private
    /// reservations and the mapped section remain adjacent, so every existing BackingBasePointer
    /// user still sees one linear DRAM allocation. Only the section-backed range can be aliased.
    bool InitHybridSection() {
        if (!pfn_CreateFileMappingFromApp || !pfn_VirtualAlloc2 || !pfn_MapViewOfFile3 ||
            !pfn_UnmapViewOfFile2 || !g_pfn_virtual_alloc_from_app || !g_pfn_add_veh) {
            return false;
        }
        hybrid_hot_offset = AlignDown(fastmem_region.offset, size_t{64} << 10);
        hybrid_hot_size = AlignDown(fastmem_region.size, size_t{64} << 10);
        if (hybrid_hot_size == 0 || hybrid_hot_offset >= backing_size ||
            hybrid_hot_size > backing_size - hybrid_hot_offset) {
            LOG_WARNING(HW_Memory, "Invalid hybrid fastmem range at {:#x}, size {:#x}",
                        hybrid_hot_offset, hybrid_hot_size);
            return false;
        }
        hybrid_alias_budget =
            (std::min)(MaxHybridAliasBudget, HybridTotalViewBudget - hybrid_hot_size);

        backing_file = OpenBackingFile();
        if (backing_file == INVALID_HANDLE_VALUE) {
            return false;
        }
        backing_handle = pfn_CreateFileMappingFromApp(backing_file, nullptr, PAGE_READWRITE,
                                                      hybrid_hot_size, nullptr);
        if (!backing_handle) {
            LOG_WARNING(HW_Memory, "Failed to create the hybrid fastmem section, error {}",
                        GetLastError());
            return false;
        }
        backing_base = static_cast<u8*>(pfn_VirtualAlloc2(
            process, nullptr, backing_size, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS,
            nullptr, 0));
        if (!backing_base) {
            LOG_WARNING(HW_Memory, "Failed to reserve the hybrid backing, error {}", GetLastError());
            return false;
        }
        hybrid = true;

        // Split [private prefix | mapped hot section | private suffix].
        if (hybrid_hot_offset != 0 &&
            !VirtualFree(backing_base, hybrid_hot_offset,
                         MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) {
            LOG_WARNING(HW_Memory, "Failed to split the hybrid backing prefix, error {}",
                        GetLastError());
            return false;
        }
        hybrid_split_prefix = hybrid_hot_offset != 0;
        if (hybrid_hot_offset + hybrid_hot_size != backing_size &&
            !VirtualFree(backing_base + hybrid_hot_offset, hybrid_hot_size,
                         MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) {
            LOG_WARNING(HW_Memory, "Failed to split the hybrid backing suffix, error {}",
                        GetLastError());
            return false;
        }
        hybrid_split_suffix = hybrid_hot_offset + hybrid_hot_size != backing_size;
        if (hybrid_hot_offset != 0 &&
            pfn_VirtualAlloc2(process, backing_base, hybrid_hot_offset,
                              MEM_RESERVE | MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr,
                              0) != backing_base) {
            LOG_WARNING(HW_Memory, "Failed to reserve the hybrid private prefix, error {}",
                        GetLastError());
            return false;
        }
        hybrid_prefix_private = hybrid_hot_offset != 0;
        const size_t suffix_offset = hybrid_hot_offset + hybrid_hot_size;
        if (suffix_offset != backing_size &&
            pfn_VirtualAlloc2(process, backing_base + suffix_offset, backing_size - suffix_offset,
                              MEM_RESERVE | MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr,
                              0) != backing_base + suffix_offset) {
            LOG_WARNING(HW_Memory, "Failed to reserve the hybrid private suffix, error {}",
                        GetLastError());
            return false;
        }
        hybrid_suffix_private = suffix_offset != backing_size;
        if (pfn_MapViewOfFile3(backing_handle, process, backing_base + hybrid_hot_offset, 0,
                               hybrid_hot_size, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr,
                               0) != backing_base + hybrid_hot_offset) {
            LOG_WARNING(HW_Memory, "Failed to map the hybrid canonical view, error {}",
                        GetLastError());
            return false;
        }
        hybrid_canonical_mapped = true;

        virtual_base = static_cast<u8*>(pfn_VirtualAlloc2(
            process, nullptr, virtual_size, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS,
            nullptr, 0));
        if (!virtual_base) {
            LOG_WARNING(HW_Memory, "Failed to reserve the hybrid fastmem arena, error {}",
                        GetLastError());
            return false;
        }

        // Exercise coherence before exposing the arena to Dynarmic. This catches a section/view
        // incompatibility here and preserves the all-private fallback instead of failing in-game.
        constexpr size_t VerifySize = size_t{64} << 10;
        bool verify_ok = VirtualFree(virtual_base, VerifySize,
                                     MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER) != FALSE;
        if (verify_ok) {
            verify_ok = pfn_MapViewOfFile3(backing_handle, process, virtual_base, 0, VerifySize,
                                           MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr,
                                           0) == virtual_base;
        }
        if (verify_ok) {
            volatile u8* const canonical = backing_base + hybrid_hot_offset;
            volatile u8* const alias = virtual_base;
            const u8 original = canonical[0];
            canonical[0] = 0xA5;
            verify_ok = alias[0] == 0xA5;
            alias[0] = 0x5A;
            verify_ok = verify_ok && canonical[0] == 0x5A;
            canonical[0] = original;
            pfn_UnmapViewOfFile2(process, virtual_base, MEM_PRESERVE_PLACEHOLDER);
        }
        const bool coalesced =
            VirtualFree(virtual_base, virtual_size,
                        MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS) != FALSE;
        verify_ok = verify_ok && coalesced;
        if (!verify_ok) {
            LOG_WARNING(HW_Memory, "Hybrid fastmem coherence check failed, error {}",
                        GetLastError());
            return false;
        }

        g_backing_base.store(backing_base, std::memory_order_release);
        g_backing_size.store(backing_size, std::memory_order_release);
        g_arena_base.store(virtual_base, std::memory_order_release);
        g_arena_size.store(virtual_size, std::memory_order_release);
        g_backing_is_section.store(true, std::memory_order_release);
        g_fastmem_canonical_bytes.store(hybrid_hot_size, std::memory_order_release);
        g_fastmem_alias_bytes.store(0, std::memory_order_release);
        g_fastmem_alias_budget.store(hybrid_alias_budget, std::memory_order_release);
        g_fastmem_guest_bytes.store(0, std::memory_order_release);
        g_fastmem_skipped_bytes.store(0, std::memory_order_release);
        g_fastmem_map_failures.store(0, std::memory_order_release);
        g_fastmem_request_bytes.store(0, std::memory_order_release);
        g_fastmem_request_min.store(~u64{0}, std::memory_order_release);
        g_fastmem_request_max.store(0, std::memory_order_release);
        for (auto& bucket : g_fastmem_request_histogram) {
            bucket.store(0, std::memory_order_release);
        }
        g_fastmem_hot_offset.store(hybrid_hot_offset, std::memory_order_release);
        g_fastmem_hot_size_actual.store(hybrid_hot_size, std::memory_order_release);
        g_backing_veh = g_pfn_add_veh(/*first=*/1, BackingDemandCommitHandler);
        if (!g_backing_veh) {
            LOG_WARNING(HW_Memory, "Failed to install the hybrid demand-commit handler");
            return false;
        }
        LOG_INFO(HW_Memory,
                 "Hybrid fastmem: {} MiB at DRAM offset {:#x}, {} GiB arena, alias budget {} MiB",
                 hybrid_hot_size >> 20, hybrid_hot_offset, virtual_size >> 30,
                 hybrid_alias_budget >> 20);
        return true;
    }

    /// Fastmem layout for the AppContainer: the DRAM is a SEC_RESERVE section mapped once as the
    /// linear backing, plus the address-space placeholder that Map() fills with views of it. Pages
    /// are committed on first touch by the demand-commit handler.
    bool InitSection() {
        if (!pfn_CreateFileMappingFromApp || !pfn_VirtualAlloc2 || !pfn_MapViewOfFile3 ||
            !pfn_UnmapViewOfFile2 || !g_pfn_virtual_alloc_from_app || !g_pfn_add_veh) {
            LOG_CRITICAL(HW_Memory, "Failed to find functions for the fastmem arena");
            return false;
        }
        // A pagefile-backed section charges system commit, of which the console leaves the app
        // about 1.1 GiB in all sections (0.2.56 probe), and the game ran out a minute in. A section
        // over a real file charges no commit: its pages are backed by the file.
        backing_file = OpenBackingFile();
        if (backing_file == INVALID_HANDLE_VALUE) {
            return false;
        }
        backing_handle = pfn_CreateFileMappingFromApp(backing_file, nullptr, PAGE_READWRITE,
                                                      backing_size, nullptr);
        if (!backing_handle) {
            LOG_CRITICAL(HW_Memory, "Failed to create a {} MiB file-backed section, error {}",
                         backing_size >> 20, GetLastError());
            return false;
        }
        backing_base = static_cast<u8*>(pfn_VirtualAlloc2(process, nullptr, backing_size,
                                                          MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
                                                          PAGE_NOACCESS, nullptr, 0));
        if (!backing_base) {
            LOG_CRITICAL(HW_Memory, "Failed to reserve {} MiB of virtual memory, error {}",
                         backing_size >> 20, GetLastError());
            return false;
        }
        if (!MapBackingViews()) {
            return false;
        }
        virtual_base = static_cast<u8*>(pfn_VirtualAlloc2(process, nullptr, virtual_size,
                                                          MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
                                                          PAGE_NOACCESS, nullptr, 0));
        if (!virtual_base) {
            LOG_CRITICAL(HW_Memory, "Failed to reserve {} GiB of virtual memory, error {}",
                         virtual_size >> 30, GetLastError());
            return false;
        }
        g_backing_base.store(backing_base, std::memory_order_release);
        g_backing_size.store(backing_size, std::memory_order_release);
        g_arena_base.store(virtual_base, std::memory_order_release);
        g_arena_size.store(virtual_size, std::memory_order_release);
        g_backing_is_section.store(true, std::memory_order_release);
        g_backing_veh = g_pfn_add_veh(/*first=*/1, BackingDemandCommitHandler);
        if (g_backing_veh == nullptr) {
            LOG_CRITICAL(HW_Memory, "Failed to install backing demand-commit handler");
            return false;
        }
        LOG_INFO(HW_Memory, "Fastmem arena: {} GiB reserved over a {} MiB file-backed section",
                 virtual_size >> 30, backing_size >> 20);
        return true;
    }

    /// Maps the section over the backing placeholder: one view, or, when the console refuses a
    /// view that large (error 8 for 4 GiB of a file, 0.2.56.1), one view per piece of the
    /// placeholder, split first.
    bool MapBackingViews() {
        if (pfn_MapViewOfFile3(backing_handle, process, backing_base, 0, backing_size,
                               MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr,
                               0) == backing_base) {
            backing_mapped = true;
            return true;
        }
        const DWORD whole_error = GetLastError();
        for (const size_t piece : {size_t{1} << 30, size_t{256} << 20}) {
            if (backing_size % piece != 0) {
                continue;
            }
            const size_t count = backing_size / piece;
            size_t mapped = 0;
            DWORD error = 0;
            for (; mapped < count; ++mapped) {
                u8* const address = backing_base + mapped * piece;
                if (mapped + 1 < count &&
                    !VirtualFree(address, piece, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) {
                    error = GetLastError();
                    break;
                }
                if (pfn_MapViewOfFile3(backing_handle, process, address, mapped * piece, piece,
                                       MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr,
                                       0) != address) {
                    error = GetLastError();
                    break;
                }
            }
            backing_piece = piece;
            backing_pieces = mapped;
            if (mapped == count) {
                LOG_INFO(HW_Memory,
                         "A {} MiB view of the DRAM failed (error {}), mapped it as {} views of "
                         "{} MiB",
                         backing_size >> 20, whole_error, count, piece >> 20);
                return true;
            }
            LOG_WARNING(HW_Memory, "Mapping the DRAM as {} MiB views stopped at {} of {}, error {}",
                        piece >> 20, mapped, count, error);
            UnmapBackingViews(); // the placeholder is split now: release it and start over
            backing_base = static_cast<u8*>(pfn_VirtualAlloc2(
                process, nullptr, backing_size, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
                PAGE_NOACCESS, nullptr, 0));
            if (!backing_base) {
                return false;
            }
        }
        LOG_CRITICAL(HW_Memory, "Failed to map {} MiB of backing memory, error {}",
                     backing_size >> 20, whole_error);
        return false;
    }

    /// Unmaps the backing views and releases the backing placeholder, whole or in pieces.
    void UnmapBackingViews() {
        if (!backing_base) {
            return;
        }
        if (backing_piece != 0) {
            const size_t count = backing_size / backing_piece;
            for (size_t i = 0; i < count; ++i) {
                u8* const address = backing_base + i * backing_piece;
                if (i < backing_pieces) {
                    pfn_UnmapViewOfFile2(process, address, MEM_PRESERVE_PLACEHOLDER);
                }
                // Past the last split the rest is one placeholder: only its first piece is an
                // allocation base, and releasing inside it just fails.
                VirtualFree(address, 0, MEM_RELEASE);
            }
        } else {
            if (backing_mapped) {
                pfn_UnmapViewOfFile2(process, backing_base, MEM_PRESERVE_PLACEHOLDER);
            }
            VirtualFreeEx(process, backing_base, 0, MEM_RELEASE);
        }
        backing_base = nullptr;
        backing_mapped = false;
        backing_piece = 0;
        backing_pieces = 0;
    }

    /// The file behind the DRAM section: in the app's temp folder, deleted when the handle closes
    /// (also when the process dies), marked temporary so the OS keeps its pages in memory rather
    /// than writing them out, and sparse so pages written far into it do not make the file system
    /// zero everything before them on disk.
    HANDLE OpenBackingFile() {
        using PFN_CreateFile2 = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, DWORD,
                                                LPCREATEFILE2_EXTENDED_PARAMETERS);
        using PFN_GetTempPathW = DWORD(WINAPI*)(DWORD, LPWSTR);
        using PFN_DeviceIoControl = BOOL(WINAPI*)(HANDLE, DWORD, LPVOID, DWORD, LPVOID, DWORD,
                                                  LPDWORD, LPOVERLAPPED);
        PFN_CreateFile2 create_file2{};
        PFN_GetTempPathW get_temp_path{};
        PFN_DeviceIoControl device_io_control{};
        GetFuncAddress(kernelbase_dll, "CreateFile2", create_file2);
        GetFuncAddress(kernelbase_dll, "GetTempPathW", get_temp_path);
        GetFuncAddress(kernelbase_dll, "DeviceIoControl", device_io_control);
        if (!create_file2 || !get_temp_path) {
            return INVALID_HANDLE_VALUE;
        }
        wchar_t temp[MAX_PATH]{};
        const DWORD length = get_temp_path(MAX_PATH, temp);
        if (length == 0 || length >= MAX_PATH) {
            LOG_CRITICAL(HW_Memory, "Failed to get the temp folder, error {}", GetLastError());
            return INVALID_HANDLE_VALUE;
        }
        const std::wstring path = std::wstring(temp) + L"eden_dram.bin";
        CREATEFILE2_EXTENDED_PARAMETERS params{};
        params.dwSize = sizeof(params);
        params.dwFileAttributes = FILE_ATTRIBUTE_TEMPORARY;
        params.dwFileFlags = FILE_FLAG_DELETE_ON_CLOSE;
        const HANDLE file = create_file2(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                                         CREATE_ALWAYS, &params);
        if (file == INVALID_HANDLE_VALUE) {
            LOG_CRITICAL(HW_Memory, "Failed to create the DRAM file in the temp folder, error {}",
                         GetLastError());
            return INVALID_HANDLE_VALUE;
        }
        DWORD returned{};
        const bool sparse = device_io_control &&
                            device_io_control(file, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0,
                                              &returned, nullptr);
        if (!sparse) {
            LOG_WARNING(HW_Memory, "The DRAM file could not be made sparse, error {}",
                        GetLastError());
        }
        g_backing_is_file.store(true, std::memory_order_release);
        g_backing_file_sparse.store(sparse, std::memory_order_release);
        return file;
    }

    bool IsSection() const {
        return backing_handle != nullptr;
    }

    /// VirtualProtect over a view of the reserved section fails on pages still only reserved, and
    /// ProtectRegion hands whole heap regions here. Committing them all to protect them (0.2.53)
    /// ran the console out of commit a minute in, with the demand-commit handler then failing. So
    /// only committed runs are protected. Reserved runs asked for read-write are skipped, since
    /// the handler commits them read-write on first touch; anything stricter (rasterizer-cached
    /// ranges) is committed so a later fastmem access still traps.
    void ProtectSectionView(u8* begin, size_t length, DWORD new_flags) {
        u8* address = begin;
        u8* const end = begin + length;
        while (address < end) {
            MEMORY_BASIC_INFORMATION info{};
            if (VirtualQuery(address, &info, sizeof(info)) == 0) {
                LOG_CRITICAL(HW_Memory, "Failed to query virtual memory, error {}", GetLastError());
                return;
            }
            u8* const region_end =
                (std::min)(static_cast<u8*>(info.BaseAddress) + info.RegionSize, end);
            const size_t region_length = static_cast<size_t>(region_end - address);
            bool apply = info.State == MEM_COMMIT;
            if (info.State == MEM_RESERVE && new_flags != PAGE_READWRITE) {
                apply = CommitSectionRange(reinterpret_cast<uintptr_t>(address),
                                           reinterpret_cast<uintptr_t>(region_end));
                if (!apply) {
                    LOG_CRITICAL(HW_Memory, "Failed to commit {} KiB to protect it, error {}",
                                 region_length >> 10, GetLastError());
                }
            }
            DWORD old_flags{};
            if (apply && !HOST_MEMORY_VIRTUAL_PROTECT(address, region_length, new_flags, &old_flags)) {
                LOG_CRITICAL(HW_Memory, "Failed to change virtual memory protect rules, error {}",
                             GetLastError());
            }
            address = region_end;
        }
    }
#endif

    void* Allocate(size_t size) {
        auto* ptr = VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (ptr == nullptr) {
            LOG_CRITICAL(HW_Memory, "Failed to allocate fallback buffer with size {:#x}, error {}", size, GetLastError());
        }
        return ptr;
    }

    void Map(size_t virtual_offset, size_t host_offset, size_t length, MemoryPermission perms) {
        std::unique_lock lock{placeholder_mutex};
        if (hybrid) {
            g_fastmem_request_bytes.fetch_add(length, std::memory_order_relaxed);
            g_fastmem_request_min.store(
                (std::min)(g_fastmem_request_min.load(std::memory_order_relaxed), u64{host_offset}),
                std::memory_order_relaxed);
            g_fastmem_request_max.store(
                (std::max)(g_fastmem_request_max.load(std::memory_order_relaxed),
                           u64{host_offset + length}),
                std::memory_order_relaxed);
            for (size_t offset = host_offset; offset < host_offset + length;) {
                const size_t bucket = offset >> FastmemHistogramShift;
                const size_t bucket_end = (std::min)(host_offset + length,
                                                     (bucket + 1) << FastmemHistogramShift);
                if (bucket < g_fastmem_request_histogram.size()) {
                    g_fastmem_request_histogram[bucket].fetch_add(bucket_end - offset,
                                                                  std::memory_order_relaxed);
                }
                offset = bucket_end;
            }
            const size_t host_end = host_offset + length;
            const size_t hot_end = hybrid_hot_offset + hybrid_hot_size;
            const size_t mapped_begin = (std::max)(host_offset, hybrid_hot_offset);
            const size_t mapped_end = (std::min)(host_end, hot_end);
            if (mapped_begin >= mapped_end) {
                return;
            }
            virtual_offset += mapped_begin - host_offset;
            host_offset = mapped_begin;
            length = mapped_end - mapped_begin;
        }
        if (!IsNiechePlaceholder(virtual_offset, length)) {
            Split(virtual_offset, length);
        }
        ASSERT(placeholders.find({virtual_offset, virtual_offset + length}) == placeholders.end());
        if (MapView(virtual_offset, host_offset, length)) {
            TrackPlaceholder(virtual_offset, host_offset, length);
        } else {
            CoalesceAroundHole(virtual_offset, length);
        }
    }

    void Unmap(size_t virtual_offset, size_t length) {
        std::scoped_lock lock{placeholder_mutex};

        // Unmap until there are no more placeholders
        while (UnmapOnePlaceholder(virtual_offset, length)) {
        }
    }

    void Protect(size_t virtual_offset, size_t length, bool read, bool write, bool execute) {
        DWORD new_flags{};
        if (read && write) {
            new_flags = PAGE_READWRITE;
        } else if (read && !write) {
            new_flags = PAGE_READONLY;
        } else if (!read && !write) {
            new_flags = PAGE_NOACCESS;
        } else {
            UNIMPLEMENTED_MSG("Protection flag combination read={} write={}", read, write);
        }
        const size_t virtual_end = virtual_offset + length;

        std::scoped_lock lock{placeholder_mutex};
        auto [it, end] = placeholders.equal_range({virtual_offset, virtual_end});
        while (it != end) {
            const size_t offset = (std::max)(it->lower(), virtual_offset);
            const size_t protect_length = (std::min)(it->upper(), virtual_end) - offset;
            DWORD old_flags{};
#ifdef HOST_MEMORY_USE_FROM_APP
            if (IsSection()) {
                ProtectSectionView(virtual_base + offset, protect_length, new_flags);
                ++it;
                continue;
            }
#endif
            const bool protected_ok = HOST_MEMORY_VIRTUAL_PROTECT(virtual_base + offset,
                                                                  protect_length, new_flags,
                                                                  &old_flags);
            if (!protected_ok) {
                LOG_CRITICAL(HW_Memory, "Failed to change virtual memory protect rules");
            }
            ++it;
        }
    }

    void EnableDirectMappedAddress() {
        // TODO
        UNREACHABLE();
    }

    const size_t backing_size; ///< Size of the backing memory in bytes
    const size_t virtual_size; ///< Size of the virtual address placeholder in bytes
    const HostMemoryFastmemRegion fastmem_region;

    u8* backing_base{};
    u8* virtual_base{};

private:
    /// Release all resources in the object
    void Release() {
#ifdef HOST_MEMORY_USE_FROM_APP
        // UWP: private demand-committed backing, no section handle and no fastmem arena. Tear down the
        // demand-commit handler first, then release the private reservation (committed pages included).
        if (g_backing_veh != nullptr && g_pfn_remove_veh != nullptr) {
            g_pfn_remove_veh(g_backing_veh);
            g_backing_veh = nullptr;
        }
        g_backing_is_section.store(false, std::memory_order_release);
        g_arena_base.store(nullptr, std::memory_order_release);
        g_arena_size.store(0, std::memory_order_release);
        g_backing_base.store(nullptr, std::memory_order_release);
        g_backing_size.store(0, std::memory_order_release);
        if (backing_handle || backing_file != INVALID_HANDLE_VALUE) {
            ReleaseSection();
            return;
        }
        if (backing_base) {
            if (!VirtualFree(backing_base, 0, MEM_RELEASE)) {
                LOG_CRITICAL(HW_Memory, "Failed to free backing memory");
            }
            backing_base = nullptr;
        }
    }

    void ReleaseSection() {
        if (!placeholders.empty()) {
            for (const auto& placeholder : placeholders) {
                pfn_UnmapViewOfFile2(process, virtual_base + placeholder.lower(),
                                     MEM_PRESERVE_PLACEHOLDER);
            }
            Coalesce(0, virtual_size);
            placeholders.clear();
            placeholder_host_pointers.clear();
        }
        if (virtual_base) {
            VirtualFree(virtual_base, 0, MEM_RELEASE);
            virtual_base = nullptr;
        }
        if (hybrid) {
            if (hybrid_canonical_mapped) {
                pfn_UnmapViewOfFile2(process, backing_base + hybrid_hot_offset,
                                     MEM_PRESERVE_PLACEHOLDER);
                hybrid_canonical_mapped = false;
            }
            const size_t suffix_offset = hybrid_hot_offset + hybrid_hot_size;
            if (hybrid_prefix_private) {
                VirtualFree(backing_base, 0, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
            }
            if (hybrid_suffix_private) {
                VirtualFree(backing_base + suffix_offset, 0,
                            MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
            }
            if (hybrid_split_prefix || hybrid_split_suffix) {
                VirtualFree(backing_base, backing_size,
                            MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS);
            }
            VirtualFree(backing_base, 0, MEM_RELEASE);
            backing_base = nullptr;
            hybrid = false;
            hybrid_alias_bytes = 0;
        } else {
            UnmapBackingViews();
        }
        if (backing_handle) {
            CloseHandle(backing_handle);
        }
        backing_handle = nullptr;
        if (backing_file != INVALID_HANDLE_VALUE) {
            CloseHandle(backing_file); // deletes it (FILE_FLAG_DELETE_ON_CLOSE)
            backing_file = INVALID_HANDLE_VALUE;
        }
        g_backing_is_file.store(false, std::memory_order_release);
        g_fastmem_canonical_bytes.store(0, std::memory_order_release);
        g_fastmem_alias_bytes.store(0, std::memory_order_release);
        g_fastmem_alias_budget.store(0, std::memory_order_release);
        g_fastmem_hot_offset.store(0, std::memory_order_release);
        g_fastmem_hot_size_actual.store(0, std::memory_order_release);
#else
        if (!placeholders.empty()) {
            for (const auto& placeholder : placeholders) {
                if (!pfn_UnmapViewOfFile2(process, virtual_base + placeholder.lower(),
                                          MEM_PRESERVE_PLACEHOLDER)) {
                    LOG_CRITICAL(HW_Memory, "Failed to unmap virtual memory placeholder");
                }
            }
            Coalesce(0, virtual_size);
        }
        if (virtual_base) {
            if (!VirtualFree(virtual_base, 0, MEM_RELEASE)) {
                LOG_CRITICAL(HW_Memory, "Failed to free virtual memory");
            }
        }
        if (backing_base) {
            if (!pfn_UnmapViewOfFile2(process, backing_base, MEM_PRESERVE_PLACEHOLDER)) {
                LOG_CRITICAL(HW_Memory, "Failed to unmap backing memory placeholder");
            }
            if (!VirtualFreeEx(process, backing_base, 0, MEM_RELEASE)) {
                LOG_CRITICAL(HW_Memory, "Failed to free backing memory");
            }
        }
        if (!CloseHandle(backing_handle)) {
            LOG_CRITICAL(HW_Memory, "Failed to free backing memory file handle");
        }
#endif
    }

    /// Unmap one placeholder in the given range (partial unmaps are supported)
    /// Return true when there are no more placeholders to unmap
    bool UnmapOnePlaceholder(size_t virtual_offset, size_t length) {
        const auto it = placeholders.find({virtual_offset, virtual_offset + length});
        const auto begin = placeholders.begin();
        const auto end = placeholders.end();
        if (it == end) {
            return false;
        }
        const size_t placeholder_begin = it->lower();
        const size_t placeholder_end = it->upper();
        const size_t unmap_begin = (std::max)(virtual_offset, placeholder_begin);
        const size_t unmap_end = (std::min)(virtual_offset + length, placeholder_end);
        ASSERT(unmap_begin >= placeholder_begin && unmap_begin < placeholder_end);
        ASSERT(unmap_end <= placeholder_end && unmap_end > placeholder_begin);

        const auto host_pointer_it = placeholder_host_pointers.find(placeholder_begin);
        ASSERT(host_pointer_it != placeholder_host_pointers.end());
        const size_t host_offset = host_pointer_it->second;

        const bool split_left = unmap_begin > placeholder_begin;
        const bool split_right = unmap_end < placeholder_end;

        if (!pfn_UnmapViewOfFile2(process, virtual_base + placeholder_begin,
                                  MEM_PRESERVE_PLACEHOLDER)) {
            LOG_CRITICAL(HW_Memory, "Failed to unmap placeholder");
        }
        if (hybrid) {
            hybrid_alias_bytes -= placeholder_end - placeholder_begin;
            g_fastmem_alias_bytes.store(hybrid_alias_bytes, std::memory_order_relaxed);
        }
        // If we have to remap memory regions due to partial unmaps, we are in a data race as
        // Windows doesn't support remapping memory without unmapping first. Avoid adding any extra
        // logic within the panic region described below.

        // Panic region, we are in a data race right now
        if (split_left || split_right) {
            Split(unmap_begin, unmap_end - unmap_begin);
        }
        const bool mapped_left =
            split_left && MapView(placeholder_begin, host_offset, unmap_begin - placeholder_begin);
        const bool mapped_right =
            split_right && MapView(unmap_end, host_offset + unmap_end - placeholder_begin,
                                   placeholder_end - unmap_end);
        // End panic region

        size_t coalesce_begin = unmap_begin;
        if (!split_left) {
            // Try to coalesce pages to the left
            coalesce_begin = it == begin ? 0 : std::prev(it)->upper();
            if (coalesce_begin != placeholder_begin) {
                Coalesce(coalesce_begin, unmap_end - coalesce_begin);
            }
        }
        if (!split_right) {
            // Try to coalesce pages to the right
            const auto next = std::next(it);
            const size_t next_begin = next == end ? virtual_size : next->lower();
            if (placeholder_end != next_begin) {
                // We can coalesce to the right
                Coalesce(coalesce_begin, next_begin - coalesce_begin);
            }
        }
        // Remove and reinsert placeholder trackers
        UntrackPlaceholder(it);
        if (mapped_left) {
            TrackPlaceholder(placeholder_begin, host_offset, unmap_begin - placeholder_begin);
        }
        if (mapped_right) {
            TrackPlaceholder(unmap_end, host_offset + unmap_end - placeholder_begin,
                             placeholder_end - unmap_end);
        }
        return true;
    }

    bool MapView(size_t virtual_offset, size_t host_offset, size_t length) {
        if (hybrid && length > hybrid_alias_budget - hybrid_alias_bytes) {
            g_fastmem_skipped_bytes.fetch_add(length, std::memory_order_relaxed);
            LOG_WARNING(HW_Memory, "Hybrid fastmem alias budget exhausted; {} KiB stays on page table",
                        length >> 10);
            return false;
        }
        const size_t section_offset = hybrid ? host_offset - hybrid_hot_offset : host_offset;
        if (!pfn_MapViewOfFile3(backing_handle, process, virtual_base + virtual_offset,
                                section_offset, length, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE,
                                nullptr, 0)) {
            g_fastmem_map_failures.fetch_add(1, std::memory_order_relaxed);
            g_fastmem_skipped_bytes.fetch_add(length, std::memory_order_relaxed);
            LOG_WARNING(HW_Memory,
                        "Failed to map fastmem alias at guest {:#x}, {} KiB, error {}; using page table",
                        virtual_offset, length >> 10, GetLastError());
            return false;
        }
        if (hybrid) {
            hybrid_alias_bytes += length;
            g_fastmem_alias_bytes.store(hybrid_alias_bytes, std::memory_order_relaxed);
            g_fastmem_guest_bytes.fetch_add(length, std::memory_order_relaxed);
        }
        return true;
    }

    void CoalesceAroundHole(size_t virtual_offset, size_t length) {
        // The failed mapping left exactly this range as a placeholder. Neighboring mapped views
        // delimit the largest placeholder range that can safely be coalesced.
        const auto right = placeholders.upper_bound({virtual_offset, virtual_offset + length});
        const size_t begin = right == placeholders.begin() ? 0 : std::prev(right)->upper();
        const size_t end = right == placeholders.end() ? virtual_size : right->lower();
        if (begin < virtual_offset || virtual_offset + length < end) {
            Coalesce(begin, end - begin);
        }
    }

    void Split(size_t virtual_offset, size_t length) {
        if (!VirtualFreeEx(process, reinterpret_cast<LPVOID>(virtual_base + virtual_offset), length,
                           MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) {
            LOG_CRITICAL(HW_Memory, "Failed to split placeholder");
        }
    }

    void Coalesce(size_t virtual_offset, size_t length) {
        if (!VirtualFreeEx(process, reinterpret_cast<LPVOID>(virtual_base + virtual_offset), length,
                           MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS)) {
            LOG_CRITICAL(HW_Memory, "Failed to coalesce placeholders");
        }
    }

    void TrackPlaceholder(size_t virtual_offset, size_t host_offset, size_t length) {
        placeholders.insert({virtual_offset, virtual_offset + length});
        placeholder_host_pointers.emplace(virtual_offset, host_offset);
    }

    void UntrackPlaceholder(boost::icl::separate_interval_set<size_t>::iterator it) {
        placeholder_host_pointers.erase(it->lower());
        placeholders.erase(it);
    }

    /// Return true when a given memory region is a "nieche" and the placeholders don't have to be
    /// split.
    bool IsNiechePlaceholder(size_t virtual_offset, size_t length) const {
        const auto it = placeholders.upper_bound({virtual_offset, virtual_offset + length});
        if (it != placeholders.end() && it->lower() == virtual_offset + length) {
            return it == placeholders.begin() ? virtual_offset == 0
                                              : std::prev(it)->upper() == virtual_offset;
        }
        return false;
    }

    HANDLE process{};        ///< Current process handle
    HANDLE backing_handle{}; ///< File based backing memory
    static constexpr size_t MaxHybridAliasBudget = size_t{512} << 20;
    static constexpr size_t HybridTotalViewBudget = size_t{896} << 20;
    bool hybrid{};
    bool hybrid_canonical_mapped{};
    bool hybrid_split_prefix{};
    bool hybrid_split_suffix{};
    bool hybrid_prefix_private{};
    bool hybrid_suffix_private{};
    size_t hybrid_hot_offset{};
    size_t hybrid_hot_size{};
    size_t hybrid_alias_bytes{};
    size_t hybrid_alias_budget{};
    bool backing_mapped{};   ///< UWP fastmem: the section view replaced the backing placeholder
    HANDLE backing_file{INVALID_HANDLE_VALUE}; ///< UWP fastmem: the file behind the section
    size_t backing_piece{};  ///< UWP fastmem: size of each backing view, 0 for a single view
    size_t backing_pieces{}; ///< UWP fastmem: backing views mapped in pieces

    DynamicLibrary kernelbase_dll;
    PFN_CreateFileMapping2 pfn_CreateFileMapping2{};
    PFN_CreateFileMappingFromApp pfn_CreateFileMappingFromApp{};
    PFN_VirtualAlloc2 pfn_VirtualAlloc2{};
    PFN_MapViewOfFile3 pfn_MapViewOfFile3{};
    PFN_UnmapViewOfFile2 pfn_UnmapViewOfFile2{};
    PFN_VirtualProtect pfn_VirtualProtectFromApp{};

    std::mutex placeholder_mutex;                                 ///< Mutex for placeholders
    boost::icl::separate_interval_set<size_t> placeholders;       ///< Mapped placeholders
    ::Common::unordered_map<size_t, size_t> placeholder_host_pointers; ///< Placeholder backing offset
};

#elif defined(__OPENORBIS__) || defined(__managarm__)
// None of the luxuries of POSIX, all of the suffering
// For managarm: see https://github.com/managarm/managarm/issues/1370
#else // ^^^ Windows ^^^ vvv POSIX vvv

#ifndef MAP_NOCORE
#define MAP_NOCORE 0
#endif

#ifdef ARCHITECTURE_arm64

static void* ChooseVirtualBase(size_t virtual_size) {
    constexpr uintptr_t Map39BitSize = (1ULL << 39);
    constexpr uintptr_t Map36BitSize = (1ULL << 36);

    // This is not a cryptographic application, we just want something random.
    std::mt19937_64 rng;

    // We want to ensure we are allocating at an address aligned to the L2 block size.
    // For Qualcomm devices, we must also allocate memory above 36 bits.
    const size_t lower = Map36BitSize / HugePageSize;
    const size_t upper = (Map39BitSize - virtual_size) / HugePageSize;
    const size_t range = upper - lower;

    // Try up to 64 times to allocate memory at random addresses in the range.
    for (int i = 0; i < 64; i++) {
        // Calculate a possible location.
        uintptr_t hint_address = ((rng() % range) + lower) * HugePageSize;

        // Try to map.
        // Note: we may be able to take advantage of MAP_FIXED_NOREPLACE here.
        void* map_pointer =
            mmap(reinterpret_cast<void*>(hint_address), virtual_size, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_NOCORE, -1, 0);

        // If we successfully mapped, we're done.
        if (reinterpret_cast<uintptr_t>(map_pointer) == hint_address) {
            return map_pointer;
        }

        // Unmap if necessary, and try again.
        if (map_pointer != MAP_FAILED) {
            munmap(map_pointer, virtual_size);
        }
    }

    return MAP_FAILED;
}

#else

static void* ChooseVirtualBase(size_t virtual_size) {
#if defined(__FreeBSD__) || defined(__DragonFly__) || defined(__OpenBSD__) || defined(__sun__) || defined(__HAIKU__) || defined(__managarm__) || defined(__AIX__)
    void* virtual_base = mmap(nullptr, virtual_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_ALIGNED_SUPER | MAP_NOCORE, -1, 0);
    if (virtual_base != MAP_FAILED)
        return virtual_base;
#endif
    return mmap(nullptr, virtual_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_NOCORE, -1, 0);
}

#endif

#if defined(__sun__) || defined(__HAIKU__) || defined(__NetBSD__) || defined(__DragonFly__)
/// Most Unices don't have a portable shm_open (AIX, OpenBSD, NetBSD, Solaris 11, OpenIndiana)
/// Portable implementation of shm_open(SHM_ANON, ...) - roughly equivalent but without
/// OS support - may fail sporadically, beware!
static int shm_open_anon(int flags, mode_t mode) {
    char name[16] = "/shm-";
    char *const limit = name + sizeof(name) - 1;
    *limit = '\0';
    char *start = name + strlen(name);
    for (int tries = 0; tries < 4; tries++) {
        struct timespec tv;
        clock_gettime(CLOCK_REALTIME, &tv);
        unsigned long r = (unsigned long)tv.tv_sec + (unsigned long)tv.tv_nsec;
        for (char *fill = start; fill < limit; r /= 8)
            *fill++ = '0' + (r % 8);
        int fd = shm_open(name, flags, mode);
        if (fd != -1) {
            if (shm_unlink(name) == -1) {
                int tmp = errno;
                close(fd);
                errno = tmp;
                return -1;
            }
            return fd;
        }
        if (errno != EEXIST)
            break;
    }
    return -1;
}
#elif defined(__OpenBSD__)
/// Except OpenBSD which explicitly uses shm_mkstemp instead (as a more secure alternative)
static int shm_open_anon(int flags, mode_t mode) {
    char name[16] = "/shm-XXXXXXXXXX";
    int fd;
    if ((fd = shm_mkstemp(name)) == -1)
        return -1;
    if (shm_unlink(name) == -1) {
        int tmp = errno;
        close(fd);
        errno = tmp;
        return -1;
    }
    return fd;
}
#endif

class HostMemory::Impl {
public:
    explicit Impl(size_t backing_size_, size_t virtual_size_)
        : backing_size{backing_size_}
        , virtual_size{virtual_size_}
    {}

    bool Init() {
        long page_size = sysconf(_SC_PAGESIZE);
        ASSERT_MSG(page_size == 0x1000, "page size {:#x} is incompatible with 4K paging", page_size);
        // Backing memory initialization
#if defined(__sun__) || defined(__HAIKU__) || defined(__NetBSD__) || defined(__DragonFly__)
        fd = shm_open_anon(O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
#elif defined(__OpenBSD__)
        fd = shm_open_anon(O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
#elif defined(__FreeBSD__)
        fd = shm_open(SHM_ANON, O_RDWR, 0600);
#elif defined(__APPLE__) || defined(__managarm__)
        // macOS doesn't have memfd_create, use anonymous temporary file
        char template_path[] = "/tmp/eden_mem_XXXXXX";
        fd = mkstemp(template_path);
        if (fd >= 0) {
            unlink(template_path);
        }
#else
        fd = memfd_create("HostMemory", 0);
#endif
        bool use_anon = false;
        if (fd <= 0) {
            LOG_WARNING(Common_Memory, "memfd_create: {}", strerror(errno));
            use_anon = true;
        }
        if (!use_anon) {
            // Defined to extend the file with zeros
            int ret = ftruncate(fd, backing_size);
            if (ret != 0) {
                LOG_WARNING(Common_Memory, "ftruncate: {} (likely out-of-emory)", strerror(errno));
                use_anon = true;
            }
        }
        if (use_anon) {
            LOG_WARNING(Common_Memory, "Using private mappings instead of shared ones");
            backing_base = static_cast<u8*>(mmap(nullptr, backing_size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE | MAP_NOCORE, -1, 0));
            if (fd > 0) {
                fd = -1;
                close(fd);
            }
        } else {
            backing_base = static_cast<u8*>(mmap(nullptr, backing_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_NOCORE, fd, 0));
        }
        if (backing_base == MAP_FAILED) {
            LOG_CRITICAL(HW_Memory, "mmap failed: {}", strerror(errno));
            return false;
        }

        // Virtual memory initialization
        virtual_base = virtual_map_base = static_cast<u8*>(ChooseVirtualBase(virtual_size));
        if (virtual_base == MAP_FAILED) {
            LOG_CRITICAL(HW_Memory, "mmap failed: {}", strerror(errno));
            return false;
        }
#if defined(__linux__)
        madvise(virtual_base, virtual_size, MADV_HUGEPAGE);
#endif
        free_manager.SetAddressSpace(virtual_base, virtual_size);
        return true;
    }

    ~Impl() {
        Release();
    }

    void* Allocate(size_t size) {
        auto* ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
        if (ptr == MAP_FAILED) {
            LOG_CRITICAL(HW_Memory, "Failed to allocate fallback buffer with size {:#x}, {}", size, strerror(errno));
        }
        return ptr;
    }

    void Map(size_t virtual_offset, size_t host_offset, size_t length, MemoryPermission perms) {
        // Intersect the range with our address space.
        AdjustMap(&virtual_offset, &length);

        // We are removing a placeholder.
        free_manager.AllocateBlock(virtual_base + virtual_offset, length);

        // Deduce mapping protection flags.
        int prot_flags = PROT_NONE;
        if (True(perms & MemoryPermission::Read))
            prot_flags |= PROT_READ;
        if (True(perms & MemoryPermission::Write))
            prot_flags |= PROT_WRITE;
#ifdef ARCHITECTURE_arm64
        if (True(perms & MemoryPermission::Execute))
            prot_flags |= PROT_EXEC;
#endif
        int flags = (fd >= 0 ? MAP_SHARED : MAP_PRIVATE) | MAP_FIXED;
        void* ret = mmap(virtual_base + virtual_offset, length, prot_flags, flags, fd, host_offset);
        ASSERT_MSG(ret != MAP_FAILED, "mmap: {} {}", strerror(errno), fd);
    }

    void Unmap(size_t virtual_offset, size_t length) {
        // The method name is wrong. We're still talking about the virtual range.
        // We don't want to unmap, we want to reserve this memory.

        // Intersect the range with our address space.
        AdjustMap(&virtual_offset, &length);

        // Merge with any adjacent placeholder mappings.
        auto [merged_pointer, merged_size] =
            free_manager.FreeBlock(virtual_base + virtual_offset, length);

        void* ret = mmap(merged_pointer, merged_size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        ASSERT_MSG(ret != MAP_FAILED, "mmap: {}", strerror(errno));
    }

    void Protect(size_t virtual_offset, size_t length, bool read, bool write, bool execute) {
        // Intersect the range with our address space.
        AdjustMap(&virtual_offset, &length);

        int flags = PROT_NONE;
        if (read) {
            flags |= PROT_READ;
        }
        if (write) {
            flags |= PROT_WRITE;
        }
#ifdef HAS_NCE
        if (execute) {
            flags |= PROT_EXEC;
        }
#endif
        int ret = mprotect(virtual_base + virtual_offset, length, flags);
        ASSERT_MSG(ret == 0, "mprotect failed: {}", strerror(errno));
    }

    void EnableDirectMappedAddress() {
        virtual_base = nullptr;
    }

    const size_t backing_size; ///< Size of the backing memory in bytes
    const size_t virtual_size; ///< Size of the virtual address placeholder in bytes

    u8* backing_base{reinterpret_cast<u8*>(MAP_FAILED)};
    u8* virtual_base{reinterpret_cast<u8*>(MAP_FAILED)};
    u8* virtual_map_base{reinterpret_cast<u8*>(MAP_FAILED)};

private:
    /// Release all resources in the object
    void Release() {
        if (virtual_map_base != MAP_FAILED) {
            int ret = munmap(virtual_map_base, virtual_size);
            ASSERT_MSG(ret == 0, "munmap failed: {}", strerror(errno));
        }

        if (backing_base != MAP_FAILED) {
            int ret = munmap(backing_base, backing_size);
            ASSERT_MSG(ret == 0, "munmap failed: {}", strerror(errno));
        }

        if (fd != -1) {
            int ret = close(fd);
            ASSERT_MSG(ret == 0, "close failed: {}", strerror(errno));
        }
    }

    void AdjustMap(size_t* virtual_offset, size_t* length) {
        if (virtual_base != nullptr) {
            return;
        }

        // If we are direct mapped, we want to make sure we are operating on a region
        // that is in range of our virtual mapping.
        size_t intended_start = *virtual_offset;
        size_t intended_end = intended_start + *length;
        size_t address_space_start = reinterpret_cast<size_t>(virtual_map_base);
        size_t address_space_end = address_space_start + virtual_size;

        if (address_space_start > intended_end || intended_start > address_space_end) {
            *virtual_offset = 0;
            *length = 0;
        } else {
            *virtual_offset = (std::max)(intended_start, address_space_start);
            *length = (std::min)(intended_end, address_space_end) - *virtual_offset;
        }
    }

    int fd{-1}; // memfd file descriptor, -1 is the error value of memfd_create
    FreeRegionManager free_manager{};
};

#endif // ^^^ POSIX ^^^

HostMemory::HostMemory(size_t backing_size_, size_t virtual_size_,
                       HostMemoryFastmemRegion fastmem_region_)
    : backing_size(backing_size_)
    , virtual_size(virtual_size_)
{
#if defined(__OPENORBIS__) || defined(__managarm__)
    LOG_WARNING(HW_Memory, "Platform doesn't support fastmem");
    backing_base = static_cast<u8*>(mmap(nullptr, backing_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    virtual_base = nullptr;
#else
    // Try to allocate a fastmem arena.
#ifdef _WIN32
    impl = std::make_unique<HostMemory::Impl>(AlignUp(backing_size, PageAlignment),
                                              AlignUp(virtual_size, PageAlignment) + HugePageSize,
                                              fastmem_region_);
#else
    (void)fastmem_region_;
    impl = std::make_unique<HostMemory::Impl>(AlignUp(backing_size, PageAlignment),
                                              AlignUp(virtual_size, PageAlignment) + HugePageSize);
#endif
    if (impl->Init()) {
        backing_base = impl->backing_base;
        virtual_base = impl->virtual_base;
        if (virtual_base) {
            // Ensure the virtual base is aligned to the L2 block size.
            virtual_base = reinterpret_cast<u8*>(Common::AlignUp(uintptr_t(virtual_base), HugePageSize));
            virtual_base_offset = virtual_base - impl->virtual_base;
        }
    } else {
        LOG_WARNING(HW_Memory, "Platform can support fastmem, but can't create it");
        fallback_buffer = true;
        backing_base = static_cast<u8*>(impl->Allocate(backing_size));
        virtual_base = nullptr;
        impl.reset();
    }
#endif
}

HostMemory::~HostMemory() {
#ifdef _WIN32
    if (fallback_buffer) {
        VirtualFree(backing_base, backing_size, MEM_RELEASE);
    }
#else
    if (fallback_buffer) {
        munmap(backing_base, backing_size);
    }
#endif
}

HostMemory::HostMemory(HostMemory&&) noexcept = default;

HostMemory& HostMemory::operator=(HostMemory&&) noexcept = default;

void HostMemory::Map(size_t virtual_offset, size_t host_offset, size_t length, MemoryPermission perms, bool separate_heap) {
#if !(defined(__OPENORBIS__) || defined(__managarm__))
    ASSERT(virtual_offset % PageAlignment == 0);
    ASSERT(host_offset % PageAlignment == 0);
    ASSERT(length % PageAlignment == 0);
    ASSERT(virtual_offset + length <= virtual_size);
    ASSERT(host_offset + length <= backing_size);
    if (length == 0 || !virtual_base || !impl) {
        return;
    }
    impl->Map(virtual_offset + virtual_base_offset, host_offset, length, perms);
#endif
}

void HostMemory::Unmap(size_t virtual_offset, size_t length, bool separate_heap) {
#if !(defined(__OPENORBIS__) || defined(__managarm__))
    ASSERT(virtual_offset % PageAlignment == 0);
    ASSERT(length % PageAlignment == 0);
    ASSERT(virtual_offset + length <= virtual_size);
    if (length == 0 || !virtual_base || !impl) {
        return;
    }
    impl->Unmap(virtual_offset + virtual_base_offset, length);
#endif
}

void HostMemory::Protect(size_t virtual_offset, size_t length, MemoryPermission perm) {
#if !(defined(__OPENORBIS__) || defined(__managarm__))
    ASSERT(virtual_offset % PageAlignment == 0);
    ASSERT(length % PageAlignment == 0);
    ASSERT(virtual_offset + length <= virtual_size);
    if (length == 0 || !virtual_base || !impl) {
        return;
    }
    const bool read = True(perm & MemoryPermission::Read);
    const bool write = True(perm & MemoryPermission::Write);
    const bool execute = True(perm & MemoryPermission::Execute);
    impl->Protect(virtual_offset + virtual_base_offset, length, read, write, execute);
#endif
}

void HostMemory::ClearBackingRegion(size_t physical_offset, size_t length, u32 fill_value) {
#ifdef HOST_MEMORY_USE_FROM_APP
    // Xbox/UWP: the backing is demand-committed, so a memset of zero COMMITS every page it touches.
    // The kernel clears every newly allocated heap page, and libnx's default heap is all available
    // memory (~3.x GiB): that memset alone busts the Game-mode title budget, the handler's commit
    // fails, and the memset access-violates (observed on-console at +3.6 GiB into the backing).
    //
    // Zeroing whole pages is a decommit instead: the next touch faults into the demand-commit
    // handler, which hands back a freshly committed page, and committed pages always read as zero.
    // Same contents for the guest, but it returns budget rather than spending it. The partial pages
    // at either end are memset as before. VirtualFree(MEM_DECOMMIT) accepts pages that were never
    // committed.
    // With fastmem the backing is a section view, which cannot be decommitted. Only the pages
    // already committed are cleared; the reserved ones read as zero whenever they get committed,
    // so the heap clear does not commit memory the guest never touched.
    // Over a file every page reads as committed. A page never written is a hole of the file and
    // reads as zero, and memset would dirty it and give it a disk block, so pages already zero are
    // only read.
    if (fill_value == 0 && g_backing_is_file.load(std::memory_order_acquire)) {
        constexpr uintptr_t HostPageSize = 4096;
        const auto begin = reinterpret_cast<uintptr_t>(backing_base + physical_offset);
        const auto end = begin + length;
        const uintptr_t pages_begin = (std::min)(Common::AlignUp(begin, HostPageSize), end);
        const uintptr_t pages_end = (std::max)(Common::AlignDown(end, HostPageSize), pages_begin);
        std::memset(reinterpret_cast<void*>(begin), 0, pages_begin - begin);
        for (uintptr_t page = pages_begin; page < pages_end; page += HostPageSize) {
            const auto* const words = reinterpret_cast<const u64*>(page);
            for (size_t i = 0; i < HostPageSize / sizeof(u64); ++i) {
                if (words[i] != 0) {
                    std::memset(reinterpret_cast<void*>(page), 0, HostPageSize);
                    break;
                }
            }
        }
        std::memset(reinterpret_cast<void*>(pages_end), 0, end - pages_end);
        return;
    }
    if (fill_value == 0 && impl && impl->IsSection()) {
        u8* address = backing_base + physical_offset;
        u8* const end = address + length;
        while (address < end) {
            MEMORY_BASIC_INFORMATION info{};
            if (VirtualQuery(address, &info, sizeof(info)) == 0) {
                std::memset(address, 0, end - address);
                return;
            }
            u8* const region_end = (std::min)(
                static_cast<u8*>(info.BaseAddress) + info.RegionSize, end);
            if (info.State == MEM_COMMIT) {
                std::memset(address, 0, region_end - address);
            }
            address = region_end;
        }
        return;
    }
    if (fill_value == 0) {
        constexpr uintptr_t HostPageSize = 4096;
        const auto begin = reinterpret_cast<uintptr_t>(backing_base + physical_offset);
        const auto end = begin + length;
        const uintptr_t pages_begin = Common::AlignUp(begin, HostPageSize);
        const uintptr_t pages_end = Common::AlignDown(end, HostPageSize);
        if (pages_begin < pages_end &&
            VirtualFree(reinterpret_cast<void*>(pages_begin), pages_end - pages_begin,
                        MEM_DECOMMIT)) {
            std::memset(reinterpret_cast<void*>(begin), 0, pages_begin - begin);
            std::memset(reinterpret_cast<void*>(pages_end), 0, end - pages_end);
            return;
        }
    }
#endif
    std::memset(backing_base + physical_offset, fill_value, length);
}

void HostMemory::EnableDirectMappedAddress() {
#if !(defined(__OPENORBIS__) || defined(__managarm__))
    if (impl) {
        impl->EnableDirectMappedAddress();
        virtual_size += reinterpret_cast<uintptr_t>(virtual_base);
    }
#endif
}

std::optional<u64> EmulatedDramCommittedBytes() {
#ifdef HOST_MEMORY_USE_FROM_APP
    if (g_backing_is_file.load(std::memory_order_acquire)) {
        return std::nullopt;
    }
    if (g_backing_is_section.load(std::memory_order_acquire)) {
        return g_section_commit_bytes.load(std::memory_order_relaxed);
    }
    // Private backing: what is committed of it, page ranges at a time.
    u8* const base = g_backing_base.load(std::memory_order_acquire);
    const size_t size = g_backing_size.load(std::memory_order_acquire);
    if (base == nullptr) {
        return std::nullopt;
    }
    u64 committed = 0;
    for (u8* address = base; address < base + size;) {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(address, &info, sizeof(info)) == 0) {
            break;
        }
        u8* const region_end =
            (std::min)(static_cast<u8*>(info.BaseAddress) + info.RegionSize, base + size);
        if (info.State == MEM_COMMIT) {
            committed += static_cast<u64>(region_end - address);
        }
        address = region_end;
    }
    return committed;
#else
    return std::nullopt;
#endif
}

std::string HostMemoryCommitStats() {
#ifdef HOST_MEMORY_USE_FROM_APP
    if (!g_backing_is_section.load(std::memory_order_acquire)) {
        // Private backing: sum what is committed, to split the app's memory into emulated DRAM
        // and the rest (JIT, GPU, caches).
        if (g_backing_base.load(std::memory_order_acquire) == nullptr) {
            return {};
        }
        return "emulated DRAM " + std::to_string(*EmulatedDramCommittedBytes() >> 20) + " MiB";
    }
    if (g_backing_is_file.load(std::memory_order_acquire)) {
        if (const u64 hot_size = g_fastmem_hot_size_actual.load(std::memory_order_acquire);
            hot_size != 0) {
            const u64 aliases = g_fastmem_alias_bytes.load(std::memory_order_relaxed);
            const u64 alias_budget = g_fastmem_alias_budget.load(std::memory_order_relaxed);
            const u64 skipped = g_fastmem_skipped_bytes.load(std::memory_order_relaxed);
            const u64 failures = g_fastmem_map_failures.load(std::memory_order_relaxed);
            const u64 request_min = g_fastmem_request_min.load(std::memory_order_relaxed);
            const u64 request_max = g_fastmem_request_max.load(std::memory_order_relaxed);
            char hot_offset[32];
            std::snprintf(hot_offset, sizeof(hot_offset), "%llx",
                          static_cast<unsigned long long>(
                              g_fastmem_hot_offset.load(std::memory_order_relaxed)));
            std::string histogram;
            for (size_t i = 0; i < g_fastmem_request_histogram.size(); ++i) {
                const u64 bytes = g_fastmem_request_histogram[i].load(std::memory_order_relaxed);
                if (bytes != 0) {
                    histogram += (histogram.empty() ? "" : ",") + std::to_string(i * 64) + ":" +
                                 std::to_string(bytes >> 20);
                }
            }
            return "hybrid fastmem hot [0x" + std::string{hot_offset} +
                   ", " + std::to_string(hot_size >> 20) + " MiB], aliases " +
                   std::to_string(aliases >> 20) + "/" + std::to_string(alias_budget >> 20) +
                   " MiB, guest mapped " +
                   std::to_string(g_fastmem_guest_bytes.load(std::memory_order_relaxed) >> 20) +
                   " MiB, skipped " + std::to_string(skipped >> 20) + " MiB, failures " +
                   std::to_string(failures) + ", requested " +
                   std::to_string(g_fastmem_request_bytes.load(std::memory_order_relaxed) >> 20) +
                   " MiB in [" + (request_min == ~u64{0} ? "none" : std::to_string(request_min >> 20)) +
                   ", " + std::to_string(request_max >> 20) + ") MiB; 64-MiB buckets " + histogram;
        }
        return g_backing_file_sparse.load(std::memory_order_acquire)
                   ? "DRAM in a sparse file-backed section"
                   : "DRAM in a file-backed section (not sparse)";
    }
    std::string stats = "section commit " +
                        std::to_string(g_section_commit_bytes.load(std::memory_order_relaxed) >> 20) +
                        " MiB";
    if (const u64 failures = g_commit_failures.load(std::memory_order_relaxed); failures != 0) {
        char failure[160];
        std::snprintf(failure, sizeof(failure),
                      ", %llu commit failures (last: error %u, %llu KiB at 0x%llx), %llu saved by "
                      "a single-page retry",
                      static_cast<unsigned long long>(failures),
                      g_last_commit_error.load(std::memory_order_relaxed),
                      static_cast<unsigned long long>(
                          g_last_failed_length.load(std::memory_order_relaxed) >> 10),
                      static_cast<unsigned long long>(
                          g_last_failed_address.load(std::memory_order_relaxed)),
                      static_cast<unsigned long long>(
                          g_page_retry_saves.load(std::memory_order_relaxed)));
        stats += failure;
    }
    return stats;
#else
    return {};
#endif
}

} // namespace Common
