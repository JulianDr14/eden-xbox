// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include "common/logging.h"
#include "common/settings.h"
#include "video_core/renderer_d3d12/d3d12_scheduler.h"
#include "video_core/renderer_d3d12/d3d12_texture_cache.h"
#include "video_core/renderer_d3d12/d3d12_resource_utils.h"
#include "video_core/renderer_d3d12/d3d12_srv_compatibility.h"
#include "video_core/renderer_d3d12/d3d12_stage_bindings.h"
#include "video_core/texture_cache/image_view_info.h"
#include <d3d12sdklayers.h>
#include <d3dcompiler.h>
#include <cassert>
#include <iostream>
#include <set>
#include <vector>
using namespace D3D12;
using namespace VideoCommon;
using VideoCore::Surface::PixelFormat;

int main() try {
    // Diagnostic metadata must not merge shader interfaces with different interpolation.
    // Exercise the top generic as well: shifts past 31 bits must remain 64-bit.
    for (u32 generic = 0; generic < 32; ++generic) {
        Shader::Info info{};
        info.loads.Set(Shader::IR::Attribute::Generic0X + generic * 4);
        info.interpolation[generic] = Shader::Interpolation::Smooth;
        const auto smooth = InternStage(info);
        assert(InternStage(info) == smooth);
        for (auto mode : {Shader::Interpolation::Flat, Shader::Interpolation::NoPerspective}) {
            info.interpolation[generic] = mode;
            const auto changed = InternStage(info);
            assert(changed != smooth);
            assert(((changed->interpolation >> (2 * generic)) & 3) == static_cast<u32>(mode));
            assert(InternStage(info) == changed);
        }
        // Unread interpolation does not inflate the interner.
        info.interpolation[generic] = Shader::Interpolation::Smooth;
        info.interpolation[(generic + 1) % 32] = Shader::Interpolation::Flat;
        assert(InternStage(info) == smooth);
    }
    D3D12_RESOURCE_DESC compatibility{};
    compatibility.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    compatibility.Width = compatibility.Height = 8;
    compatibility.DepthOrArraySize = 12;
    compatibility.SampleDesc.Count = 1;
    using Shader::TextureType;
    assert(IsSrvDimensionCompatible(TextureType::ColorCube, compatibility, 6, 6));
    assert(!IsSrvDimensionCompatible(TextureType::ColorCube, compatibility, 7, 6));
    assert(!IsSrvDimensionCompatible(TextureType::ColorCube, compatibility, UINT32_MAX, 6));
    assert(!IsSrvDimensionCompatible(TextureType::ColorCube, compatibility, 0, UINT32_MAX));
    assert(!IsSrvDimensionCompatible(TextureType::ColorCube, compatibility, 0, 5));
    assert(!IsSrvDimensionCompatible(TextureType::ColorArrayCube, compatibility, 0, 7));
    assert(IsSrvDimensionCompatible(TextureType::ColorArrayCube, compatibility, 0, 12));
    compatibility.SampleDesc.Count = 4;
    assert(!IsSrvDimensionCompatible(TextureType::ColorCube, compatibility, 0, 6));
    assert(!IsSrvDimensionCompatible(TextureType::ColorArrayCube, compatibility, 0, 12));
    assert(IsSrvDimensionCompatible(TextureType::Color2D, compatibility, 0, 1));
    assert(IsSrvDimensionCompatible(TextureType::ColorArray2D, compatibility, 0, 12));
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
    // Compile a real cube sample, including LOD relative to the view and all six directions.
    const char shader[] = R"(
TextureCube<float4> tex : register(t0);
SamplerState samp : register(s0);
RWByteAddressBuffer result : register(u0);
[numthreads(6,1,1)] void main(uint3 id : SV_DispatchThreadID) {
    const float3 dirs[6] = {float3(1,0,0),float3(-1,0,0),float3(0,1,0),
                           float3(0,-1,0),float3(0,0,1),float3(0,0,-1)};
    result.Store(id.x*4, (uint)round(tex.SampleLevel(samp, dirs[id.x], 0).r * 255));
})";
    ComPtr<ID3DBlob> code, error;
    ThrowIfFailed(D3DCompile(shader, sizeof(shader), nullptr, nullptr, nullptr, "main", "cs_5_1",
                            0, 0, &code, &error), "compile cube test");
    D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable = {1, &range};
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    D3D12_ROOT_SIGNATURE_DESC root_desc{2, params, 1, &sampler};
    ComPtr<ID3DBlob> root_blob;
    ThrowIfFailed(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                            &root_blob, &error), "serialize root");
    ComPtr<ID3D12RootSignature> root;
    ThrowIfFailed(device.Get()->CreateRootSignature(0, root_blob->GetBufferPointer(),
                  root_blob->GetBufferSize(), IID_PPV_ARGS(&root)), "root");
    D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{};
    pso_desc.pRootSignature = root.Get();
    pso_desc.CS = {code->GetBufferPointer(), code->GetBufferSize()};
    ComPtr<ID3D12PipelineState> pso;
    ThrowIfFailed(device.Get()->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&pso)), "pso");
    DescriptorRing ring{device.Get(), scheduler, 128};
    auto output = CreateCommittedBuffer(device.Get(), 24, D3D12_HEAP_TYPE_DEFAULT,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, "output");
    auto readback = CreateCommittedBuffer(device.Get(), 24, D3D12_HEAP_TYPE_READBACK,
        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE, "readback");
    const auto sample = [&](D3D12_CPU_DESCRIPTOR_HANDLE handle, u32 first) {
        auto gpu = ring.Upload(std::span{&handle, 1});
        auto* cmd = scheduler.CommandList();
        ID3D12DescriptorHeap* heap = ring.Heap();
        cmd->SetDescriptorHeaps(1, &heap);
        cmd->SetComputeRootSignature(root.Get());
        scheduler.SetPipelineState(pso.Get());
        cmd->SetComputeRootDescriptorTable(0, gpu);
        cmd->SetComputeRootUnorderedAccessView(1, output->GetGPUVirtualAddress());
        cmd->Dispatch(1, 1, 1);
        TransitionResource(cmd, output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                           D3D12_RESOURCE_STATE_COPY_SOURCE);
        cmd->CopyBufferRegion(readback.Get(), 0, output.Get(), 0, 24);
        TransitionResource(cmd, output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        scheduler.Finish();
        void* data{};
        const D3D12_RANGE read{0, 24};
        ThrowIfFailed(readback->Map(0, &read, &data), "map samples");
        for (u32 face = 0; face < 6; ++face) {
            assert(static_cast<u32*>(data)[face] == (first ? first + face : 0));
        }
        const D3D12_RANGE written{0, 0};
        readback->Unmap(0, &written);
    };
    for (u32 layers : {1U, 5U, 6U, 12U}) {
        ImageInfo info;
        info.type = ImageType::e2D;
        info.format = PixelFormat::A8B8G8R8_UNORM;
        info.size = {8, 8, 1};
        info.resources = {3, static_cast<s32>(layers)};
        Image image{runtime, info, 0, 0};
        auto upload = runtime.UploadStagingBuffer(4 * 4 * 4 * layers);
        for (u32 layer = 0; layer < layers; ++layer) {
            for (u32 pixel = 0; pixel < 16; ++pixel) {
                upload.mapped_span[layer * 64 + pixel * 4] = u8(10 + layer);
            }
        }
        const BufferImageCopy copy{.buffer_offset = 0, .buffer_size = 64 * layers,
            .buffer_row_length = 4, .buffer_image_height = 4,
            .image_subresource = {1, 0, static_cast<s32>(layers)},
            .image_offset = {}, .image_extent = {4, 4, 1}};
        image.UploadMemory(upload, std::span{&copy, 1});
        image.Transition(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        for (u32 base : {0U, layers == 12 ? 6U : 0U, layers == 12 ? 7U : 0U}) {
            ImageViewInfo vi{ImageViewType::e2DArray, info.format,
                {{1, static_cast<s32>(base)}, {1, static_cast<s32>(layers - base)}}};
            ImageView view{runtime, vi, {}, image};
            const auto cube = view.Handle(Shader::TextureType::ColorCube);
            const std::string metadata = view.DescribeSrv(Shader::TextureType::ColorCube);
            assert(metadata.find(layers - base >= 6 ? "null=false" : "null=true") != std::string::npos);
            assert(metadata.find("mips=1+1") != std::string::npos);
            assert(cube.ptr == view.Handle(Shader::TextureType::ColorCube).ptr);
            assert(cube.ptr != view.Handle(Shader::TextureType::ColorArray2D).ptr);
            sample(cube, layers - base >= 6 ? 10 + base : 0);
            assert(view.Handle(Shader::TextureType::ColorArrayCube).ptr);
        }
        scheduler.Finish();
    }
    // Original BC3/ColorCube/Texture2D rejection and a rectangular six-face resource.
    // Also cover a natural cube constructor. MSAA compatibility is checked above:
    // the production placed allocator has an independent MSAA heap-alignment error.
    for (u32 scenario = 0; scenario < 2; ++scenario) {
        ImageInfo info;
        info.type = ImageType::e2D;
        info.format = scenario == 0 ? PixelFormat::BC3_UNORM : PixelFormat::A8B8G8R8_UNORM;
        info.size = {8, scenario == 1 ? 4U : 8U, 1};
        info.resources = {1, scenario == 0 ? 1 : 6};
        Image image{runtime, info, 0, 0};
        ImageViewInfo vi{ImageViewType::Cube, info.format, {{0, 0}, {1, info.resources.layers}}};
        ImageView view{runtime, vi, {}, image};
        const auto cube = view.Handle(Shader::TextureType::ColorCube);
        assert(view.DescribeSrv(Shader::TextureType::ColorCube).find("null=true") != std::string::npos);
        sample(cube, 0);
        assert(cube.ptr != view.Handle(Shader::TextureType::ColorArray2D).ptr);
    }
    // Exercise all requested dimensions on each physical dimension, then destruction/reuse.
    for (auto dimension : {ImageType::e1D, ImageType::e2D, ImageType::e3D}) {
        ImageInfo info;
        info.type = dimension;
        info.format = PixelFormat::A8B8G8R8_UNORM;
        info.size = {8, dimension == ImageType::e1D ? 1U : 8U,
                     dimension == ImageType::e3D ? 4U : 1U};
        info.resources = {3, 1};
        Image image{runtime, info, 0, 0};
        const auto vt = dimension == ImageType::e1D ? ImageViewType::e1D :
                        dimension == ImageType::e3D ? ImageViewType::e3D : ImageViewType::e2D;
        {
            ImageViewInfo vi{vt, info.format, {{0, 0}, {3, 1}}};
            ImageView view{runtime, vi, {}, image};
            std::set<SIZE_T> handles;
            for (u32 t = 0; t < Shader::NUM_TEXTURE_TYPES; ++t) {
                assert(handles.insert(view.Handle(static_cast<Shader::TextureType>(t)).ptr).second);
            }
        }
        std::set<SIZE_T> recycled;
        std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> allocated;
        for (u32 t = 0; t < Shader::NUM_TEXTURE_TYPES; ++t) {
            auto handle = views.Allocate();
            assert(recycled.insert(handle.ptr).second);
            allocated.push_back(handle);
        }
        for (auto handle : allocated) views.Free(handle);
    }
    // Original TSC and translated sampler metadata survive ownership transfers.
    {
        Tegra::Texture::TSCEntry tsc{};
        tsc.min_filter.Assign(Tegra::Texture::TextureFilter::Linear);
        tsc.mag_filter.Assign(Tegra::Texture::TextureFilter::Linear);
        tsc.mipmap_filter.Assign(Tegra::Texture::TextureMipmapFilter::Linear);
        tsc.wrap_u.Assign(Tegra::Texture::WrapMode::Border);
        tsc.border_color = {1.0f, 0.5f, 0.25f, 0.0f};
        Sampler original{runtime, tsc};
        const auto metadata = original.Describe();
        assert(metadata.find("border [1,0.5,0.25,0]") != std::string::npos);
        assert(metadata.find("TSC=") != std::string::npos);
        Sampler moved{std::move(original)};
        assert(moved.Describe() == metadata);
        Sampler assigned{runtime, tsc};
        assigned = std::move(moved);
        assert(assigned.Describe() == metadata);
    }
    scheduler.Finish();
    ThrowIfFailed(device.Get()->GetDeviceRemovedReason(), "removed device");
    for (u64 i = 0; i < messages->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
        SIZE_T bytes{};
        messages->GetMessage(i, nullptr, &bytes);
        std::vector<u8> storage(bytes);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        messages->GetMessage(i, message, &bytes);
        if (message->Severity <= D3D12_MESSAGE_SEVERITY_WARNING) {
            std::cerr << message->pDescription << '\n';
            return 1;
        }
    }
    std::cout << "PASS: cube GPU samples six faces/mip/base-layer, null cube zero reads, "
                 "all dimensions, descriptor caching/ownership; D3D12 debug clean\n";
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
