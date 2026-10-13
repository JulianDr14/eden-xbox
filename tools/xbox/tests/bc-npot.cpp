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
#include <bit>
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
SamplerState wrapPoint : register(s3);
RWByteAddressBuffer result : register(u0);
[numthreads(32,1,1)] void main(uint3 id : SV_DispatchThreadID) {
    uint w,h,levels; tex.GetDimensions(0,w,h,levels);
    tex.GetDimensions(id.y,w,h,levels);
    if(id.x==0) {
        uint meta=levels*32*32+id.y*12;
        result.Store(meta,w); result.Store(meta+4,h); result.Store(meta+8,levels);
    }
    const float u[8]={-1.0/w,0,0.5/w,1.0-0.5/w,1,1.0+1.0/w,-1,2};
    const float v[4]={-1.0/h,0,1,1.0+1.0/h};
    float2 uv=float2(u[id.x%8],v[(id.x/8)%4]);
    float lod=id.y+(id.x<16 ? 0.0 : 0.25);
    float4 a=tex.SampleLevel(wrapLinear,uv,lod);
    float4 b=tex.SampleLevel(clampLinear,uv,lod);
    float4 c=tex.SampleLevel(wrapAniso,uv,lod);
    float4 p=tex.SampleLevel(wrapPoint,uv,lod);
    uint offset=(id.y*32+id.x)*32;
    result.Store4(offset,asuint(a));
    result.Store4(offset+16,asuint(float4(b.g,c.g,p.g,p.a)));
})";
    ComPtr<ID3DBlob> code,error,root_blob;
    ThrowIfFailed(D3DCompile(shader,sizeof(shader),nullptr,nullptr,nullptr,"main","cs_5_1",0,0,&code,&error),"compile");
    D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0,0,0};
    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; params[0].DescriptorTable={1,&range};
    params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;
    std::array<D3D12_STATIC_SAMPLER_DESC,4> static_samplers{};
    for (u32 i=0;i<4;++i) {
        auto& sampler=static_samplers[i]; sampler.ShaderRegister=i;
        sampler.Filter=i==2 ? D3D12_FILTER_ANISOTROPIC : i==3 ? D3D12_FILTER_MIN_MAG_MIP_POINT : D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU=sampler.AddressV=sampler.AddressW=i==1 ? D3D12_TEXTURE_ADDRESS_MODE_CLAMP : D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        sampler.MaxAnisotropy=16; sampler.MaxLOD=D3D12_FLOAT32_MAX; sampler.ComparisonFunc=D3D12_COMPARISON_FUNC_ALWAYS;
    }
    D3D12_ROOT_SIGNATURE_DESC root_desc{2,params,4,static_samplers.data()};
    ThrowIfFailed(D3D12SerializeRootSignature(&root_desc,D3D_ROOT_SIGNATURE_VERSION_1,&root_blob,&error),"serialize");
    ComPtr<ID3D12RootSignature> root;
    ThrowIfFailed(device.Get()->CreateRootSignature(0,root_blob->GetBufferPointer(),root_blob->GetBufferSize(),IID_PPV_ARGS(&root)),"root");
    D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{}; pso_desc.pRootSignature=root.Get(); pso_desc.CS={code->GetBufferPointer(),code->GetBufferSize()};
    ComPtr<ID3D12PipelineState> pso;
    ThrowIfFailed(device.Get()->CreateComputePipelineState(&pso_desc,IID_PPV_ARGS(&pso)),"pso");
    DescriptorRing ring{device.Get(),scheduler,128};
    constexpr u32 samples_per_mip=32, components_per_sample=8, max_mips=16;
    constexpr u32 max_output_bytes=max_mips*(samples_per_mip*components_per_sample*4+12);
    auto output=CreateCommittedBuffer(device.Get(),max_output_bytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,"samples");
    auto readback=CreateCommittedBuffer(device.Get(),max_output_bytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_FLAG_NONE,"readback");
    struct Extent { u32 width, height; };
    constexpr std::array extents{
        Extent{7,7}, Extent{717,717}, Extent{718,718}, Extent{719,719}, Extent{720,720},
        Extent{717,720}, Extent{720,717}, Extent{718,719}, Extent{719,718}, Extent{720,716},
        Extent{1,1}, Extent{1,2}, Extent{2,1}, Extent{2,2}, Extent{3,3}, Extent{3,5},
        Extent{5,3}, Extent{4,4}, Extent{4,8}, Extent{8,4}, Extent{1,719}, Extent{719,1}};
    u32 sample_count=0, image_count=0, mip_count=0;
    for (const auto extent : extents) {
    const u32 levels=std::bit_width(std::max(extent.width,extent.height));
    const bool needs_decode=extent.width%4!=0 || extent.height%4!=0;
    for(auto format : {PixelFormat::BC1_RGBA_UNORM,PixelFormat::BC1_RGBA_SRGB,PixelFormat::BC3_UNORM,PixelFormat::BC3_SRGB}) {
      for (u8 alpha : {u8(255), u8(0)}) {
        if (alpha == 0 && VideoCore::Surface::BytesPerBlock(format) != 16) continue;
        ImageInfo info; info.type=ImageType::e2D; info.format=format;
        info.size={extent.width,extent.height,1}; info.resources={static_cast<s32>(levels),1};
        Image image{runtime,info,0,0};
        const auto resource_desc=image.Handle()->GetDesc();
        if(image.IsBcDecoded()!=needs_decode || resource_desc.Width!=extent.width ||
           resource_desc.Height!=extent.height || resource_desc.MipLevels!=levels) {
            throw std::runtime_error("BC logical extent or aligned native route changed");
        }
        std::vector<BufferImageCopy> copies; std::vector<u8> raw;
        const u32 block_bytes=VideoCore::Surface::BytesPerBlock(format);
        for(s32 level=0;level<static_cast<s32>(levels);++level) {
            const u32 width=std::max(1U,extent.width>>level), height=std::max(1U,extent.height>>level);
            const u32 blocks_x=(width+3)/4, blocks_y=(height+3)/4;
            const size_t offset=raw.size(), size=blocks_x*blocks_y*block_bytes; raw.resize(offset+size);
            for(u32 by=0;by<blocks_y;++by) for(u32 bx=0;bx<blocks_x;++bx) {
                u8* block=raw.data()+offset+(by*blocks_x+bx)*block_bytes;
                if(block_bytes==16) {
                    block[0]=alpha; block[1]=255-alpha;
                    u64 alpha_indices=0;
                    for(u32 y=0;y<4;++y) for(u32 x=0;x<4;++x)
                        if(bx*4+x>=width || by*4+y>=height) alpha_indices |= 1ULL << ((y*4+x)*3);
                    std::memcpy(block+2,&alpha_indices,6); block+=8;
                }
                // Alternate red/green per mip: identical mip contents would hide a wrong LOD.
                // Every invalid texel is black with opposite alpha, exposing padding bleed.
                const u16 color=level%2 ? 0xf800 : 0x07e0; std::memcpy(block,&color,2);
                u32 indices=0;
                for(u32 y=0;y<4;++y) for(u32 x=0;x<4;++x)
                    if(bx*4+x>=width || by*4+y>=height) indices |= 1U << ((y*4+x)*2);
                std::memcpy(block+4,&indices,4);
            }
            copies.push_back({.buffer_offset=offset,.buffer_size=size,.buffer_row_length=blocks_x*4,
                .buffer_image_height=blocks_y*4,.image_subresource={level,0,1},.image_offset={},.image_extent={width,height,1}});
        }
        const auto original_copies=copies;
        auto upload=runtime.UploadStagingBuffer(image.IsBcDecoded() ? image.converted_size_bytes : raw.size());
        if(image.IsBcDecoded()) {
            image.PreserveGuestBlocks(raw,copies); ConvertImage(raw,info,upload.mapped_span,copies);
        } else std::memcpy(upload.mapped_span.data(),raw.data(),raw.size());
        image.UploadMemory(upload,copies); image.Transition(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        ImageViewInfo vi{ImageViewType::e2D,format,{{0,0},{static_cast<s32>(levels),1}}}; ImageView view{runtime,vi,{},image};
        auto handle=view.Handle(Shader::TextureType::Color2D); auto gpu=ring.Upload(std::span{&handle,1});
        auto* cmd=scheduler.CommandList(); ID3D12DescriptorHeap* heap=ring.Heap(); cmd->SetDescriptorHeaps(1,&heap);
        cmd->SetComputeRootSignature(root.Get()); scheduler.SetPipelineState(pso.Get());
        cmd->SetComputeRootDescriptorTable(0,gpu); cmd->SetComputeRootUnorderedAccessView(1,output->GetGPUVirtualAddress()); cmd->Dispatch(1,levels,1);
        TransitionResource(cmd,output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
        const u32 component_count=levels*samples_per_mip*components_per_sample;
        const u32 output_bytes=component_count*4+levels*12;
        cmd->CopyBufferRegion(readback.Get(),0,output.Get(),0,output_bytes);
        TransitionResource(cmd,output.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS); scheduler.Finish();
        void* mapped{}; ThrowIfFailed(readback->Map(0,nullptr,&mapped),"map");
        const auto* values=static_cast<float*>(mapped);
        const auto* dimensions=reinterpret_cast<const u32*>(values+component_count);
        for(u32 level=0;level<levels;++level) {
            if(dimensions[level*3]!=std::max(1U,extent.width>>level) ||
               dimensions[level*3+1]!=std::max(1U,extent.height>>level) || dimensions[level*3+2]!=levels) {
                throw std::runtime_error("Shader-visible BC mip dimensions changed");
            }
            for(u32 sample=0;sample<samples_per_mip;++sample) {
                const float green=level%2 ? 0.0f : 1.0f;
                const float next_green=std::min(level+1,levels-1)%2 ? 0.0f : 1.0f;
                const float filtered=sample<16 ? green : green*0.75f+next_green*0.25f;
                const float opacity=alpha/255.0f;
                const std::array<float,8> expected{1-filtered,filtered,0,opacity,filtered,filtered,green,opacity};
                for(u32 channel=0;channel<components_per_sample;++channel) {
                    const float actual=values[(level*samples_per_mip+sample)*components_per_sample+channel];
                    if(!std::isfinite(actual) || std::abs(actual-expected[channel])>0.00001f) {
                        throw std::runtime_error("BC sample mismatch: "+std::to_string(extent.width)+"x"+
                            std::to_string(extent.height)+" format="+std::to_string(static_cast<u32>(format))+
                            " mip="+std::to_string(level)+" sample="+std::to_string(sample)+
                            " channel="+std::to_string(channel)+" actual="+std::to_string(actual)+
                            " expected="+std::to_string(expected[channel]));
                    }
                }
            }
        }
        sample_count+=component_count; ++image_count; mip_count+=levels;
        readback->Unmap(0,nullptr);
        if(needs_decode) {
            if(!image.GuestBlocks()) throw std::runtime_error("original blocks not retained");
            auto blocks=runtime.DownloadStagingBuffer(raw.size());
            image.GuestBlocks()->DownloadMemory(blocks,original_copies); scheduler.Finish();
            if(std::memcmp(blocks.mapped_span.data(),raw.data(),raw.size())!=0) {
                throw std::runtime_error("Original BC block bytes changed");
            }
        }
      }
    }
    std::cout<<"Validated "<<extent.width<<'x'<<extent.height<<" with "<<levels<<" mips\n";
    }
    scheduler.Finish();
    for(u64 i=0;i<messages->GetNumStoredMessagesAllowedByRetrievalFilter();++i) {
        SIZE_T size{}; messages->GetMessage(i,nullptr,&size); std::vector<u8> bytes(size);
        auto* message=reinterpret_cast<D3D12_MESSAGE*>(bytes.data()); messages->GetMessage(i,message,&size);
        if(message->Severity<=D3D12_MESSAGE_SEVERITY_WARNING) { std::cerr<<message->pDescription<<'\n'; return 1; }
    }
    std::cout<<"PASS NPOT BC: "<<extents.size()<<" extents, "<<image_count<<" images, "<<mip_count
        <<" mip levels, "<<sample_count<<" GPU components; "
        "point/linear/wrap/clamp/fractional LOD/aniso SampleLevel, sRGB/alpha, exact original blocks, aligned native path; debug clean\n";
    return 0;
} catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
