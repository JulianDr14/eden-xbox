// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>
#include <d3d12.h>

namespace D3D12 {

/// Host identity, independent of the guest key and pointer/padding representations. Shader
/// spans are kept alive by bytecode_owner. Root signatures are interned by PipelineCache and
/// outlive this per-device index. Hash collisions always compare the complete input bytes.
struct GraphicsPsoKey {
    std::shared_ptr<const void> bytecode_owner;
    std::array<std::span<const std::uint8_t>, 5> shaders;
    ID3D12RootSignature* root{};
    std::vector<std::uint32_t> state;

    bool operator==(const GraphicsPsoKey& other) const {
        if (root != other.root || state != other.state) {
            return false;
        }
        for (size_t i = 0; i < shaders.size(); ++i) {
            if (!std::ranges::equal(shaders[i], other.shaders[i])) {
                return false;
            }
        }
        return true;
    }
};

struct GraphicsPsoKeyHash {
    size_t operator()(const GraphicsPsoKey& key) const noexcept {
        std::uint64_t hash = reinterpret_cast<std::uintptr_t>(key.root);
        const auto add = [&](std::uint64_t value) { hash = (hash ^ value) * 1099511628211ULL; };
        for (const auto word : key.state) {
            add(word);
        }
        for (const auto shader : key.shaders) {
            add(shader.size());
            for (const auto byte : shader) {
                add(byte);
            }
        }
        return static_cast<size_t>(hash);
    }
};

inline GraphicsPsoKey MakeGraphicsPsoKey(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc,
                                        std::shared_ptr<const void> bytecode_owner) {
    GraphicsPsoKey key{.bytecode_owner = std::move(bytecode_owner), .root = desc.pRootSignature};
    const std::array bytecodes{desc.VS, desc.HS, desc.DS, desc.GS, desc.PS};
    for (size_t i = 0; i < bytecodes.size(); ++i) {
        key.shaders[i] = {static_cast<const std::uint8_t*>(bytecodes[i].pShaderBytecode),
                          bytecodes[i].BytecodeLength};
    }
    auto& words = key.state;
    words.reserve(256 + desc.InputLayout.NumElements * 16);
    const auto add = [&](auto value) { words.push_back(static_cast<std::uint32_t>(value)); };
    const auto name = [&](const char* text) {
        const std::string_view string = text ? text : "";
        add(string.size());
        for (const unsigned char c : string) {
            add(c);
        }
    };
    add(desc.StreamOutput.NumEntries);
    for (UINT i = 0; i < desc.StreamOutput.NumEntries; ++i) {
        const auto& e = desc.StreamOutput.pSODeclaration[i];
        add(e.Stream); name(e.SemanticName); add(e.SemanticIndex);
        add(e.StartComponent); add(e.ComponentCount); add(e.OutputSlot);
    }
    add(desc.StreamOutput.NumStrides);
    for (UINT i = 0; i < desc.StreamOutput.NumStrides; ++i) {
        add(desc.StreamOutput.pBufferStrides[i]);
    }
    add(desc.StreamOutput.RasterizedStream);
    add(desc.BlendState.AlphaToCoverageEnable); add(desc.BlendState.IndependentBlendEnable);
    for (const auto& rt : desc.BlendState.RenderTarget) {
        add(rt.BlendEnable); add(rt.LogicOpEnable); add(rt.SrcBlend); add(rt.DestBlend);
        add(rt.BlendOp); add(rt.SrcBlendAlpha); add(rt.DestBlendAlpha); add(rt.BlendOpAlpha);
        add(rt.LogicOp); add(rt.RenderTargetWriteMask);
    }
    add(desc.SampleMask);
    const auto& r = desc.RasterizerState;
    add(r.FillMode); add(r.CullMode); add(r.FrontCounterClockwise); add(r.DepthBias);
    add(std::bit_cast<std::uint32_t>(r.DepthBiasClamp));
    add(std::bit_cast<std::uint32_t>(r.SlopeScaledDepthBias));
    add(r.DepthClipEnable); add(r.MultisampleEnable); add(r.AntialiasedLineEnable);
    add(r.ForcedSampleCount); add(r.ConservativeRaster);
    const auto& d = desc.DepthStencilState;
    add(d.DepthEnable); add(d.DepthWriteMask); add(d.DepthFunc); add(d.StencilEnable);
    add(d.StencilReadMask); add(d.StencilWriteMask);
    for (const auto& f : {d.FrontFace, d.BackFace}) {
        add(f.StencilFailOp); add(f.StencilDepthFailOp); add(f.StencilPassOp); add(f.StencilFunc);
    }
    add(desc.InputLayout.NumElements);
    for (UINT i = 0; i < desc.InputLayout.NumElements; ++i) {
        const auto& e = desc.InputLayout.pInputElementDescs[i];
        name(e.SemanticName); add(e.SemanticIndex); add(e.Format); add(e.InputSlot);
        add(e.AlignedByteOffset); add(e.InputSlotClass); add(e.InstanceDataStepRate);
    }
    add(desc.IBStripCutValue); add(desc.PrimitiveTopologyType); add(desc.NumRenderTargets);
    for (const auto format : desc.RTVFormats) {
        add(format);
    }
    add(desc.DSVFormat); add(desc.SampleDesc.Count); add(desc.SampleDesc.Quality);
    add(desc.NodeMask); add(desc.Flags);
    // CachedPSO is a driver optimization, not functional identity. No native blobs on Series.
    return key;
}

} // namespace D3D12
