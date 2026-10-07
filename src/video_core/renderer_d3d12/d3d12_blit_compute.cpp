// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cstring>
#include <span>
#include <vector>

#include "common/div_ceil.h"
#include "video_core/host_shaders/d3d12_astc_decoder_comp_spv.h"
#include "video_core/host_shaders/d3d12_bc3_encoder_comp_spv.h"
#include "video_core/host_shaders/d3d12_depth_stencil_merge_comp_spv.h"
#include "video_core/host_shaders/d3d12_depth_stencil_split_comp_spv.h"
#include "video_core/renderer_d3d12/d3d12_blit_image.h"
#include "video_core/renderer_d3d12/d3d12_descriptor_heap.h"
#include "video_core/renderer_d3d12/d3d12_resource_utils.h"
#include "video_core/renderer_d3d12/d3d12_scheduler.h"
#include "video_core/renderer_d3d12/d3d12_shader_compiler.h"

// The compute helpers of BlitImageHelper: depth-stencil plane packing and the GPU ASTC decoder
// and BC3 encoder.

namespace D3D12 {

namespace {

/// spirv_to_dxil's compute runtime data (group counts, base group zero). Root constants a
/// dispatch does not set are undefined: they keep whatever the previous signature left there.
void SetComputeRuntimeData(ID3D12GraphicsCommandList* cmd, UINT param,
                           const std::array<u32, 3>& groups) {
    dxil_spirv_compute_runtime_data data{};
    data.group_count_x = groups[0];
    data.group_count_y = groups[1];
    data.group_count_z = groups[2];
    std::array<u32, sizeof(data) / sizeof(u32)> words{};
    std::memcpy(words.data(), &data, sizeof(data));
    cmd->SetComputeRoot32BitConstants(param, static_cast<UINT>(words.size()), words.data(), 0);
}

/// Root parameters of the depth-stencil pack shaders: their push constants, the buffer they read
/// (a readonly SSBO at binding 0 becomes t0) and the one they write (binding 1, u1). Root
/// descriptors need no descriptor heap; they have no bounds either, so the shaders stay inside
/// the region they are given.
constexpr u32 PACK_CONSTANTS_PARAM = 0;
constexpr u32 PACK_SOURCE_PARAM = 1;
constexpr u32 PACK_DESTINATION_PARAM = 2;
constexpr u32 PACK_CONSTANT_WORDS = 12;
constexpr u32 PACK_GROUP_SIZE = 8;

/// Root parameters of the ASTC decoder: its seven push-constant words, the staging blocks (a
/// readonly SSBO at binding 0, t0) and a table with the level's UAV (the storage image at binding
/// 1, u1: typed texture UAVs cannot be root descriptors).
constexpr u32 ASTC_CONSTANTS_PARAM = 0;
constexpr u32 ASTC_SOURCE_PARAM = 1;
constexpr u32 ASTC_DESTINATION_PARAM = 2;
constexpr u32 ASTC_CONSTANT_WORDS = 9;
constexpr u32 ASTC_GROUP_SIZE = 8;

constexpr u32 BC3_CONSTANTS_PARAM = 0;
constexpr u32 BC3_SOURCE_PARAM = 1;
constexpr u32 BC3_DESTINATION_PARAM = 2;
constexpr u32 BC3_CONSTANT_WORDS = 4;
constexpr u32 BC3_GROUP_SIZE = 8;

} // Anonymous namespace

void BlitImageHelper::CreatePackPipelines(const ShaderCompiler& compiler) {
    std::array<D3D12_ROOT_PARAMETER, 4> params{};
    params[PACK_CONSTANTS_PARAM].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[PACK_CONSTANTS_PARAM].Constants = {.ShaderRegister = 0,
                                              .RegisterSpace = PUSH_CONSTANT_SPACE,
                                              .Num32BitValues = PACK_CONSTANT_WORDS};
    params[PACK_SOURCE_PARAM].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[PACK_SOURCE_PARAM].Descriptor = {.ShaderRegister = 0, .RegisterSpace = 0};
    params[PACK_DESTINATION_PARAM].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[PACK_DESTINATION_PARAM].Descriptor = {.ShaderRegister = 1, .RegisterSpace = 0};
    // spirv_to_dxil's compute runtime data (group counts); these shaders do not read it, the
    // parameter only keeps the signature a superset of whatever the translator declares.
    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[3].Constants = {
        .ShaderRegister = 0, .RegisterSpace = RUNTIME_DATA_SPACE,
        .Num32BitValues = static_cast<UINT>(sizeof(dxil_spirv_compute_runtime_data) / sizeof(u32))};
    for (auto& param : params) {
        param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    const D3D12_ROOT_SIGNATURE_DESC desc{
        .NumParameters = static_cast<UINT>(params.size()),
        .pParameters = params.data(),
        .NumStaticSamplers = 0,
        .pStaticSamplers = nullptr,
        .Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE,
    };
    pack_root_signature =
        D3D12::CreateRootSignature(device.Get(), desc, "depth-stencil pack root signature");
    const auto create = [&](std::span<const u32> spirv, ComPtr<ID3D12PipelineState>& out) {
        const std::vector<u8> dxil = compiler.Compile(spirv, DXIL_SPIRV_SHADER_COMPUTE);
        const D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_desc{
            .pRootSignature = pack_root_signature.Get(),
            .CS = {dxil.data(), dxil.size()},
        };
        ThrowIfFailed(device.Get()->CreateComputePipelineState(&pipeline_desc,
                                                               IID_PPV_ARGS(&out)),
                      "CreateComputePipelineState (depth-stencil pack)");
    };
    create(D3D12_DEPTH_STENCIL_SPLIT_COMP_SPV, split_pipeline);
    create(D3D12_DEPTH_STENCIL_MERGE_COMP_SPV, merge_pipeline);
}

void BlitImageHelper::CreateAstcPipeline(const ShaderCompiler& compiler) {
    const D3D12_DESCRIPTOR_RANGE srv_range{
        .RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
        .NumDescriptors = 1,
        .BaseShaderRegister = 0,
        .RegisterSpace = 0,
        .OffsetInDescriptorsFromTableStart = 0,
    };
    const D3D12_DESCRIPTOR_RANGE uav_range{
        .RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV,
        .NumDescriptors = 1,
        .BaseShaderRegister = 1,
        .RegisterSpace = 0,
        .OffsetInDescriptorsFromTableStart = 0,
    };
    std::array<D3D12_ROOT_PARAMETER, 4> params{};
    params[ASTC_CONSTANTS_PARAM].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[ASTC_CONSTANTS_PARAM].Constants = {.ShaderRegister = 0,
                                              .RegisterSpace = PUSH_CONSTANT_SPACE,
                                              .Num32BitValues = ASTC_CONSTANT_WORDS};
    params[ASTC_SOURCE_PARAM].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[ASTC_SOURCE_PARAM].DescriptorTable = {.NumDescriptorRanges = 1,
                                                 .pDescriptorRanges = &srv_range};
    params[ASTC_DESTINATION_PARAM].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[ASTC_DESTINATION_PARAM].DescriptorTable = {.NumDescriptorRanges = 1,
                                                      .pDescriptorRanges = &uav_range};
    // As in the pack signature: spirv_to_dxil's compute runtime data, never read.
    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[3].Constants = {
        .ShaderRegister = 0, .RegisterSpace = RUNTIME_DATA_SPACE,
        .Num32BitValues = static_cast<UINT>(sizeof(dxil_spirv_compute_runtime_data) / sizeof(u32))};
    for (auto& param : params) {
        param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    const D3D12_ROOT_SIGNATURE_DESC desc{
        .NumParameters = static_cast<UINT>(params.size()),
        .pParameters = params.data(),
        .NumStaticSamplers = 0,
        .pStaticSamplers = nullptr,
        .Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE,
    };
    astc_root_signature =
        D3D12::CreateRootSignature(device.Get(), desc, "ASTC decoder root signature");
    const std::array<ShaderCompiler::PipelineStage, 1> stages{{
        {D3D12_ASTC_DECODER_COMP_SPV, DXIL_SPIRV_SHADER_COMPUTE},
    }};
    auto compiled = compiler.CompilePipeline(stages, {});
    const std::vector<u8>& dxil = compiled[0].dxil;
    const D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_desc{
        .pRootSignature = astc_root_signature.Get(),
        .CS = {dxil.data(), dxil.size()},
    };
    ThrowIfFailed(device.Get()->CreateComputePipelineState(&pipeline_desc,
                                                           IID_PPV_ARGS(&astc_pipeline)),
                  "CreateComputePipelineState (ASTC decoder)");
}

void BlitImageHelper::DecodeAstc(const AstcDecode& decode) {
    if (!astc_available || decode.blocks_x == 0 || decode.blocks_y == 0 || decode.layers == 0) {
        return;
    }
    // Descriptors first: a full ring flushes the command list.
    const std::array descriptors{decode.source, decode.destination};
    const D3D12_GPU_DESCRIPTOR_HANDLE table = descriptor_ring.Upload(descriptors);
    const D3D12_GPU_DESCRIPTOR_HANDLE destination{
        table.ptr + descriptor_ring.Stride(),
    };
    ID3D12GraphicsCommandList* const cmd = scheduler.CommandList();
    ID3D12DescriptorHeap* const heaps[] = {descriptor_ring.Heap(), sampler_heap.Heap()};
    cmd->SetDescriptorHeaps(2, heaps);
    cmd->SetComputeRootSignature(astc_root_signature.Get());
    scheduler.SetPipelineState(astc_pipeline.Get());
    const std::array<u32, ASTC_CONSTANT_WORDS> constants{
        decode.block_width, decode.block_height,     decode.layer_stride,         decode.block_size,
        decode.x_shift,     decode.gob_block_height, decode.gob_block_height_mask,
        decode.input_words, decode.first_block_row,
    };
    cmd->SetComputeRoot32BitConstants(ASTC_CONSTANTS_PARAM, ASTC_CONSTANT_WORDS, constants.data(),
                                      0);
    cmd->SetComputeRootDescriptorTable(ASTC_SOURCE_PARAM, table);
    cmd->SetComputeRootDescriptorTable(ASTC_DESTINATION_PARAM, destination);
    // One invocation per ASTC block.
    const std::array<u32, 3> groups{Common::DivCeil(decode.blocks_x, ASTC_GROUP_SIZE),
                                    Common::DivCeil(decode.blocks_y, ASTC_GROUP_SIZE),
                                    decode.layers};
    SetComputeRuntimeData(cmd, 3, groups);
    cmd->Dispatch(groups[0], groups[1], groups[2]);
}

void BlitImageHelper::CreateBc3Pipeline(const ShaderCompiler& compiler) {
    const D3D12_DESCRIPTOR_RANGE srv_range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
    std::array<D3D12_ROOT_PARAMETER, 4> params{};
    params[BC3_CONSTANTS_PARAM].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[BC3_CONSTANTS_PARAM].Constants = {.ShaderRegister = 0,
                                             .RegisterSpace = PUSH_CONSTANT_SPACE,
                                             .Num32BitValues = BC3_CONSTANT_WORDS};
    params[BC3_SOURCE_PARAM].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[BC3_SOURCE_PARAM].DescriptorTable = {1, &srv_range};
    params[BC3_DESTINATION_PARAM].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[BC3_DESTINATION_PARAM].Descriptor = {.ShaderRegister = 1, .RegisterSpace = 0};
    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[3].Constants = {
        .ShaderRegister = 0, .RegisterSpace = RUNTIME_DATA_SPACE,
        .Num32BitValues = static_cast<UINT>(sizeof(dxil_spirv_compute_runtime_data) / sizeof(u32))};
    for (auto& param : params) {
        param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    const D3D12_STATIC_SAMPLER_DESC sampler{
        .Filter = D3D12_FILTER_MIN_MAG_MIP_POINT,
        .AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        .AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        .AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        .MipLODBias = 0.0f,
        .MaxAnisotropy = 1,
        .ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER,
        .BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK,
        .MinLOD = 0.0f,
        .MaxLOD = D3D12_FLOAT32_MAX,
        .ShaderRegister = 0,
        .RegisterSpace = 0,
        .ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL,
    };
    const D3D12_ROOT_SIGNATURE_DESC desc{static_cast<UINT>(params.size()), params.data(), 1,
                                         &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    bc3_root_signature =
        D3D12::CreateRootSignature(device.Get(), desc, "BC3 encoder root signature");
    const std::array<ShaderCompiler::PipelineStage, 1> stages{{
        {D3D12_BC3_ENCODER_COMP_SPV, DXIL_SPIRV_SHADER_COMPUTE},
    }};
    auto compiled = compiler.CompilePipeline(stages, {});
    const auto& dxil = compiled[0].dxil;
    const D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_desc{
        .pRootSignature = bc3_root_signature.Get(), .CS = {dxil.data(), dxil.size()}};
    ThrowIfFailed(device.Get()->CreateComputePipelineState(&pipeline_desc,
                                                           IID_PPV_ARGS(&bc3_pipeline)),
                  "CreateComputePipelineState (BC3 encoder)");
}

void BlitImageHelper::EncodeBc3(const Bc3Encode& encode) {
    if (!bc3_available || encode.width == 0 || encode.band_height == 0) {
        return;
    }
    const D3D12_GPU_DESCRIPTOR_HANDLE table = descriptor_ring.Upload({&encode.source, 1});
    ID3D12GraphicsCommandList* const cmd = scheduler.CommandList();
    ID3D12DescriptorHeap* const heaps[] = {descriptor_ring.Heap(), sampler_heap.Heap()};
    cmd->SetDescriptorHeaps(2, heaps);
    cmd->SetComputeRootSignature(bc3_root_signature.Get());
    scheduler.SetPipelineState(bc3_pipeline.Get());
    const std::array<u32, BC3_CONSTANT_WORDS> constants{
        encode.width, encode.height, encode.band_height, encode.output_row_words};
    cmd->SetComputeRoot32BitConstants(BC3_CONSTANTS_PARAM, BC3_CONSTANT_WORDS, constants.data(), 0);
    cmd->SetComputeRootDescriptorTable(BC3_SOURCE_PARAM, table);
    cmd->SetComputeRootUnorderedAccessView(BC3_DESTINATION_PARAM, encode.destination);
    const std::array<u32, 3> groups{Common::DivCeil(encode.width, 4U * BC3_GROUP_SIZE),
                                    Common::DivCeil(encode.band_height, 4U * BC3_GROUP_SIZE), 1U};
    SetComputeRuntimeData(cmd, 3, groups);
    cmd->Dispatch(groups[0], groups[1], groups[2]);
}

void BlitImageHelper::DispatchPack(ID3D12PipelineState* pipeline, const DepthStencilPack& pack,
                                   u32 groups_x, u32 groups_y) {
    if (groups_x == 0 || groups_y == 0) {
        return;
    }
    ID3D12GraphicsCommandList* const cmd = scheduler.CommandList();
    cmd->SetComputeRootSignature(pack_root_signature.Get());
    scheduler.SetPipelineState(pipeline);
    const std::array<u32, PACK_CONSTANT_WORDS> constants{
        pack.packed_offset, pack.packed_row,  pack.depth_pitch, pack.stencil_offset,
        pack.stencil_pitch, pack.x,           pack.y,           pack.width,
        pack.height,        pack.row_texels, static_cast<u32>(pack.layout), 0,
    };
    cmd->SetComputeRoot32BitConstants(PACK_CONSTANTS_PARAM, PACK_CONSTANT_WORDS, constants.data(),
                                      0);
    cmd->SetComputeRootShaderResourceView(PACK_SOURCE_PARAM, pack.source);
    cmd->SetComputeRootUnorderedAccessView(PACK_DESTINATION_PARAM, pack.destination);
    cmd->Dispatch(groups_x, groups_y, 1);
}

void BlitImageHelper::SplitDepthStencil(const DepthStencilPack& pack) {
    if (!pack_available || pack.width == 0 || pack.height == 0) {
        return;
    }
    // One invocation per stencil word: the four texels of a row it covers.
    const u32 first_word = pack.x / 4;
    const u32 last_word = (pack.x + pack.width - 1) / 4;
    DispatchPack(split_pipeline.Get(), pack,
                 Common::DivCeil(last_word - first_word + 1, PACK_GROUP_SIZE),
                 Common::DivCeil(pack.height, PACK_GROUP_SIZE));
}

void BlitImageHelper::MergeDepthStencil(const DepthStencilPack& pack) {
    if (!pack_available || pack.width == 0 || pack.height == 0) {
        return;
    }
    DispatchPack(merge_pipeline.Get(), pack,
                 Common::DivCeil(std::max(pack.row_texels, pack.width), PACK_GROUP_SIZE),
                 Common::DivCeil(pack.height, PACK_GROUP_SIZE));
}

} // namespace D3D12
