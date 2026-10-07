// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>
#include <cstring>
#include <string>

#include <fmt/format.h>

#include "common/logging.h"
#include "video_core/renderer_d3d12/d3d12_device.h"
#include "video_core/renderer_d3d12/diagnostics/d3d12_pipeline_diagnostics.h"

namespace D3D12 {

namespace {

/// Failed pipelines diagnosed so far; each one creates several extra PSOs, so only the first few.
std::atomic_int diagnosed_failures;
constexpr int MAX_DIAGNOSED_FAILURES = 6;

} // Anonymous namespace

u64 ShaderFeatureFlags(const std::vector<u8>& dxil) {
    const auto read_u32 = [&](size_t offset) {
        u32 value{};
        std::memcpy(&value, dxil.data() + offset, sizeof(value));
        return value;
    };
    constexpr size_t HEADER_SIZE = 32; // "DXBC", digest, version, size, part count
    if (dxil.size() < HEADER_SIZE || std::memcmp(dxil.data(), "DXBC", 4) != 0) {
        return 0;
    }
    const u32 part_count = read_u32(28);
    for (u32 part = 0; part < part_count; ++part) {
        const size_t index_offset = HEADER_SIZE + part * sizeof(u32);
        if (index_offset + sizeof(u32) > dxil.size()) {
            return 0;
        }
        const size_t part_offset = read_u32(index_offset);
        if (part_offset + 16 > dxil.size()) {
            return 0;
        }
        if (std::memcmp(dxil.data() + part_offset, "SFI0", 4) == 0) {
            u64 flags{};
            std::memcpy(&flags, dxil.data() + part_offset + 8, sizeof(flags));
            return flags;
        }
    }
    return 0;
}

void DiagnoseFailedPipeline(ID3D12Device* device, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc) {
    if (diagnosed_failures.fetch_add(1, std::memory_order_relaxed) >= MAX_DIAGNOSED_FAILURES) {
        return;
    }
    for (UINT i = 0; i < desc.InputLayout.NumElements; ++i) {
        const D3D12_INPUT_ELEMENT_DESC& element = desc.InputLayout.pInputElementDescs[i];
        D3D12_FEATURE_DATA_FORMAT_SUPPORT support{.Format = element.Format};
        const bool ia_support =
            SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support,
                                                  sizeof(support))) &&
            (support.Support1 & D3D12_FORMAT_SUPPORT1_IA_VERTEX_BUFFER) != 0;
        LOG_ERROR(Render,
                  "D3D12 PSO diag:   TEXCOORD{} format {} (IA vertex buffer {}) slot {} offset {} "
                  "{} step {}",
                  element.SemanticIndex, static_cast<u32>(element.Format),
                  ia_support ? "yes" : "NO", element.InputSlot, element.AlignedByteOffset,
                  element.InputSlotClass == D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA
                      ? "instance"
                      : "vertex",
                  element.InstanceDataStepRate);
    }
    const D3D12_RASTERIZER_DESC& raster = desc.RasterizerState;
    LOG_ERROR(Render,
              "D3D12 PSO diag:   VS {} B, PS {} B, HS {} B, DS {} B, GS {} B; topology type {}, "
              "strip cut {}, samples {}",
              desc.VS.BytecodeLength, desc.PS.BytecodeLength, desc.HS.BytecodeLength,
              desc.DS.BytecodeLength, desc.GS.BytecodeLength,
              static_cast<u32>(desc.PrimitiveTopologyType), static_cast<u32>(desc.IBStripCutValue),
              desc.SampleDesc.Count);
    LOG_ERROR(Render,
              "D3D12 PSO diag:   raster fill {} cull {} ccw {} bias {} clamp {} slope {} clip {} "
              "aa lines {}; depth {} write {} func {} stencil {}; alpha to coverage {}",
              static_cast<u32>(raster.FillMode), static_cast<u32>(raster.CullMode),
              raster.FrontCounterClockwise, raster.DepthBias, raster.DepthBiasClamp,
              raster.SlopeScaledDepthBias, raster.DepthClipEnable, raster.AntialiasedLineEnable,
              desc.DepthStencilState.DepthEnable,
              static_cast<u32>(desc.DepthStencilState.DepthWriteMask),
              static_cast<u32>(desc.DepthStencilState.DepthFunc),
              desc.DepthStencilState.StencilEnable, desc.BlendState.AlphaToCoverageEnable);
    for (UINT i = 0; i < desc.NumRenderTargets; ++i) {
        const D3D12_RENDER_TARGET_BLEND_DESC& blend = desc.BlendState.RenderTarget[i];
        LOG_ERROR(Render,
                  "D3D12 PSO diag:   RT{} format {} blend {} rgb {}/{}/{} alpha {}/{}/{} mask {:x}",
                  i, static_cast<u32>(desc.RTVFormats[i]), blend.BlendEnable,
                  static_cast<u32>(blend.SrcBlend), static_cast<u32>(blend.DestBlend),
                  static_cast<u32>(blend.BlendOp), static_cast<u32>(blend.SrcBlendAlpha),
                  static_cast<u32>(blend.DestBlendAlpha), static_cast<u32>(blend.BlendOpAlpha),
                  blend.RenderTargetWriteMask);
    }

    const auto try_variant = [&](const char* name, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& v) {
        ComPtr<ID3D12PipelineState> pso;
        const HRESULT hr = device->CreateGraphicsPipelineState(&v, IID_PPV_ARGS(&pso));
        CheckRemovedAfter(device, [&] { return fmt::format("PSO diag variant '{}'", name); });
        LOG_ERROR(Render, "D3D12 PSO diag: variant '{}' -> {}", name,
                  SUCCEEDED(hr) ? std::string("BUILDS")
                                : fmt::format("fails 0x{:08X}", static_cast<u32>(hr)));
    };
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC v = desc;
        v.PS = {};
        try_variant("no pixel shader", v);
    }
    {
        std::vector<D3D12_INPUT_ELEMENT_DESC> elements(
            desc.InputLayout.pInputElementDescs,
            desc.InputLayout.pInputElementDescs + desc.InputLayout.NumElements);
        for (D3D12_INPUT_ELEMENT_DESC& element : elements) {
            element.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        }
        D3D12_GRAPHICS_PIPELINE_STATE_DESC v = desc;
        v.InputLayout.pInputElementDescs = elements.empty() ? nullptr : elements.data();
        try_variant("all attributes RGBA32F", v);
    }
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC v = desc;
        v.RasterizerState = {
            .FillMode = D3D12_FILL_MODE_SOLID,
            .CullMode = D3D12_CULL_MODE_NONE,
            .FrontCounterClockwise = FALSE,
            .DepthBias = 0,
            .DepthBiasClamp = 0.0f,
            .SlopeScaledDepthBias = 0.0f,
            .DepthClipEnable = TRUE,
            .MultisampleEnable = FALSE,
            .AntialiasedLineEnable = FALSE,
            .ForcedSampleCount = 0,
            .ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF,
        };
        try_variant("default rasterizer", v);
    }
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC v = desc;
        v.BlendState.AlphaToCoverageEnable = FALSE;
        for (UINT i = 0; i < v.NumRenderTargets; ++i) {
            D3D12_RENDER_TARGET_BLEND_DESC& blend = v.BlendState.RenderTarget[i];
            blend.BlendEnable = FALSE;
            blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        }
        try_variant("no blending", v);
    }
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC v = desc;
        v.DepthStencilState.DepthEnable = FALSE;
        v.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        v.DepthStencilState.StencilEnable = FALSE;
        try_variant("no depth-stencil test", v);
    }
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC v = desc;
        v.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
        try_variant("no strip cut", v);
    }
}

} // namespace D3D12
