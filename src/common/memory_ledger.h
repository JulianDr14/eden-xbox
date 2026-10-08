// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>
#include <string_view>

#include "common/common_types.h"

namespace Common {

/// Owners of memory that the OS reports only as an anonymous total. Each one keeps its bytes and
/// object count here, so a memory report can split the app's commit by owner.
enum class MemoryAccount : u8 {
    TextureHeaps,    ///< Texture heap pool (GPU-local heaps).
    BufferCache,     ///< Guest buffers (GPU-local).
    StagingRing,     ///< Upload stream ring (CPU-visible).
    StagingUpload,   ///< Dedicated upload buffers (CPU-visible).
    StagingReadback, ///< Dedicated readback buffers (CPU-visible).
    ShaderBytecode,  ///< Host shader bytecode kept to build pipeline variants.
    Pipelines,       ///< Pipeline objects: how many, and the host memory of their bookkeeping.
    PipelineStates,  ///< Resident driver PSOs: counted only, the driver does not tell their size.
    Count,
};

struct MemoryAccountTotals {
    u64 bytes;
    u64 objects;
};

/// Charges bytes and objects to an account for as long as it lives (RAII), so an owner keeps the
/// ledger right on every path that frees it, early returns and exceptions included.
class MemoryCharge {
public:
    MemoryCharge() noexcept = default;
    MemoryCharge(MemoryAccount account, u64 bytes, u64 objects = 1) noexcept;
    ~MemoryCharge();

    MemoryCharge(const MemoryCharge&) = delete;
    MemoryCharge& operator=(const MemoryCharge&) = delete;
    MemoryCharge(MemoryCharge&& other) noexcept;
    MemoryCharge& operator=(MemoryCharge&& other) noexcept;

    void Reset() noexcept;

private:
    MemoryAccount account{MemoryAccount::Count};
    u64 bytes{};
    u64 objects{};
};

[[nodiscard]] MemoryAccountTotals ReadMemoryAccount(MemoryAccount account) noexcept;

/// Whether GPU-local memory is system memory (a unified architecture, such as the Xbox): then the
/// texture and buffer accounts count against the app's commit as well. Set by the renderer.
void SetGpuMemoryUnified(bool unified) noexcept;

/// Bytes of every account that the process commit includes.
[[nodiscard]] u64 MemoryLedgerCommittedBytes() noexcept;

/// One line with every account, e.g. "textures 1418 MiB in 23 heaps, ...".
[[nodiscard]] std::string DescribeMemoryLedger();

} // namespace Common
