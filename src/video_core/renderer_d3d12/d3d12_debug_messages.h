// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string_view>
#include <d3d12sdklayers.h>

namespace D3D12 {

// Older Windows debug layers compare a typed null RTV against UNKNOWN in the PSO.
// Xenia documents this layer bug in D3D12RenderTargetCache's null RTV setup.
// Do not filter ID 613 globally: non-null and non-UNKNOWN mismatches are real errors.
[[nodiscard]] inline bool IsNullRtvFormatFalsePositive(const D3D12_MESSAGE& message) {
    if (message.ID != D3D12_MESSAGE_ID_RENDER_TARGET_FORMAT_MISMATCH_PIPELINE_STATE ||
        message.Severity != D3D12_MESSAGE_SEVERITY_ERROR || !message.pDescription) {
        return false;
    }
    const std::string_view text{message.pDescription, message.DescriptionByteLength};
    if (!text.starts_with("ID3D12CommandList::Draw") ||
        text.find("The render target format in slot ") == std::string_view::npos) {
        return false;
    }
    constexpr std::string_view prefix = "(pipeline state = UNKNOWN, RTV ID3D12Resource* = 0x";
    const auto start = text.find(prefix);
    if (start == std::string_view::npos) {
        return false;
    }
    const auto pointer = text.substr(start + prefix.size());
    const auto end = pointer.find_first_not_of('0');
    return end != std::string_view::npos && end > 0 &&
           pointer.substr(end).starts_with(":'(nullptr)')");
}

} // namespace D3D12
