// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/memory_ledger.h"
#include "video_core/renderer_d3d12/d3d12_device.h"
#include "video_core/renderer_d3d12/d3d12_live_pipeline_cache.h"
#include "video_core/renderer_d3d12/d3d12_pso_key.h"

namespace D3D12 {

struct ResidentGraphicsPso {
    // A retire callback can outlive the guest pipeline/root-signature index at shutdown.
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> handle;
    Common::MemoryCharge charge{Common::MemoryAccount::PipelineStates, 0};
    Common::MemoryCharge index_charge;
};

using GraphicsPsoCache = LivePipelineCache<GraphicsPsoKey, ResidentGraphicsPso, GraphicsPsoKeyHash>;
using SharedGraphicsPso = std::shared_ptr<const GraphicsPsoCache::Entry>;

} // namespace D3D12
