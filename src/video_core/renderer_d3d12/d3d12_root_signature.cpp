// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cstddef>
#include <stdexcept>
#include <string>

#include <eden_spirv_to_dxil.h>
#include <fmt/format.h>
#include <spirv_to_dxil.h>

#include "common/bug_tracker.h"
#include "common/cityhash.h"
#include "common/logging.h"
#include "video_core/renderer_d3d12/d3d12_resource_utils.h"
#include "video_core/renderer_d3d12/d3d12_root_signature.h"
#include "video_core/renderer_d3d12/d3d12_shader_compiler.h"

namespace D3D12 {

namespace {

/// Descriptor ranges of both tables, in slot order.
struct TableRanges {
    std::vector<D3D12_DESCRIPTOR_RANGE> resources;
    std::vector<D3D12_DESCRIPTOR_RANGE> samplers;
    u32 num_resources{};
    u32 num_samplers{};

    void Add(D3D12_DESCRIPTOR_RANGE_TYPE type, u32 reg, u32 count) {
        const bool sampler = type == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
        u32& offset = sampler ? num_samplers : num_resources;
        (sampler ? samplers : resources)
            .push_back({
                .RangeType = type,
                .NumDescriptors = count,
                .BaseShaderRegister = reg,
                .RegisterSpace = 0,
                .OffsetInDescriptorsFromTableStart = offset,
            });
        offset += count;
    }
};

/// Walks one stage's descriptors in the order spirv_emit_context.cpp assigns bindings.
void AddStage(const Shader::Info& info, u32& binding, TableRanges& ranges) {
    for (const auto& desc : info.constant_buffer_descriptors) {
        ranges.Add(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, binding, desc.count);
        binding += desc.count;
    }
    for (const auto& desc : info.storage_buffers_descriptors) {
        ranges.Add(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, binding, desc.count);
        ranges.Add(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, binding, desc.count);
        binding += desc.count;
    }
    // Texture buffers, image buffers and images take one binding each (the backend rejects or
    // ignores arrays of them), so their ranges hold a single descriptor.
    for (size_t i = 0; i < info.texture_buffer_descriptors.size(); ++i) {
        ranges.Add(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, binding, 1);
        ++binding;
    }
    for (size_t i = 0; i < info.image_buffer_descriptors.size(); ++i) {
        ranges.Add(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, binding, 1);
        ranges.Add(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, binding, 1);
        ++binding;
    }
    // Profile::descriptor_arrays_use_count: a texture array takes one binding per element.
    for (const auto& desc : info.texture_descriptors) {
        ranges.Add(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, binding, desc.count);
        ranges.Add(D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, binding, desc.count);
        binding += desc.count;
    }
    for (const auto& desc : info.image_descriptors) {
        if (desc.count != 1) {
            BUG_TRACK_KEY(UnsupportedState, desc.count,
                          "storage image array of {} elements: only the first is bound",
                          desc.count);
            const Common::BugTracker::TapMute bug_tracker_mute;
            LOG_WARNING(Render, "D3D12: image array of {} elements; only the first is bound",
                        desc.count);
        }
        ranges.Add(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, binding, 1);
        ranges.Add(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, binding, 1);
        ++binding;
    }
}

void AppendKey(std::vector<u32>& key, std::span<const D3D12_DESCRIPTOR_RANGE> ranges) {
    key.push_back(static_cast<u32>(ranges.size()));
    for (const D3D12_DESCRIPTOR_RANGE& range : ranges) {
        key.push_back(static_cast<u32>(range.RangeType));
        key.push_back(range.NumDescriptors);
        key.push_back(range.BaseShaderRegister);
    }
}

} // Anonymous namespace

u32 FirstTextureBinding(const Shader::Info& info, u32 stage_binding) {
    return stage_binding + Shader::NumDescriptors(info.constant_buffer_descriptors) +
           Shader::NumDescriptors(info.storage_buffers_descriptors) +
           static_cast<u32>(info.texture_buffer_descriptors.size()) +
           static_cast<u32>(info.image_buffer_descriptors.size());
}

u32 NumStageBindings(const Shader::Info& info) {
    return FirstTextureBinding(info, 0) + Shader::NumDescriptors(info.texture_descriptors) +
           static_cast<u32>(info.image_descriptors.size());
}

RootSignatureCache::RootSignatureCache(const Device& device_) : device{device_} {}

RootSignatureCache::~RootSignatureCache() = default;

void RootSignatureCache::CreateCommandSignatures(PipelineLayout& layout, bool is_compute) {
    static_assert(offsetof(dxil_spirv_vertex_runtime_data, draw_id) ==
                  (INDIRECT_DRAW_CONSTANT_WORDS - 1) * sizeof(u32));
    static_assert(offsetof(dxil_spirv_compute_runtime_data, group_count_z) ==
                  (INDIRECT_DISPATCH_CONSTANT_WORDS - 1) * sizeof(u32));
    const auto create = [&](D3D12_INDIRECT_ARGUMENT_TYPE type, u32 constant_words, u32 words,
                            ComPtr<ID3D12CommandSignature>& out) {
        const std::array<D3D12_INDIRECT_ARGUMENT_DESC, 2> arguments{{
            {.Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT,
             .Constant = {.RootParameterIndex = PipelineLayout::RUNTIME_DATA_INDEX,
                          .DestOffsetIn32BitValues = 0,
                          .Num32BitValuesToSet = constant_words}},
            {.Type = type},
        }};
        const D3D12_COMMAND_SIGNATURE_DESC desc{
            .ByteStride = words * static_cast<u32>(sizeof(u32)),
            .NumArgumentDescs = static_cast<UINT>(arguments.size()),
            .pArgumentDescs = arguments.data(),
            .NodeMask = 0,
        };
        const HRESULT hr = device.Get()->CreateCommandSignature(
            &desc, layout.root_signature.Get(), IID_PPV_ARGS(&out));
        if (FAILED(hr)) {
            LOG_ERROR(Render, "D3D12: CreateCommandSignature failed (0x{:08X})",
                      static_cast<u32>(hr));
        }
    };
    if (is_compute) {
        create(D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH, INDIRECT_DISPATCH_CONSTANT_WORDS,
               INDIRECT_DISPATCH_WORDS, layout.dispatch_signature);
    } else {
        create(D3D12_INDIRECT_ARGUMENT_TYPE_DRAW, INDIRECT_DRAW_CONSTANT_WORDS,
               INDIRECT_DRAW_WORDS, layout.draw_signature);
        create(D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED, INDIRECT_DRAW_CONSTANT_WORDS,
               INDIRECT_DRAW_INDEXED_WORDS, layout.draw_indexed_signature);
    }
}

const PipelineLayout& RootSignatureCache::Get(std::span<const Shader::Info* const> infos,
                                              bool is_compute) {
    TableRanges ranges;
    u32 binding = 0;
    bool integer_textures = false;
    for (const Shader::Info* info : infos) {
        if (info) {
            AddStage(*info, binding, ranges);
            for (const auto& desc : info->texture_descriptors) {
                integer_textures |= desc.is_integer;
            }
        }
    }
    if (integer_textures && binding > EDEN_INTEGER_SAMPLER_MAX_BINDINGS) {
        throw std::runtime_error(fmt::format(
            "D3D12: {} bindings exceed the integer sampler table", binding));
    }
    std::vector<u32> key{(is_compute ? 1u : 0u) | (integer_textures ? 2u : 0u)};
    AppendKey(key, ranges.resources);
    AppendKey(key, ranges.samplers);
    const u64 hash = Common::CityHash64(reinterpret_cast<const char*>(key.data()),
                                        key.size() * sizeof(u32));

    std::scoped_lock lock{mutex};
    auto& bucket = layouts[hash];
    for (const auto& [existing_key, layout] : bucket) {
        if (existing_key == key) {
            return *layout;
        }
    }

    auto layout = std::make_unique<PipelineLayout>();
    layout->runtime_data_words =
        static_cast<u32>((is_compute ? sizeof(dxil_spirv_compute_runtime_data)
                                     : sizeof(dxil_spirv_vertex_runtime_data)) /
                         sizeof(u32));
    layout->num_resources = ranges.num_resources;
    layout->num_samplers = ranges.num_samplers;

    std::vector<D3D12_ROOT_PARAMETER> params;
    params.push_back({
        .ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS,
        .Constants = {.ShaderRegister = 0,
                      .RegisterSpace = PUSH_CONSTANT_SPACE,
                      .Num32BitValues = PUSH_CONSTANT_WORDS},
        .ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL,
    });
    params.push_back({
        .ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS,
        .Constants = {.ShaderRegister = 0,
                      .RegisterSpace = RUNTIME_DATA_SPACE,
                      .Num32BitValues = layout->runtime_data_words},
        .ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL,
    });
    if (!ranges.resources.empty()) {
        layout->resource_table_index = static_cast<u32>(params.size());
        params.push_back({
            .ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE,
            .DescriptorTable = {.NumDescriptorRanges = static_cast<UINT>(ranges.resources.size()),
                                .pDescriptorRanges = ranges.resources.data()},
            .ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL,
        });
    }
    if (!ranges.samplers.empty()) {
        layout->sampler_table_index = static_cast<u32>(params.size());
        params.push_back({
            .ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE,
            .DescriptorTable = {.NumDescriptorRanges = static_cast<UINT>(ranges.samplers.size()),
                                .pDescriptorRanges = ranges.samplers.data()},
            .ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL,
        });
    }
    if (integer_textures) {
        layout->integer_sampler_index = static_cast<u32>(params.size());
        params.push_back({
            .ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV,
            .Descriptor = {.ShaderRegister = 0, .RegisterSpace = EDEN_INTEGER_SAMPLER_SPACE},
            .ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL,
        });
    }
    const D3D12_ROOT_SIGNATURE_DESC desc{
        .NumParameters = static_cast<UINT>(params.size()),
        .pParameters = params.data(),
        .NumStaticSamplers = 0,
        .pStaticSamplers = nullptr,
        .Flags = is_compute ? D3D12_ROOT_SIGNATURE_FLAG_NONE
                            : D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT,
    };
    layout->root_signature = CreateRootSignature(device.Get(), desc, "guest root signature");
    CreateCommandSignatures(*layout, is_compute);
    LOG_DEBUG(Render, "D3D12: root signature {:016x} ({} resource and {} sampler descriptors)",
              hash, ranges.num_resources, ranges.num_samplers);

    bucket.emplace_back(std::move(key), std::move(layout));
    return *bucket.back().second;
}

} // namespace D3D12
