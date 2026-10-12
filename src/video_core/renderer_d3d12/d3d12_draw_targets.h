// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <span>

#include <d3d12.h>

#include "common/common_types.h"

namespace D3D12 {

[[nodiscard]] inline size_t DrawColorTargetCount(size_t count, u8 bound_mask) {
    while (count && !(bound_mask >> (count - 1) & 1U)) {
        --count;
    }
    return count;
}

template <typename Attachments>
[[nodiscard]] u8 WritableColorTargetMask(const Attachments& attachments) {
    u8 result{};
    for (size_t index = 0; index < attachments.size(); ++index) {
        const auto mask = attachments[index].Mask();
        if (mask[0] || mask[1] || mask[2] || mask[3]) {
            result |= static_cast<u8>(1U << index);
        }
    }
    return result;
}

[[nodiscard]] inline DXGI_FORMAT DrawColorTargetFormat(DXGI_FORMAT format, u8 writable_mask,
                                                      size_t index) {
    return (writable_mask >> index & 1U) ? format : DXGI_FORMAT_UNKNOWN;
}

// Keep SV_Target indices intact; trailing unused slots may be omitted. A zero write mask does not
// make overlapping physical RTV bindings legal; use the existing null RTV instead.
// The PSO uses DrawColorTargetFormat so null slots use UNKNOWN as required by D3D12.
[[nodiscard]] inline std::array<D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT>
DrawColorTargets(std::span<const D3D12_CPU_DESCRIPTOR_HANDLE> targets, u8 writable_mask,
                 D3D12_CPU_DESCRIPTOR_HANDLE null_target) {
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT> result{};
    for (size_t index = 0; index < targets.size(); ++index) {
        result[index] = (writable_mask >> index & 1U) ? targets[index] : null_target;
    }
    return result;
}

} // namespace D3D12
