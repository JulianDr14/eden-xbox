// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <exception>
#include <span>
#include <stdexcept>
#include <utility>

#include "common/logging.h"
#include "video_core/host_shaders/vulkan_fidelityfx_fsr_easu_fp32_frag_spv.h"
#include "video_core/host_shaders/vulkan_fidelityfx_fsr_rcas_fp32_frag_spv.h"
#include "video_core/host_shaders/vulkan_fidelityfx_fsr_vert_spv.h"
#include "video_core/renderer_d3d12/d3d12_command_stream.h"
#include "video_core/renderer_d3d12/d3d12_descriptor_heap.h"
#include "video_core/renderer_d3d12/d3d12_present_fsr.h"
#include "video_core/renderer_d3d12/d3d12_resource_utils.h"
#include "video_core/renderer_d3d12/d3d12_scheduler.h"
#include "video_core/renderer_d3d12/d3d12_shader_compiler.h"
#include "video_core/renderer_d3d12/d3d12_swapchain.h"

namespace D3D12 {
namespace {

// Root parameters, shared by both passes so the tables bound for EASU stay bound for RCAS.
enum RootParameter : UINT {
    CONSTANTS,    ///< the shaders' push constants: Const0..Const3 (b0, PUSH_CONSTANT_SPACE)
    SOURCE,       ///< the image the pass reads (t0)
    SAMPLER,      ///< s0: EASU gathers through it; RCAS loads texels and ignores it
    RUNTIME_DATA, ///< only when the translation asks for it (b0, RUNTIME_DATA_SPACE)
};
constexpr UINT CONSTANT_COUNT = 16;
/// Runtime data spirv_to_dxil reads in a vertex shader (d3d12_present_blit.cpp declares the same).
constexpr UINT RUNTIME_DATA_COUNT = 12;

ComPtr<ID3D12PipelineState> CreatePipeline(ID3D12Device* device, ID3D12RootSignature* signature,
                                           std::span<const u8> vs, std::span<const u8> ps,
                                           DXGI_FORMAT format, const char* what) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = signature;
    desc.VS = {vs.data(), vs.size()};
    desc.PS = {ps.data(), ps.size()};
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = UINT_MAX;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = format;
    desc.SampleDesc.Count = 1;
    ComPtr<ID3D12PipelineState> pipeline;
    ThrowIfFailed(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline)), what);
    return pipeline;
}

} // Anonymous namespace

PresentFsr::PresentFsr(const Device& device_, const ShaderCompiler& shader_compiler_,
                       Scheduler& scheduler_, CpuDescriptorAllocator& view_descriptors_,
                       CpuDescriptorAllocator& rtv_descriptors_)
    : device{device_}, shader_compiler{shader_compiler_}, scheduler{scheduler_},
      view_descriptors{view_descriptors_}, rtv_descriptors{rtv_descriptors_} {}

PresentFsr::~PresentFsr() {
    // The renderer has drained the GPU before its members go away.
    if (target_rtv.ptr != 0) {
        rtv_descriptors.Free(target_rtv);
    }
    if (target_srv.ptr != 0) {
        view_descriptors.Free(target_srv);
    }
}

bool PresentFsr::EnsurePipelines() {
    if (state == State::NotBuilt) {
        try {
            CreatePipelines();
            state = State::Ready;
        } catch (const std::exception& e) {
            LOG_ERROR(Render, "D3D12: FSR 1 unavailable, presenting with the bilinear blit: {}",
                      e.what());
            state = State::Failed;
        }
    }
    return state == State::Ready;
}

void PresentFsr::CreatePipelines() {
    if (!shader_compiler.IsAvailable()) {
        throw std::runtime_error("no shader path");
    }
    ID3D12Device* const dev = device.Get();

    // Vulkan's clip space has y down and D3D12's y up: the vertex shader's texcoord.y must be 0
    // at the top for RCAS, which finds its pixel from it. EASU uses SV_Position, unaffected.
    ShaderCompiler::PipelineOptions options{};
    options.yz_flip = DXIL_SPIRV_Y_FLIP_UNCONDITIONAL;
    options.y_flip_mask = 1;
    const std::array<ShaderCompiler::PipelineStage, 2> easu_stages{{
        {VULKAN_FIDELITYFX_FSR_VERT_SPV, DXIL_SPIRV_SHADER_VERTEX},
        {VULKAN_FIDELITYFX_FSR_EASU_FP32_FRAG_SPV, DXIL_SPIRV_SHADER_FRAGMENT},
    }};
    const std::array<ShaderCompiler::PipelineStage, 2> rcas_stages{{
        {VULKAN_FIDELITYFX_FSR_VERT_SPV, DXIL_SPIRV_SHADER_VERTEX},
        {VULKAN_FIDELITYFX_FSR_RCAS_FP32_FRAG_SPV, DXIL_SPIRV_SHADER_FRAGMENT},
    }};
    const auto easu = shader_compiler.CompilePipeline(easu_stages, options);
    const auto rcas = shader_compiler.CompilePipeline(rcas_stages, options);
    bool runtime_data = false;
    for (const auto* stages : {&easu, &rcas}) {
        for (const ShaderCompiler::CompiledStage& stage : *stages) {
            runtime_data |= stage.metadata.requires_runtime_data;
        }
    }

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
    params[CONSTANTS].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[CONSTANTS].Constants = {.ShaderRegister = 0, .RegisterSpace = PUSH_CONSTANT_SPACE,
                                   .Num32BitValues = CONSTANT_COUNT};
    params[CONSTANTS].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[SOURCE].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[SOURCE].DescriptorTable = {.NumDescriptorRanges = 1, .pDescriptorRanges = &srv_range};
    params[SOURCE].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[SAMPLER].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[SAMPLER].DescriptorTable = {.NumDescriptorRanges = 1,
                                       .pDescriptorRanges = &sampler_range};
    params[SAMPLER].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[RUNTIME_DATA].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[RUNTIME_DATA].Constants = {.ShaderRegister = 0, .RegisterSpace = RUNTIME_DATA_SPACE,
                                      .Num32BitValues = RUNTIME_DATA_COUNT};
    params[RUNTIME_DATA].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    // The stages these passes do not have get no root arguments (Microsoft's recommendation).
    const D3D12_ROOT_SIGNATURE_DESC root_desc{
        .NumParameters = runtime_data ? 4u : 3u,
        .pParameters = params.data(),
        .NumStaticSamplers = 0,
        .pStaticSamplers = nullptr,
        .Flags = D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
                 D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
                 D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS,
    };
    root_signature = CreateRootSignature(dev, root_desc, "FSR root signature");
    needs_runtime_data = runtime_data;

    easu_pipeline = CreatePipeline(dev, root_signature.Get(), easu[0].dxil, easu[1].dxil,
                                   TARGET_FORMAT, "CreateGraphicsPipelineState (FSR EASU)");
    rcas_pipeline = CreatePipeline(dev, root_signature.Get(), rcas[0].dxil, rcas[1].dxil,
                                   Swapchain::FORMAT, "CreateGraphicsPipelineState (FSR RCAS)");
    LOG_INFO(Render,
             "D3D12: FSR 1 ready (EASU + RCAS, 32-bit, from translated SPIR-V; EASU PS {} bytes, "
             "RCAS PS {} bytes{})",
             easu[1].dxil.size(), rcas[1].dxil.size(), runtime_data ? ", runtime data" : "");
}

bool PresentFsr::PrepareTarget(const Fsr::Plan& plan) {
    if (state != State::Ready) {
        return false;
    }
    if (target && plan.output_width == target_width && plan.output_height == target_height) {
        return true;
    }
    try {
        CreateTarget(plan.output_width, plan.output_height);
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR(Render, "D3D12: FSR 1 off, presenting with the bilinear blit: {}", e.what());
        state = State::Failed;
        return false;
    }
}

void PresentFsr::ReleaseTarget() {
    if (!target) {
        return;
    }
    // The descriptors stay allocated: PrepareTarget rewrites them for the next target.
    scheduler.DeferRelease(std::move(target));
    target_width = 0;
    target_height = 0;
    LOG_INFO(Render, "D3D12: FSR target released");
}

void PresentFsr::CreateTarget(u32 width, u32 height) {
    ID3D12Device* const dev = device.Get();
    // Frames in flight may still read the old target: the scheduler keeps it alive until then.
    scheduler.DeferRelease(std::move(target));
    target_width = 0;
    target_height = 0;

    const D3D12_HEAP_PROPERTIES heap{.Type = D3D12_HEAP_TYPE_DEFAULT};
    const D3D12_RESOURCE_DESC desc{
        .Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
        .Alignment = 0,
        .Width = width,
        .Height = height,
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .Format = TARGET_FORMAT,
        .SampleDesc = {.Count = 1, .Quality = 0},
        .Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
        .Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
    };
    ThrowIfFailed(dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                               nullptr, IID_PPV_ARGS(&target)),
                  "CreateCommittedResource (FSR target)");
    target->SetName(L"FSR EASU target");

    // The SRV is copied into the ring when a frame's tables are allocated, so it can be rewritten
    // in place. The RTV is read when the list call is recorded, possibly later on the recording
    // worker: a new one, while the allocator holds the old one back until that has happened.
    if (target_srv.ptr == 0) {
        target_srv = view_descriptors.Allocate();
    }
    dev->CreateShaderResourceView(target.Get(), nullptr, target_srv);
    if (target_rtv.ptr != 0) {
        rtv_descriptors.Free(target_rtv);
    }
    target_rtv = rtv_descriptors.Allocate();
    dev->CreateRenderTargetView(target.Get(), nullptr, target_rtv);

    target_width = width;
    target_height = height;
    LOG_INFO(Render, "D3D12: FSR target {}x{} ({} KiB)", target_width, target_height,
             static_cast<u64>(target_width) * target_height * 4 / 1024);
}

void PresentFsr::RecordEasu(CommandStream* cmd, const Fsr::Plan& plan,
                            D3D12_GPU_DESCRIPTOR_HANDLE source_table,
                            D3D12_GPU_DESCRIPTOR_HANDLE sampler_table) {
    TransitionResource(cmd, target.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_RENDER_TARGET);
    // EASU writes every texel: nothing of the previous frame needs to be kept.
    cmd->DiscardResource(target.Get());
    cmd->OMSetRenderTargets(1, &target_rtv, FALSE, nullptr);
    const D3D12_VIEWPORT viewport{
        .TopLeftX = 0.0f,
        .TopLeftY = 0.0f,
        .Width = static_cast<float>(plan.output_width),
        .Height = static_cast<float>(plan.output_height),
        .MinDepth = 0.0f,
        .MaxDepth = 1.0f,
    };
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(plan.output_width),
                             static_cast<LONG>(plan.output_height)};
    cmd->RSSetViewports(1, &viewport);
    cmd->RSSetScissorRects(1, &scissor);

    cmd->SetGraphicsRootSignature(root_signature.Get());
    if (needs_runtime_data) {
        // The translated full-screen triangle reads none of it; zeros are a valid draw.
        constexpr std::array<u32, RUNTIME_DATA_COUNT> runtime_data{};
        cmd->SetGraphicsRoot32BitConstants(RUNTIME_DATA, RUNTIME_DATA_COUNT, runtime_data.data(),
                                           0);
    }
    cmd->SetGraphicsRootDescriptorTable(SAMPLER, sampler_table);
    cmd->SetGraphicsRootDescriptorTable(SOURCE, source_table);
    cmd->SetGraphicsRoot32BitConstants(CONSTANTS, CONSTANT_COUNT, plan.easu.data(), 0);
    scheduler.SetPipelineState(easu_pipeline.Get());
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->DrawInstanced(3, 1, 0, 0);

    TransitionResource(cmd, target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

void PresentFsr::RecordRcas(CommandStream* cmd, const Fsr::Plan& plan,
                            D3D12_GPU_DESCRIPTOR_HANDLE target_table) {
    // Same root signature as EASU: the sampler and runtime data stay bound.
    cmd->SetGraphicsRootDescriptorTable(SOURCE, target_table);
    cmd->SetGraphicsRoot32BitConstants(CONSTANTS, static_cast<UINT>(plan.rcas.size()),
                                       plan.rcas.data(), 0);
    scheduler.SetPipelineState(rcas_pipeline.Get());
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->DrawInstanced(3, 1, 0, 0);
}

} // namespace D3D12
