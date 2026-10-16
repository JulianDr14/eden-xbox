// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <optional>
#include <string>
#include "common/common_funcs.h"
#include "common/common_types.h"

namespace Common {

struct HostMemoryFastmemRegion {
    size_t offset{};
    size_t size{};
    bool force_full{};
};

/// Configured by the UWP frontend before Core::System creates DeviceMemory.
void ConfigureHostMemoryFastmem(size_t hot_mib, bool force_full);
[[nodiscard]] size_t HostMemoryFastmemHotSize();
[[nodiscard]] bool HostMemoryFastmemForceFull();

enum class MemoryPermission : u32 {
    Read = 1 << 0,
    Write = 1 << 1,
    ReadWrite = Read | Write,
    Execute = 1 << 2,
};
DECLARE_ENUM_FLAG_OPERATORS(MemoryPermission)

/**
 * A low level linear memory buffer, which supports multiple mappings
 * Its purpose is to rebuild a given sparse memory layout, including mirrors.
 */
class HostMemory {
public:
    explicit HostMemory(size_t backing_size_, size_t virtual_size_,
                        HostMemoryFastmemRegion fastmem_region_ = {});
    ~HostMemory();

    /**
     * Copy constructors. They shall return a copy of the buffer without the mappings.
     * TODO: Implement them with COW if needed.
     */
    HostMemory(const HostMemory& other) = delete;
    HostMemory& operator=(const HostMemory& other) = delete;

    /**
     * Move constructors. They will move the buffer and the mappings to the new object.
     */
    HostMemory(HostMemory&& other) noexcept;
    HostMemory& operator=(HostMemory&& other) noexcept;

    void Map(size_t virtual_offset, size_t host_offset, size_t length, MemoryPermission perms,
             bool separate_heap);

    void Unmap(size_t virtual_offset, size_t length, bool separate_heap);

    void Protect(size_t virtual_offset, size_t length, MemoryPermission perms);

    void EnableDirectMappedAddress();

    void ClearBackingRegion(size_t physical_offset, size_t length, u32 fill_value);

    [[nodiscard]] u8* BackingBasePointer() noexcept {
        return backing_base;
    }
    [[nodiscard]] const u8* BackingBasePointer() const noexcept {
        return backing_base;
    }

    [[nodiscard]] size_t BackingSize() const noexcept {
        return backing_size;
    }

    [[nodiscard]] u8* VirtualBasePointer() noexcept {
        return virtual_base;
    }
    [[nodiscard]] const u8* VirtualBasePointer() const noexcept {
        return virtual_base;
    }

    bool IsInVirtualRange(void* address) const noexcept {
        return address >= virtual_base && address < virtual_base + virtual_size;
    }

private:
    size_t backing_size{};
    size_t virtual_size{};

#if !(defined(__OPENORBIS__) || defined(__managarm__))
    // Low level handler for the platform dependent memory routines
    class Impl;
    std::unique_ptr<Impl> impl;
#endif
    u8* backing_base{};
    u8* virtual_base{};
    size_t virtual_base_offset{};
    // Windows requires it for kernels whom lack proper support for some functions!
    bool fallback_buffer{false};
};

/// Xbox fastmem: MiB committed in the DRAM section and the demand-commit failures so far, for the
/// diag. Empty elsewhere.
std::string HostMemoryCommitStats();

/// Bytes of the emulated DRAM committed in the app (Xbox host memory). Empty where it cannot be
/// told apart, such as a file-backed section.
std::optional<u64> EmulatedDramCommittedBytes();

/// Xbox private DRAM backing: the run of host pages around `host` that share its commit state,
/// clamped to the backing. `untouched` pages are reserved but never committed, and the guest reads
/// them as zero. Empty outside such a backing (fastmem section, file, desktop).
struct DramRegion {
    const u8* begin{};
    const u8* end{};
    bool untouched{};
};
std::optional<DramRegion> QueryDramRegion(const void* host);

/// Diagnostic: the commit state of the DRAM page at `host`, and the last kernel clear that
/// decommitted it.
std::string DescribeDramPage(const void* host);

/// Diagnostic: queue a DRAM page whose first write after ArmDramWriteWatch is logged.
void WatchDramPage(const void* host);

/// Diagnostic: forget the queued pages.
void ResetDramWatch();

/// Diagnostic: each queued page as its nonzero word count, or 'r' when never committed.
std::string DescribeWatchedPages();

/// Diagnostic: write-protect the queued pages; each one logs its first writer and unprotects.
void ArmDramWriteWatch();

/// Diagnostic: names the kernel path whose DRAM clears follow, on this thread.
class ScopedClearTag {
public:
    explicit ScopedClearTag(const char* tag);
    ~ScopedClearTag();

private:
    const char* previous;
};

} // namespace Common
