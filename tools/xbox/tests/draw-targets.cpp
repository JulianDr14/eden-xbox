// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include "common/logging.h"
#include "common/settings.h"
#include "video_core/renderer_d3d12/d3d12_draw_targets.h"
#include "video_core/renderer_d3d12/d3d12_debug_messages.h"
#include "video_core/renderer_d3d12/d3d12_graphics_pipeline.h"
#include "video_core/perf_counters.h"
#include "video_core/renderer_d3d12/d3d12_resource_utils.h"
#include "video_core/renderer_d3d12/d3d12_scheduler.h"
#include <d3d12sdklayers.h>
#include <d3dcompiler.h>
#include <cassert>
#include <cstring>
#include <iostream>
#include <vector>
using namespace D3D12;

static u32 real_format_errors{}, real_overlap_errors{}, limit_reports{};
static bool TapSeen(Common::Log::Class log_class, Common::Log::Level, const char*,
                    unsigned int) noexcept {
    return log_class == Common::Log::Class::Render;
}
static void TapRecord(const Common::Log::TapEntry& entry) noexcept {
    real_format_errors += entry.message.find("regression-real-613") != std::string_view::npos;
    real_overlap_errors += entry.message.find("regression-real-728") != std::string_view::npos;
    limit_reports += entry.message.find("the rest are dropped") != std::string_view::npos;
}

static void CheckDebugMessageClassification() {
    const std::string valid = "ID3D12CommandList::DrawIndexedInstanced: The render target format "
        "in slot 0 does not match that specified by the current pipeline state. "
        "(pipeline state = UNKNOWN, RTV ID3D12Resource* = 0x0000000000000000:'(nullptr)')";
    D3D12_MESSAGE message{D3D12_MESSAGE_CATEGORY_EXECUTION, D3D12_MESSAGE_SEVERITY_ERROR,
        D3D12_MESSAGE_ID_RENDER_TARGET_FORMAT_MISMATCH_PIPELINE_STATE,
        valid.c_str(), valid.size() + 1};
    assert(IsNullRtvFormatFalsePositive(message));
    for (const auto severity : {D3D12_MESSAGE_SEVERITY_CORRUPTION,
                               D3D12_MESSAGE_SEVERITY_WARNING}) {
        auto changed = message;
        changed.Severity = severity;
        assert(!IsNullRtvFormatFalsePositive(changed));
    }
    auto changed = message;
    changed.ID = D3D12_MESSAGE_ID_SET_RENDER_TARGETS_INVALID;
    assert(!IsNullRtvFormatFalsePositive(changed));
    changed = message;
    changed.pDescription = nullptr;
    assert(!IsNullRtvFormatFalsePositive(changed));
    for (const auto replacement : {std::pair{"UNKNOWN", "R8G8B8A8_UNORM"},
                                  std::pair{"0000000000000000", "0000000000000001"},
                                  std::pair{"(nullptr)", "real resource"},
                                  std::pair{"ID3D12CommandList::Draw", "OtherOperation"}}) {
        auto text = valid;
        text.replace(text.find(replacement.first), std::strlen(replacement.first), replacement.second);
        changed = message;
        changed.pDescription = text.c_str();
        changed.DescriptionByteLength = text.size() + 1;
        assert(!IsNullRtvFormatFalsePositive(changed));
    }
    changed = message;
    changed.DescriptionByteLength = valid.find("(pipeline state");
    assert(!IsNullRtvFormatFalsePositive(changed));
}

static ComPtr<ID3DBlob> Compile(const std::string& source, const char* profile) {
    ComPtr<ID3DBlob> code, error;
    const auto hr = D3DCompile(source.data(), source.size(), nullptr, nullptr, nullptr,
                              "main", profile, 0, 0, &code, &error);
    if (FAILED(hr) && error) std::cerr << static_cast<const char*>(error->GetBufferPointer());
    ThrowIfFailed(hr, "compile test shader");
    return code;
}

int main() try {
    CheckDebugMessageClassification();
    // All masks and counts, including sparse slots and all-disabled/depth-only bindings.
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 8> input{};
    for (size_t i = 0; i < input.size(); ++i) input[i].ptr = i + 1;
    for (u32 mask = 0; mask < 256; ++mask) {
        std::array<Vulkan::FixedPipelineState::BlendingAttachment, 8> state{};
        for (u32 i = 0; i < 8; ++i) state[i].mask_r.Assign(mask >> i & 1);
        assert(WritableColorTargetMask(state) == mask);
        for (size_t count = 0; count <= 8; ++count) {
            const auto targets = DrawColorTargets(std::span{input.data(), count}, u8(mask), {99});
            for (size_t i = 0; i < count; ++i)
                assert(targets[i].ptr == ((mask >> i & 1) ? i + 1 : 99));
        }
    }
    // Every component individually keeps a target active, not just R.
    std::array<Vulkan::FixedPipelineState::BlendingAttachment, 8> channels{};
    channels[0].mask_r.Assign(1); channels[1].mask_g.Assign(1);
    channels[2].mask_b.Assign(1); channels[3].mask_a.Assign(1);
    assert(WritableColorTargetMask(channels) == 15);
    Common::Log::Initialize();
    Settings::values.renderer_debug = true;
    Device device;
    ComPtr<ID3D12InfoQueue> messages;
    ThrowIfFailed(device.Get()->QueryInterface(IID_PPV_ARGS(&messages)), "debug queue");
    messages->ClearStoredMessages();
    Scheduler scheduler{device};
    StagingBufferPool staging{device, scheduler};
    TransferBufferPool transfers{device, scheduler};
    CpuDescriptorAllocator views{device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV};
    CpuDescriptorAllocator samplers{device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER};
    CpuDescriptorAllocator rtvs{device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV};
    CpuDescriptorAllocator dsvs{device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV};
    TextureCacheRuntime runtime{device, scheduler, staging, transfers, views, samplers, rtvs, dsvs};
    const auto vs = Compile(R"(
float4 main(uint id:SV_VertexID):SV_Position {
    float2 p[3]={float2(-1,-1),float2(-1,3),float2(3,-1)};
    return float4(p[id],0,1);
})", "vs_5_1");
    {
        // Exercise the production GraphicsPipeline/Build/Wake/Evict path, including its
        // memory ledger and scheduler retire callback (not just the standalone index).
        const auto ps = Compile("float4 main():SV_Target{return float4(1,0,0,1);}", "ps_5_1");
        auto bytecodes = std::make_shared<GraphicsPipeline::DxilStages>();
        const auto copy_blob = [](const auto& blob) {
            const auto* begin = static_cast<const u8*>(blob->GetBufferPointer());
            return std::vector<u8>(begin, begin + blob->GetBufferSize());
        };
        (*bytecodes)[0] = copy_blob(vs);
        (*bytecodes)[4] = copy_blob(ps);
        Shader::Info vertex{}, fragment{};
        std::array<const Shader::Info*, GraphicsPipeline::NUM_STAGES> infos{};
        infos[0] = &vertex; infos[4] = &fragment;
        RootSignatureCache roots{device};
        const auto& layout = roots.Get(infos, false);
        GraphicsPsoCache psos;
        Common::ThreadWorker builders{4, "PsoTest"};
        GraphicsPipelineCacheKey key{};
        key.state.topology.Assign(Maxwell::PrimitiveTopology::Triangles);
        key.state.polygon_mode.Assign(Vulkan::FixedPipelineState::PackPolygonMode(Maxwell::PolygonMode::Fill));
        key.state.color_formats[0] = static_cast<u8>(Tegra::RenderTargetFormat::A8B8G8R8_UNORM);
        key.state.attachments[0].raw = 15;
        const auto resident_before = Common::ReadMemoryAccount(Common::MemoryAccount::PipelineStates).objects;
        const auto creates_before = VideoCore::Perf::Get(VideoCore::Perf::Read(), VideoCore::Perf::Counter::PsoCreates);
        {
            // Baseline: the former per-guest construction path, with the shared index absent.
            std::vector<std::unique_ptr<GraphicsPipeline>> independent;
            for (u64 i = 0; i < 16; ++i) {
                key.state.vertex_strides[0] = static_cast<u16>(i * 16);
                independent.push_back(std::make_unique<GraphicsPipeline>(device, runtime, nullptr,
                    &builders, key, [bytecodes](u64) { return bytecodes; }, infos, layout));
            }
            builders.WaitForRequests();
            assert(Common::ReadMemoryAccount(Common::MemoryAccount::PipelineStates).objects == resident_before + 16);
            assert(VideoCore::Perf::Get(VideoCore::Perf::Read(), VideoCore::Perf::Counter::PsoCreates) == creates_before + 16);
        }
        assert(Common::ReadMemoryAccount(Common::MemoryAccount::PipelineStates).objects == resident_before);
        std::vector<std::unique_ptr<GraphicsPipeline>> pipelines;
        for (u64 i = 0; i < 16; ++i) {
            // Different guest keys, identical consumed host state: unused vertex strides
            // must not cause repeated driver compilations.
            key.state.vertex_strides[0] = static_cast<u16>(i * 16);
            pipelines.push_back(std::make_unique<GraphicsPipeline>(device, runtime, nullptr,
                &builders, key, [bytecodes](u64) { return bytecodes; }, infos, layout, true, &psos));
        }
        builders.WaitForRequests();
        assert(pipelines[0]->Handle());
        for (const auto& pipeline : pipelines) {
            assert(pipeline->IsBuilt() && pipeline->Handle() == pipelines[0]->Handle());
        }
        assert(Common::ReadMemoryAccount(Common::MemoryAccount::PipelineStates).objects == resident_before + 1);
        assert(VideoCore::Perf::Get(VideoCore::Perf::Read(), VideoCore::Perf::Counter::PsoCreates) == creates_before + 17);
        for (const auto& pipeline : pipelines) { assert(pipeline->Evict(scheduler)); }
        // Entries still belong to the deferred fence callbacks, not the weak index.
        assert(Common::ReadMemoryAccount(Common::MemoryAccount::PipelineStates).objects == resident_before + 1);
        scheduler.Finish(); scheduler.CollectGarbage();
        assert(Common::ReadMemoryAccount(Common::MemoryAccount::PipelineStates).objects == resident_before);
        pipelines[0]->Wake(builders); pipelines[0]->WaitBuilt();
        pipelines[1]->Wake(builders); pipelines[1]->WaitBuilt();
        assert(pipelines[0]->Handle() && pipelines[0]->Handle() == pipelines[1]->Handle());
        assert(VideoCore::Perf::Get(VideoCore::Perf::Read(), VideoCore::Perf::Counter::PsoCreates) == creates_before + 18);
        builders.WaitForRequests();
        std::cout << "PASS: production PSO sharing across 16 guest keys: driver calls/resident PSOs 16 -> 1; worker publication, unique ledger, fence retirement and Wake reuse\n";
    }
    const D3D12_ROOT_SIGNATURE_DESC root_desc{0, nullptr, 0, nullptr,
                                              D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
    auto root = CreateRootSignature(device.Get(), root_desc, "test root");
    const D3D12_HEAP_PROPERTIES heap{.Type = D3D12_HEAP_TYPE_DEFAULT};
    u32 draws{};
    for (const auto format : {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM,
                             DXGI_FORMAT_R16G16B16A16_FLOAT}) {
        for (u32 samples : {1U, 4U}) {
            D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS quality{format, samples};
            ThrowIfFailed(device.Get()->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS,
                         &quality, sizeof(quality)), "MSAA quality");
            if (!quality.NumQualityLevels) throw std::runtime_error("required MSAA test unsupported");
            for (u32 variant = 0; variant < (samples > 1 ? 7U : 5U); ++variant) {
                const u32 a = variant >= 5 ? 0 : variant == 2 ? 7 : 3;
                const u32 b = variant >= 5 ? 3 : variant == 3 ? 7 : 4;
                const bool write_a = variant != 1 && variant != 4 && variant < 5;
                const bool write_b = variant == 1 || variant >= 5;
                const u8 mask = u8((write_a ? 1U << a : 0) | (write_b ? 1U << b : 0));
                const u8 bound_mask = variant >= 5 ? u8(mask | 1U) : mask ? mask : u8(1U << a);
                const u32 count = u32(DrawColorTargetCount(std::max(a, b) + 1, bound_mask));
                const auto ps = Compile("struct Out {float4 a:SV_Target" + std::to_string(a) +
                    ";float4 b:SV_Target" + std::to_string(b) +
                    ";};Out main(){Out o;o.a=float4(1,0,0," + std::string(variant == 6 ? "0" : "1") +
                    ");o.b=float4(0,1,0,1);return o;}", "ps_5_1");
                D3D12_RESOURCE_DESC desc{.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
                    .Width = 4, .Height = 4, .DepthOrArraySize = 1, .MipLevels = 1,
                    .Format = format, .SampleDesc = {samples, 0},
                    .Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
                ComPtr<ID3D12Resource> target;
                ThrowIfFailed(device.Get()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
                    &desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target)), "target");
                const auto rtv = rtvs.Allocate();
                device.Get()->CreateRenderTargetView(target.Get(), nullptr, rtv);
                D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc{};
                pso_desc.pRootSignature = root.Get();
                pso_desc.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
                pso_desc.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
                pso_desc.BlendState.IndependentBlendEnable = TRUE;
                pso_desc.BlendState.AlphaToCoverageEnable = variant >= 5;
                pso_desc.NumRenderTargets = count;
                for (u32 i = 0; i < count; ++i) {
                    pso_desc.RTVFormats[i] = DrawColorTargetFormat(format, bound_mask, i);
                    pso_desc.BlendState.RenderTarget[i] = {FALSE, FALSE,
                        D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD,
                        D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD,
                        D3D12_LOGIC_OP_NOOP, UINT8((mask >> i & 1) ? 15 : 0)};
                }
                pso_desc.SampleMask = UINT_MAX;
                pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
                pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
                pso_desc.RasterizerState.DepthClipEnable = TRUE;
                pso_desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
                pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
                pso_desc.SampleDesc = {samples, 0};
                ComPtr<ID3D12PipelineState> pso;
                const auto result = device.Get()->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&pso));
                if (FAILED(result)) {
                    for (u64 n = 0; n < messages->GetNumStoredMessagesAllowedByRetrievalFilter(); ++n) {
                        SIZE_T size{}; messages->GetMessage(n, nullptr, &size);
                        std::vector<u8> storage(size);
                        auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
                        messages->GetMessage(n, message, &size);
                        std::cerr << message->pDescription << std::endl;
                    }
                }
                ThrowIfFailed(result, "pso");
                std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 8> original;
                original.fill(runtime.NullRenderTarget(samples > 1));
                original[a] = original[b] = rtv;
                ComPtr<ID3D12Resource> alpha_target;
                D3D12_CPU_DESCRIPTOR_HANDLE alpha_rtv{};
                if (variant >= 5) {
                    ThrowIfFailed(device.Get()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
                        &desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                        IID_PPV_ARGS(&alpha_target)), "alpha target");
                    alpha_rtv = rtvs.Allocate();
                    device.Get()->CreateRenderTargetView(alpha_target.Get(), nullptr, alpha_rtv);
                    original[0] = alpha_rtv;
                }
                auto filtered = DrawColorTargets(std::span{original.data(), count}, bound_mask,
                                                  runtime.NullRenderTarget(samples > 1));
                auto* cmd = scheduler.CommandList();
                const float black[4]{};
                cmd->ClearRenderTargetView(rtv, black, 0, nullptr);
                cmd->SetGraphicsRootSignature(root.Get());
                scheduler.SetPipelineState(pso.Get());
                cmd->OMSetRenderTargets(count, filtered.data(), FALSE, nullptr);
                const D3D12_VIEWPORT vp{0, 0, 4, 4, 0, 1};
                const D3D12_RECT scissor{0, 0, 4, 4};
                cmd->RSSetViewports(1, &vp); cmd->RSSetScissorRects(1, &scissor);
                cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                cmd->DrawInstanced(3, 1, 0, 0);
                ComPtr<ID3D12Resource> resolved;
                ID3D12Resource* copied = target.Get();
                if (samples > 1) {
                    auto single = desc;
                    single.SampleDesc.Count = 1;
                    ThrowIfFailed(device.Get()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
                        &single, D3D12_RESOURCE_STATE_RESOLVE_DEST, nullptr,
                        IID_PPV_ARGS(&resolved)), "resolve target");
                    TransitionResource(cmd, target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                                       D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
                    cmd->Record([dst = resolved.Get(), src = target.Get(), format](auto* list) {
                        list->ResolveSubresource(dst, 0, src, 0, format);
                    });
                    copied = resolved.Get();
                    TransitionResource(cmd, copied, D3D12_RESOURCE_STATE_RESOLVE_DEST,
                                       D3D12_RESOURCE_STATE_COPY_SOURCE);
                } else {
                    TransitionResource(cmd, copied, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                       D3D12_RESOURCE_STATE_COPY_SOURCE);
                }
                auto copied_desc = copied->GetDesc();
                D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
                u64 bytes{};
                device.Get()->GetCopyableFootprints(&copied_desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
                auto readback = CreateCommittedBuffer(device.Get(), bytes, D3D12_HEAP_TYPE_READBACK,
                    D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE, "readback");
                D3D12_TEXTURE_COPY_LOCATION dst{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
                dst.PlacedFootprint = footprint;
                const D3D12_TEXTURE_COPY_LOCATION src{copied, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
                cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                scheduler.Finish();
                void* data{};
                const D3D12_RANGE read{0, SIZE_T(bytes)};
                ThrowIfFailed(readback->Map(0, &read, &data), "map");
                const u64 expected = !mask || variant == 6 ? 0 : format == DXGI_FORMAT_R8G8B8A8_UNORM ?
                    (write_a ? 0xff0000ffULL : 0xff00ff00ULL) :
                    format == DXGI_FORMAT_R10G10B10A2_UNORM ?
                    (write_a ? 0xc00003ffULL : 0xc00ffc00ULL) :
                    (write_a ? 0x3c00000000003c00ULL : 0x3c0000003c000000ULL);
                const u32 bpp = format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8 : 4;
                for (u32 y = 0; y < 4; ++y) for (u32 x = 0; x < 4; ++x) {
                    u64 pixel{};
                    std::memcpy(&pixel, static_cast<u8*>(data) + footprint.Offset +
                                y * footprint.Footprint.RowPitch + x * bpp, bpp);
                    assert(pixel == expected);
                }
                const D3D12_RANGE written{}; readback->Unmap(0, &written);
                rtvs.Free(rtv);
                if (alpha_rtv.ptr) rtvs.Free(alpha_rtv);
                ++draws;
            }
        }
    }
    ThrowIfFailed(device.Get()->GetDeviceRemovedReason(), "device removed");
    u32 layer_false_positives{};
    for (u64 i = 0; i < messages->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
        SIZE_T bytes{}; messages->GetMessage(i, nullptr, &bytes);
        std::vector<u8> storage(bytes);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        messages->GetMessage(i, message, &bytes);
        if (message->Severity <= D3D12_MESSAGE_SEVERITY_WARNING) {

            if (IsNullRtvFormatFalsePositive(*message)) {
                ++layer_false_positives;
                continue;
            }
            std::cerr << message->pDescription << '\n'; return 1;
        }
    }
    // Exercise the production logger: a false-positive flood must not hide later real errors,
    // including another error with the SAME ID 613.
    Common::Log::SetTap(TapSeen, TapRecord);
    const char* known = "ID3D12CommandList::DrawIndexedInstanced: The render target format "
        "in slot 0 does not match that specified by the current pipeline state. "
        "(pipeline state = UNKNOWN, RTV ID3D12Resource* = 0x0000000000000000:'(nullptr)')";
    ThrowIfFailed(messages->SetMessageCountLimit(2048), "queue capacity");
    for (u32 i = 0; i < 600; ++i) {
        ThrowIfFailed(messages->AddMessage(D3D12_MESSAGE_CATEGORY_EXECUTION,
            D3D12_MESSAGE_SEVERITY_ERROR,
            D3D12_MESSAGE_ID_RENDER_TARGET_FORMAT_MISMATCH_PIPELINE_STATE, known), "inject known");
    }
    ThrowIfFailed(messages->AddMessage(D3D12_MESSAGE_CATEGORY_EXECUTION,
        D3D12_MESSAGE_SEVERITY_ERROR, D3D12_MESSAGE_ID_RENDER_TARGET_FORMAT_MISMATCH_PIPELINE_STATE,
        "regression-real-613: non-null resource format mismatch"), "inject real 613");
    ThrowIfFailed(messages->AddMessage(D3D12_MESSAGE_CATEGORY_EXECUTION,
        D3D12_MESSAGE_SEVERITY_ERROR, D3D12_MESSAGE_ID_SET_RENDER_TARGETS_INVALID,
        "regression-real-728: overlapping render targets"), "inject real 728");
    device.LogDebugMessages();
    Common::Log::SetTap(nullptr, nullptr);
    std::cout << "Logger probe: real613=" << real_format_errors << " real728=" << real_overlap_errors
              << " limit=" << limit_reports << std::endl;
    assert(real_format_errors == 1 && real_overlap_errors == 1 && !limit_reports);
    assert(messages->GetNumStoredMessages() == 0);
    std::cout << "PASS: strict debug classification and production logger flood of 600 known "
                 "messages followed by real 613/728 errors; real errors retained\n";
    std::cout << "Known Windows null/UNKNOWN debug-layer false positives: " << layer_false_positives << "\n";
    std::cout << "PASS: 2304 mask/count cases, RGBA components, " << draws <<
        " GPU draws/readbacks, slots 3/4 and 7, disabled/re-enabled/reversed, "
        "RGBA8/RGB10A2/RGBA16F, MSAA 1/4, alpha-to-coverage with masked Target0, matching null slots; D3D12 debug: known null/UNKNOWN layer bug excepted explicitly\n";
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
