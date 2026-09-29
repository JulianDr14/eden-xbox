// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/device_memory.h"
#include "hle/kernel/board/nintendo/nx/k_system_control.h"
#include "hle/kernel/k_trace.h"

namespace Core {

#ifdef HAS_NCE
constexpr size_t VirtualReserveSize = 1ULL << 38;
#else
constexpr size_t VirtualReserveSize = 1ULL << 39;
#endif

Common::HostMemoryFastmemRegion GetFastmemRegion() {
    using Kernel::Board::Nintendo::Nx::KSystemControl;
    const size_t dram_size = KSystemControl::Init::GetIntendedMemorySize();
    const size_t application_size = KSystemControl::Init::GetApplicationPoolSize();
    const size_t hot_size = (std::min)(Common::HostMemoryFastmemHotSize(), application_size);
    const size_t application_end = dram_size - Kernel::KTraceBufferSize;
    return {
        // Application processes allocate their large page groups from the back of this pool in
        // practice. Wonder maps the continuous upper tail; the lower edge was untouched.
        .offset = application_end - hot_size,
        .size = hot_size,
        .force_full = Common::HostMemoryFastmemForceFull(),
    };
}

DeviceMemory::DeviceMemory()
    : buffer{Kernel::Board::Nintendo::Nx::KSystemControl::Init::GetIntendedMemorySize(),
             VirtualReserveSize, GetFastmemRegion()} {}

DeviceMemory::~DeviceMemory() = default;

} // namespace Core
