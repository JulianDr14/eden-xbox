// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <vector>

#include <d3d12.h>

#include "common/common_types.h"

namespace D3D12 {

/// The shader feature flags (the SFI0 part of the DXIL container): the optional features the
/// shader needs, which the driver checks against its caps. 0 when absent or malformed.
[[nodiscard]] u64 ShaderFeatureFlags(const std::vector<u8>& dxil);

/// E_INVALIDARG from CreateGraphicsPipelineState names no parameter, and the Xbox driver rejects
/// descs the PC one (and its debug layer) accept. Logs the desc and retries it with one part
/// simplified at a time: the variants that build point at the part the driver rejects. Only the
/// first few failures are diagnosed.
void DiagnoseFailedPipeline(ID3D12Device* device, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc);

} // namespace D3D12
