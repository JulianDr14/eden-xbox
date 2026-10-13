// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include "common/logging.h"
#include "common/settings.h"
#include "video_core/renderer_d3d12/d3d12_scheduler.h"
#include "video_core/renderer_d3d12/d3d12_texture_cache.h"
#include "video_core/renderer_d3d12/d3d12_resource_utils.h"
#include "video_core/texture_cache/image_view_info.h"
#include "video_core/texture_cache/util.h"
#include <d3d12sdklayers.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <vector>
using namespace D3D12;
using namespace VideoCommon;
using VideoCore::Surface::PixelFormat;
int main() try {
    Common::Log::Initialize(); Settings::values.renderer_debug = true;
    Device device;
    ComPtr<ID3D12InfoQueue> messages;
    ThrowIfFailed(device.Get()->QueryInterface(IID_PPV_ARGS(&messages)), "debug queue");
    messages->ClearStoredMessages();
    Scheduler scheduler{device}; StagingBufferPool staging{device, scheduler};
    TransferBufferPool transfers{device, scheduler};
    CpuDescriptorAllocator views{device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV};
    CpuDescriptorAllocator samplers{device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER};
    CpuDescriptorAllocator rtvs{device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV};
    CpuDescriptorAllocator dsvs{device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV};
    TextureCacheRuntime runtime{device, scheduler, staging, transfers, views, samplers, rtvs, dsvs};
    const char shader[] = R"(
Texture2D<float4> tex : register(t0);
SamplerState wrapLinear : register(s0);
SamplerState clampLinear : register(s1);
SamplerState wrapAniso : register(s2);
RWByteAddressBuffer result : register(u0);
[numthreads(32,1,1)] void main(uint3 id : SV_DispatchThreadID) {
    uint w,h,levels; tex.GetDimensions(0,w,h,levels);
    if(id.x==0) { result.Store(512,w); result.Store(516,h); result.Store(520,levels); }
    float2 uv = float2((int(id.x % 8)-2)/7.0, id.x % 2 ? 0.0 : 1.0);
    float lod = (id.x/8)*0.5;
    float4 a=tex.SampleLevel(wrapLinear,uv,lod);
    float4 b=tex.SampleLevel(clampLinear,uv,lod);
    float4 c=tex.SampleLevel(wrapAniso,uv,lod);
    result.Store4(id.x*16,asuint(float4(a.g,b.g,c.g,a.a)));
})";
    ComPtr<ID3DBlob> code,error,root_blob;
    ThrowIfFailed(D3DCompile(shader,sizeof(shader),nullptr,nullptr,nullptr,"main","cs_5_1",0,0,&code,&error),"compile");
    D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0,0,0};
    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; params[0].DescriptorTable={1,&range};
    params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;
    std::array<D3D12_STATIC_SAMPLER_DESC,3> static_samplers{};
    for (u32 i=0;i<3;++i) {
        auto& sampler=static_samplers[i]; sampler.ShaderRegister=i;
        sampler.Filter=i==2 ? D3D12_FILTER_ANISOTROPIC : D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU=sampler.AddressV=sampler.AddressW=i==1 ? D3D12_TEXTURE_ADDRESS_MODE_CLAMP : D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        sampler.MaxAnisotropy=16; sampler.MaxLOD=D3D12_FLOAT32_MAX; sampler.ComparisonFunc=D3D12_COMPARISON_FUNC_ALWAYS;
    }
    D3D12_ROOT_SIGNATURE_DESC root_desc{2,params,3,static_samplers.data()};
    ThrowIfFailed(D3D12SerializeRootSignature(&root_desc,D3D_ROOT_SIGNATURE_VERSION_1,&root_blob,&error),"serialize");
    ComPtr<ID3D12RootSignature> root;
    ThrowIfFailed(device.Get()->CreateRootSignature(0,root_blob->GetBufferPointer(),root_blob->GetBufferSize(),IID_PPV_ARGS(&root)),"root");
    D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{}; pso_desc.pRootSignature=root.Get(); pso_desc.CS={code->GetBufferPointer(),code->GetBufferSize()};
    ComPtr<ID3D12PipelineState> pso;
    ThrowIfFailed(device.Get()->CreateComputePipelineState(&pso_desc,IID_PPV_ARGS(&pso)),"pso");
    DescriptorRing ring{device.Get(),scheduler,128};
    auto output=CreateCommittedBuffer(device.Get(),524,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,"samples");
    auto readback=CreateCommittedBuffer(device.Get(),524,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_FLAG_NONE,"readback");
    u32 sample_count=0;
    for(auto format : {PixelFormat::BC1_RGBA_UNORM,PixelFormat::BC1_RGBA_SRGB,PixelFormat::BC3_UNORM,PixelFormat::BC3_SRGB}) {
      for (u8 alpha : {u8(255), u8(0)}) {
        if (alpha == 0 && VideoCore::Surface::BytesPerBlock(format) != 16) continue;
        ImageInfo info; info.type=ImageType::e2D; info.format=format; info.size={7,7,1}; info.resources={3,1};
        Image image{runtime,info,0,0};
        std::vector<BufferImageCopy> copies; std::vector<u8> raw;
        const u32 block_bytes=VideoCore::Surface::BytesPerBlock(format);
        for(s32 level=0;level<3;++level) {
            const u32 width=7U>>level, blocks=(width+3)/4;
            const size_t offset=raw.size(), size=blocks*blocks*block_bytes; raw.resize(offset+size);
            for(u32 by=0;by<blocks;++by) for(u32 bx=0;bx<blocks;++bx) {
                u8* block=raw.data()+offset+(by*blocks+bx)*block_bytes;
                if(block_bytes==16) {
                    block[0]=alpha; block[1]=255-alpha;
                    u64 alpha_indices=0;
                    for(u32 y=0;y<4;++y) for(u32 x=0;x<4;++x)
                        if(bx*4+x>=width || by*4+y>=width) alpha_indices |= 1ULL << ((y*4+x)*3);
                    std::memcpy(block+2,&alpha_indices,6); block+=8;
                }
                const u16 green=0x07e0; std::memcpy(block,&green,2);
                u32 indices=0;
                for(u32 y=0;y<4;++y) for(u32 x=0;x<4;++x)
                    if(bx*4+x>=width || by*4+y>=width) indices |= 1U << ((y*4+x)*2);
                std::memcpy(block+4,&indices,4);
            }
            copies.push_back({.buffer_offset=offset,.buffer_size=size,.buffer_row_length=blocks*4,
                .buffer_image_height=blocks*4,.image_subresource={level,0,1},.image_offset={},.image_extent={width,width,1}});
        }
        auto upload=runtime.UploadStagingBuffer(image.IsBcDecoded() ? image.converted_size_bytes : raw.size());
        if(image.IsBcDecoded()) {
            image.PreserveGuestBlocks(raw,copies); ConvertImage(raw,info,upload.mapped_span,copies);
        } else std::memcpy(upload.mapped_span.data(),raw.data(),raw.size());
        image.UploadMemory(upload,copies); image.Transition(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        ImageViewInfo vi{ImageViewType::e2D,format,{{0,0},{3,1}}}; ImageView view{runtime,vi,{},image};
        auto handle=view.Handle(Shader::TextureType::Color2D); auto gpu=ring.Upload(std::span{&handle,1});
        auto* cmd=scheduler.CommandList(); ID3D12DescriptorHeap* heap=ring.Heap(); cmd->SetDescriptorHeaps(1,&heap);
        cmd->SetComputeRootSignature(root.Get()); scheduler.SetPipelineState(pso.Get());
        cmd->SetComputeRootDescriptorTable(0,gpu); cmd->SetComputeRootUnorderedAccessView(1,output->GetGPUVirtualAddress()); cmd->Dispatch(1,1,1);
        TransitionResource(cmd,output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
        cmd->CopyBufferRegion(readback.Get(),0,output.Get(),0,524);
        TransitionResource(cmd,output.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS); scheduler.Finish();
        void* mapped{}; ThrowIfFailed(readback->Map(0,nullptr,&mapped),"map");
        const auto* values=static_cast<float*>(mapped); const auto* dimensions=reinterpret_cast<const u32*>(values+128);
        std::cout << "BC " << static_cast<u32>(format) << ": virtual " << dimensions[0] << 'x' << dimensions[1] << ", first wrap G=" << values[0] << '\n';
        u32 bad=0; for(u32 i=0;i<128;++i) {
            const float expected = i % 4 == 3 ? alpha / 255.0f : 1.0f;
            if(!std::isfinite(values[i]) || std::abs(values[i]-expected)>0.00001f) ++bad;
        }
        if(dimensions[0]!=7 || dimensions[1]!=7 || dimensions[2]!=3 || bad) throw std::runtime_error("NPOT wrap/clamp/mip/alpha semantics changed: bad components="+std::to_string(bad));
        sample_count+=128; readback->Unmap(0,nullptr);
        if(!image.GuestBlocks() || image.GuestBlocks()->Handle()->GetDesc().Width!=8) throw std::runtime_error("original blocks not retained");
      }
    }
    // Actual observed extent, plus aligned singles retain the fast native BC path.
    ImageInfo info; info.type=ImageType::e2D; info.format=PixelFormat::BC1_RGBA_SRGB; info.size={717,717,1}; info.resources={10,1};
    Image observed{runtime,info,0,0};
    if(!observed.IsBcDecoded() || observed.Handle()->GetDesc().Width!=717 || observed.Handle()->GetDesc().Height!=717) throw std::runtime_error("observed extent not preserved");
    info.size={720,720,1}; Image aligned{runtime,info,0,0};
    if(aligned.IsBcDecoded() || aligned.Handle()->GetDesc().Format!=DXGI_FORMAT_BC1_TYPELESS) throw std::runtime_error("aligned native path changed");
    scheduler.Finish();
    for(u64 i=0;i<messages->GetNumStoredMessagesAllowedByRetrievalFilter();++i) {
        SIZE_T size{}; messages->GetMessage(i,nullptr,&size); std::vector<u8> bytes(size);
        auto* message=reinterpret_cast<D3D12_MESSAGE*>(bytes.data()); messages->GetMessage(i,message,&size);
        if(message->Severity<=D3D12_MESSAGE_SEVERITY_WARNING) { std::cerr<<message->pDescription<<'\n'; return 1; }
    }
    std::cout<<"PASS NPOT BC: "<<sample_count<<" GPU components, wrap/clamp/trilinear/aniso, sRGB/alpha, original blocks, 717 extent, aligned fast path; debug clean\n";
    return 0;
} catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
