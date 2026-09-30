// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "common/div_ceil.h"
#include "common/logging.h"
#include "video_core/host_shaders/d3d12_astc_decoder_comp_spv.h"
#include "video_core/host_shaders/blit_color_float_frag_spv.h"
#include "video_core/host_shaders/blit_depth_frag_spv.h"
#include "video_core/host_shaders/d3d12_depth_stencil_merge_comp_spv.h"
#include "video_core/host_shaders/d3d12_depth_stencil_split_comp_spv.h"
#include "video_core/host_shaders/d3d12_bc3_encoder_comp_spv.h"
#include "video_core/host_shaders/full_screen_triangle_vert_spv.h"
#include "video_core/host_shaders/vulkan_color_clear_frag_spv.h"
#include "video_core/host_shaders/vulkan_color_clear_vert_spv.h"
#include "video_core/host_shaders/vulkan_depthstencil_clear_frag_spv.h"
#include "video_core/renderer_d3d12/d3d12_blit_image.h"
#include "video_core/renderer_d3d12/d3d12_descriptor_heap.h"
#include "video_core/renderer_d3d12/d3d12_scheduler.h"
#include "video_core/renderer_d3d12/d3d12_shader_compiler.h"

namespace D3D12 {

namespace {

/// Root parameters (one signature for every helper pipeline): the four push-constant words each
/// helper shader uses, spirv_to_dxil's vertex runtime data, then one SRV and one sampler table.
constexpr u32 PUSH_CONSTANTS_PARAM = 0;
constexpr u32 SRV_TABLE_PARAM = 2;
constexpr u32 SAMPLER_TABLE_PARAM = 3;
constexpr u32 PUSH_CONSTANT_WORDS = 4;
constexpr u32 RUNTIME_DATA_WORDS = 12;

/// Keys of the helper's samplers in SamplerHeap: guest samplers count up from 1 and the
/// presenter's linear sampler is 0, so these never meet them.
constexpr u64 NEAREST_SAMPLER_KEY = ~0ULL;

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
constexpr u64 LINEAR_SAMPLER_KEY = ~0ULL - 1;

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

void Serialize(const Device& device, const D3D12_ROOT_SIGNATURE_DESC& desc,
               ComPtr<ID3D12RootSignature>& out, const char* what) {
    ComPtr<ID3DBlob> serialized;
    ComPtr<ID3DBlob> error;
    if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized,
                                           &error))) {
        throw std::runtime_error(
            error ? std::string(static_cast<const char*>(error->GetBufferPointer()),
                                error->GetBufferSize())
                  : std::string("D3D12SerializeRootSignature failed"));
    }
    ThrowIfFailed(device.Get()->CreateRootSignature(0, serialized->GetBufferPointer(),
                                                    serialized->GetBufferSize(),
                                                    IID_PPV_ARGS(&out)),
                  what);
}

D3D12_DEPTH_STENCILOP_DESC ReplaceStencil() {
    return {
        .StencilFailOp = D3D12_STENCIL_OP_KEEP,
        .StencilDepthFailOp = D3D12_STENCIL_OP_KEEP,
        .StencilPassOp = D3D12_STENCIL_OP_REPLACE,
        .StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS,
    };
}

} // Anonymous namespace

BlitImageHelper::BlitImageHelper(const Device& device_, Scheduler& scheduler_,
                                 const ShaderCompiler& compiler, DescriptorRing& descriptor_ring_,
                                 SamplerHeap& sampler_heap_,
                                 CpuDescriptorAllocator& sampler_descriptors)
    : device{device_}, scheduler{scheduler_}, descriptor_ring{descriptor_ring_},
      sampler_heap{sampler_heap_} {
    if (!compiler.IsAvailable()) {
        LOG_WARNING(Render, "D3D12: no shader path, texture blits and masked clears are skipped");
        return;
    }
    try {
        // Linked like guest pipelines, so the vertex outputs match the pixel inputs.
        const auto compile = [&compiler](std::span<const u32> vs, std::span<const u32> ps) {
            const std::array<ShaderCompiler::PipelineStage, 2> stages{{
                {vs, DXIL_SPIRV_SHADER_VERTEX},
                {ps, DXIL_SPIRV_SHADER_FRAGMENT},
            }};
            auto compiled = compiler.CompilePipeline(stages, {});
            return Program{std::move(compiled[0].dxil), std::move(compiled[1].dxil)};
        };
        programs[static_cast<size_t>(Kind::BlitColor)] =
            compile(FULL_SCREEN_TRIANGLE_VERT_SPV, BLIT_COLOR_FLOAT_FRAG_SPV);
        programs[static_cast<size_t>(Kind::BlitDepth)] =
            compile(FULL_SCREEN_TRIANGLE_VERT_SPV, BLIT_DEPTH_FRAG_SPV);
        programs[static_cast<size_t>(Kind::ClearColor)] =
            compile(VULKAN_COLOR_CLEAR_VERT_SPV, VULKAN_COLOR_CLEAR_FRAG_SPV);
        programs[static_cast<size_t>(Kind::ClearDepthStencil)] =
            compile(VULKAN_COLOR_CLEAR_VERT_SPV, VULKAN_DEPTHSTENCIL_CLEAR_FRAG_SPV);
        CreateRootSignature();
        CreateSamplers(sampler_descriptors);
        available = true;
        LOG_INFO(Render, "D3D12: blit and clear helpers ready (4 programs)");
    } catch (const std::exception& exception) {
        LOG_ERROR(Render, "D3D12: blit helpers unavailable: {}", exception.what());
    }
    try {
        CreatePackPipelines(compiler);
        pack_available = true;
        LOG_INFO(Render, "D3D12: depth-stencil pack shaders ready");
    } catch (const std::exception& exception) {
        LOG_ERROR(Render, "D3D12: depth-stencil pack shaders unavailable, depth-stencil "
                  "contents will not be transferred: {}", exception.what());
    }
    try {
        CreateAstcPipeline(compiler);
        astc_available = true;
        LOG_INFO(Render, "D3D12: ASTC decoder shader ready");
    } catch (const std::exception& exception) {
        LOG_ERROR(Render, "D3D12: ASTC decoder shader unavailable, ASTC is decoded on the CPU: {}",
                  exception.what());
    }
    try {
        CreateBc3Pipeline(compiler);
        bc3_available = true;
        LOG_INFO(Render, "D3D12: BC3 GPU encoder shader ready");
    } catch (const std::exception& exception) {
        LOG_ERROR(Render, "D3D12: BC3 GPU encoder unavailable, recompressed ASTC uses the CPU: {}",
                  exception.what());
    }
}

BlitImageHelper::~BlitImageHelper() = default;

void BlitImageHelper::CreateRootSignature() {
    const D3D12_DESCRIPTOR_RANGE srv_range{
        .RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
        .NumDescriptors = 1,
        .BaseShaderRegister = 0,
        .RegisterSpace = 0,
        .OffsetInDescriptorsFromTableStart = 0,
    };
    const D3D12_DESCRIPTOR_RANGE sampler_range{
        .RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER,
        .NumDescriptors = 1,
        .BaseShaderRegister = 0,
        .RegisterSpace = 0,
        .OffsetInDescriptorsFromTableStart = 0,
    };
    std::array<D3D12_ROOT_PARAMETER, 4> params{};
    params[PUSH_CONSTANTS_PARAM].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[PUSH_CONSTANTS_PARAM].Constants = {.ShaderRegister = 0,
                                              .RegisterSpace = PUSH_CONSTANT_SPACE,
                                              .Num32BitValues = PUSH_CONSTANT_WORDS};
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants = {.ShaderRegister = 0, .RegisterSpace = RUNTIME_DATA_SPACE,
                           .Num32BitValues = RUNTIME_DATA_WORDS};
    params[SRV_TABLE_PARAM].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[SRV_TABLE_PARAM].DescriptorTable = {.NumDescriptorRanges = 1,
                                               .pDescriptorRanges = &srv_range};
    params[SAMPLER_TABLE_PARAM].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[SAMPLER_TABLE_PARAM].DescriptorTable = {.NumDescriptorRanges = 1,
                                                   .pDescriptorRanges = &sampler_range};
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
    Serialize(device, desc, root_signature, "CreateRootSignature (blit helpers)");
}

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
    Serialize(device, desc, pack_root_signature, "CreateRootSignature (depth-stencil pack)");
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
    Serialize(device, desc, astc_root_signature, "CreateRootSignature (ASTC decoder)");
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
    Serialize(device, desc, bc3_root_signature, "CreateRootSignature (BC3 encoder)");
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

void BlitImageHelper::CreateSamplers(CpuDescriptorAllocator& sampler_descriptors) {
    const auto create = [&](D3D12_FILTER filter, u64 key) {
        const D3D12_SAMPLER_DESC desc{
            .Filter = filter,
            .AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
            .AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
            .AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
            .MipLODBias = 0.0f,
            .MaxAnisotropy = 1,
            .ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER,
            .BorderColor = {},
            .MinLOD = 0.0f,
            .MaxLOD = 0.0f,
        };
        const D3D12_CPU_DESCRIPTOR_HANDLE handle = sampler_descriptors.Allocate();
        device.Get()->CreateSampler(&desc, handle);
        return Sampling{handle, key};
    };
    nearest_sampler = create(D3D12_FILTER_MIN_MAG_MIP_POINT, NEAREST_SAMPLER_KEY);
    linear_sampler = create(D3D12_FILTER_MIN_MAG_MIP_LINEAR, LINEAR_SAMPLER_KEY);
}

ID3D12PipelineState* BlitImageHelper::Pipeline(Kind kind, const Target& target, u8 mask,
                                               bool depth) {
    const u64 key = static_cast<u64>(kind) | (static_cast<u64>(target.format) << 8) |
                    (static_cast<u64>(mask) << 40) | (static_cast<u64>(depth) << 48) |
                    (static_cast<u64>(target.samples & 0xff) << 52);
    if (const auto it = pipelines.find(key); it != pipelines.end()) {
        return it->second.Get();
    }
    const Program& program = programs[static_cast<size_t>(kind)];
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = root_signature.Get();
    desc.VS = {program.vs.data(), program.vs.size()};
    desc.PS = {program.ps.data(), program.ps.size()};
    desc.SampleMask = UINT_MAX;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.SampleDesc = {.Count = std::max(1U, target.samples), .Quality = 0};
    switch (kind) {
    case Kind::BlitColor:
    case Kind::ClearColor:
        desc.NumRenderTargets = 1;
        desc.RTVFormats[0] = target.format;
        desc.BlendState.RenderTarget[0].RenderTargetWriteMask =
            kind == Kind::BlitColor ? static_cast<UINT8>(D3D12_COLOR_WRITE_ENABLE_ALL) : mask;
        break;
    case Kind::BlitDepth:
        desc.DSVFormat = target.format;
        desc.DepthStencilState.DepthEnable = TRUE;
        desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        break;
    case Kind::ClearDepthStencil:
        desc.DSVFormat = target.format;
        desc.DepthStencilState = {
            .DepthEnable = depth,
            .DepthWriteMask = depth ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO,
            .DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS,
            .StencilEnable = TRUE,
            .StencilReadMask = 0xFF,
            .StencilWriteMask = mask,
            .FrontFace = ReplaceStencil(),
            .BackFace = ReplaceStencil(),
        };
        break;
    }
    ComPtr<ID3D12PipelineState> pipeline;
    const HRESULT hr = device.Get()->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline));
    if (FAILED(hr)) {
        LOG_ERROR(Render,
                  "D3D12: blit helper pipeline {} for format {} rejected (HRESULT 0x{:08X})",
                  static_cast<u32>(kind), static_cast<u32>(target.format), static_cast<u32>(hr));
    } else {
        LOG_INFO(Render, "D3D12: blit helper pipeline {} built for format {} ({} samples)",
                 static_cast<u32>(kind), static_cast<u32>(target.format), desc.SampleDesc.Count);
    }
    // A rejected pipeline stays cached as null, so it is reported once.
    return pipelines.emplace(key, std::move(pipeline)).first->second.Get();
}

void BlitImageHelper::Begin(ID3D12PipelineState* pipeline, const Target& target, bool is_depth,
                            const D3D12_RECT& rect) {
    ID3D12GraphicsCommandList* const cmd = scheduler.CommandList();
    ID3D12DescriptorHeap* const heaps[] = {descriptor_ring.Heap(), sampler_heap.Heap()};
    cmd->SetDescriptorHeaps(2, heaps);
    cmd->SetGraphicsRootSignature(root_signature.Get());
    scheduler.SetPipelineState(pipeline);
    if (is_depth) {
        cmd->OMSetRenderTargets(0, nullptr, FALSE, &target.view);
    } else {
        cmd->OMSetRenderTargets(1, &target.view, FALSE, nullptr);
    }
    const D3D12_VIEWPORT viewport{
        .TopLeftX = static_cast<f32>(rect.left),
        .TopLeftY = static_cast<f32>(rect.top),
        .Width = static_cast<f32>(rect.right - rect.left),
        .Height = static_cast<f32>(rect.bottom - rect.top),
        .MinDepth = 0.0f,
        .MaxDepth = 1.0f,
    };
    cmd->RSSetViewports(1, &viewport);
    cmd->RSSetScissorRects(1, &rect);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

void BlitImageHelper::Blit(Kind kind, const Target& dst, D3D12_CPU_DESCRIPTOR_HANDLE src_srv,
                           const Sampling& sampler, const VideoCommon::Region2D& dst_region,
                           const VideoCommon::Region2D& src_region,
                           const VideoCommon::Extent2D& src_size) {
    if (!available || src_size.width == 0 || src_size.height == 0) {
        return;
    }
    // As Vulkan's BindBlitState: the destination is the region's bounding box (a mirrored
    // destination is not mirrored); a mirrored source is, through a negative scale.
    const D3D12_RECT rect{
        .left = std::min(dst_region.start.x, dst_region.end.x),
        .top = std::min(dst_region.start.y, dst_region.end.y),
        .right = std::max(dst_region.start.x, dst_region.end.x),
        .bottom = std::max(dst_region.start.y, dst_region.end.y),
    };
    if (rect.right <= rect.left || rect.bottom <= rect.top) {
        return;
    }
    ID3D12PipelineState* const pipeline = Pipeline(kind, dst, 0, false);
    if (!pipeline) {
        return;
    }
    // Descriptors first: a full ring or sampler heap flushes the command list.
    const D3D12_GPU_DESCRIPTOR_HANDLE srv_table = descriptor_ring.Upload({&src_srv, 1});
    const D3D12_GPU_DESCRIPTOR_HANDLE sampler_table =
        sampler_heap.GetTable({&sampler.key, 1}, {&sampler.handle, 1});

    Begin(pipeline, dst, kind == Kind::BlitDepth, rect);
    ID3D12GraphicsCommandList* const cmd = scheduler.CommandList();
    // full_screen_triangle.vert computes texcoord = (x, y) / 2 * scale + offset with Vulkan's
    // clip space, where y = 0 is the top of the viewport; D3D12's y = 0 is the bottom, so the
    // vertical mapping is mirrored here: v' = (1 - v), hence scale -sy and offset oy + sy.
    const f32 width = static_cast<f32>(src_size.width);
    const f32 height = static_cast<f32>(src_size.height);
    const f32 scale_x = static_cast<f32>(src_region.end.x - src_region.start.x) / width;
    const f32 scale_y = static_cast<f32>(src_region.end.y - src_region.start.y) / height;
    const f32 offset_x = static_cast<f32>(src_region.start.x) / width;
    const f32 offset_y = static_cast<f32>(src_region.start.y) / height;
    const std::array<f32, PUSH_CONSTANT_WORDS> push{scale_x, -scale_y, offset_x,
                                                    offset_y + scale_y};
    cmd->SetGraphicsRoot32BitConstants(PUSH_CONSTANTS_PARAM, PUSH_CONSTANT_WORDS, push.data(), 0);
    cmd->SetGraphicsRootDescriptorTable(SRV_TABLE_PARAM, srv_table);
    cmd->SetGraphicsRootDescriptorTable(SAMPLER_TABLE_PARAM, sampler_table);
    cmd->DrawInstanced(3, 1, 0, 0);
}

void BlitImageHelper::BlitColor(const Target& dst, D3D12_CPU_DESCRIPTOR_HANDLE src_srv,
                                const Sampling& sampler, const VideoCommon::Region2D& dst_region,
                                const VideoCommon::Region2D& src_region,
                                const VideoCommon::Extent2D& src_size) {
    Blit(Kind::BlitColor, dst, src_srv, sampler, dst_region, src_region, src_size);
}

void BlitImageHelper::BlitDepth(const Target& dst, D3D12_CPU_DESCRIPTOR_HANDLE src_srv,
                                const VideoCommon::Region2D& dst_region,
                                const VideoCommon::Region2D& src_region,
                                const VideoCommon::Extent2D& src_size) {
    Blit(Kind::BlitDepth, dst, src_srv, nearest_sampler, dst_region, src_region, src_size);
}

void BlitImageHelper::ClearColor(const Target& dst, u8 color_mask, const std::array<f32, 4>& color,
                                 const D3D12_RECT& rect) {
    if (!available || rect.right <= rect.left || rect.bottom <= rect.top) {
        return;
    }
    // D3D12's write mask bits are red 1, green 2, blue 4, alpha 8: the guest's order.
    ID3D12PipelineState* const pipeline =
        Pipeline(Kind::ClearColor, dst, static_cast<u8>(color_mask & 0xF), false);
    if (!pipeline) {
        return;
    }
    Begin(pipeline, dst, false, rect);
    scheduler.CommandList()->SetGraphicsRoot32BitConstants(PUSH_CONSTANTS_PARAM,
                                                           PUSH_CONSTANT_WORDS, color.data(), 0);
    scheduler.CommandList()->DrawInstanced(3, 1, 0, 0);
}

void BlitImageHelper::ClearDepthStencil(const Target& dst, bool clear_depth, f32 depth,
                                        u8 stencil_mask, u8 stencil_value,
                                        const D3D12_RECT& rect) {
    if (!available || rect.right <= rect.left || rect.bottom <= rect.top) {
        return;
    }
    ID3D12PipelineState* const pipeline =
        Pipeline(Kind::ClearDepthStencil, dst, stencil_mask, clear_depth);
    if (!pipeline) {
        return;
    }
    Begin(pipeline, dst, true, rect);
    ID3D12GraphicsCommandList* const cmd = scheduler.CommandList();
    const std::array<f32, PUSH_CONSTANT_WORDS> push{depth, 0.0f, 0.0f, 0.0f};
    cmd->SetGraphicsRoot32BitConstants(PUSH_CONSTANTS_PARAM, PUSH_CONSTANT_WORDS, push.data(), 0);
    cmd->OMSetStencilRef(stencil_value);
    cmd->DrawInstanced(3, 1, 0, 0);
}

} // namespace D3D12
