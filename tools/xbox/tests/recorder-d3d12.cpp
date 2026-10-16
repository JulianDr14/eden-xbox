// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <iostream>
#include <vector>
#include <d3dcompiler.h>
#include <d3d12sdklayers.h>
#include "common/logging.h"
#include "common/settings.h"
#include "video_core/perf_counters.h"
#include "video_core/renderer_d3d12/d3d12_device.h"
#include "video_core/renderer_d3d12/d3d12_command_stream.h"
#include "video_core/renderer_d3d12/d3d12_resource_utils.h"
using namespace D3D12;
static u64 Time(FILETIME f){return (u64{f.dwHighDateTime}<<32)|f.dwLowDateTime;}
static u64 Cpu(){FILETIME a,b,c,d;assert(GetProcessTimes(GetCurrentProcess(),&a,&b,&c,&d));return Time(c)+Time(d);}
static ComPtr<ID3DBlob> Shader(const char* src,const char* entry,const char* target){
    ComPtr<ID3DBlob> code,error;
    auto hr=D3DCompile(src,std::strlen(src),nullptr,nullptr,nullptr,entry,target,D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&error);
    if(FAILED(hr)&&error)std::cerr<<static_cast<const char*>(error->GetBufferPointer());
    ThrowIfFailed(hr,"shader");return code;
}
struct Result{double producer{},drain{},complete{},cpu{};};
static Result Run(bool threaded,bool debug,u32 burn){
    Settings::values.renderer_debug=debug;
    Device device;
    ComPtr<ID3D12InfoQueue> messages;if(debug)ThrowIfFailed(device.Get()->QueryInterface(IID_PPV_ARGS(&messages)),"info queue");
    const D3D12_ROOT_PARAMETER param{.ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS,
        .Constants={.ShaderRegister=0,.RegisterSpace=0,.Num32BitValues=1},.ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL};
    const D3D12_ROOT_SIGNATURE_DESC rd{.NumParameters=1,.pParameters=&param,.Flags=D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
    auto root=CreateRootSignature(device.Get(),rd,"benchmark root");
    const char* source=R"(cbuffer C:register(b0){uint value;};
        float4 VS(uint id:SV_VertexID):SV_Position {float2 p=float2((id<<1)&2,id&2);return float4(p*float2(2,-2)+float2(-1,1),0,1);}
        uint4 PS():SV_Target{return uint4(value&255,(value>>8)&255,91,255);})";
    auto vs=Shader(source,"VS","vs_5_0"),ps=Shader(source,"PS","ps_5_0");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature=root.Get();pd.VS={vs->GetBufferPointer(),vs->GetBufferSize()};pd.PS={ps->GetBufferPointer(),ps->GetBufferSize()};
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask=UINT_MAX;pd.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;pd.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable=TRUE;pd.DepthStencilState.DepthEnable=FALSE;pd.DepthStencilState.StencilEnable=FALSE;
    pd.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;pd.NumRenderTargets=1;pd.RTVFormats[0]=DXGI_FORMAT_R8G8B8A8_UINT;pd.SampleDesc.Count=1;
    ComPtr<ID3D12PipelineState> pso;ThrowIfFailed(device.Get()->CreateGraphicsPipelineState(&pd,IID_PPV_ARGS(&pso)),"benchmark PSO");
    const D3D12_RESOURCE_DESC td{.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D,.Width=1,.Height=1,.DepthOrArraySize=1,.MipLevels=1,
        .Format=DXGI_FORMAT_R8G8B8A8_UINT,.SampleDesc={1,0},.Layout=D3D12_TEXTURE_LAYOUT_UNKNOWN,.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
    const D3D12_HEAP_PROPERTIES heap{.Type=D3D12_HEAP_TYPE_DEFAULT};
    ComPtr<ID3D12Resource> target;ThrowIfFailed(device.Get()->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&td,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&target)),"target");
    const D3D12_DESCRIPTOR_HEAP_DESC hd{.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV,.NumDescriptors=1};
    ComPtr<ID3D12DescriptorHeap> rtv;ThrowIfFailed(device.Get()->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&rtv)),"rtv heap");
    auto handle=rtv->GetCPUDescriptorHandleForHeapStart();device.Get()->CreateRenderTargetView(target.Get(),nullptr,handle);
    auto readback=CreateCommittedBuffer(device.Get(),256,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_FLAG_NONE,"benchmark readback");
    constexpr u32 LISTS=8,DRAWS=4000;
    std::array<ComPtr<ID3D12CommandAllocator>,LISTS> allocators;
    for(auto& a:allocators)ThrowIfFailed(device.Get()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&a)),"allocator");
    ComPtr<ID3D12GraphicsCommandList> list;ThrowIfFailed(device.Get()->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocators[0].Get(),nullptr,IID_PPV_ARGS(&list)),"list");
    ThrowIfFailed(list->Close(),"initial close");
    ComPtr<ID3D12Fence> fence;ThrowIfFailed(device.Get()->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"fence");
    HANDLE event=CreateEventExW(nullptr,nullptr,0,EVENT_ALL_ACCESS);assert(event);
    ListRecorder recorder{list.Get(),{.threaded=threaded,.profile=debug}};
    CommandStream stream{recorder};
    if(messages)messages->ClearStoredMessages();
    const u64 cpu=Cpu();auto start=std::chrono::steady_clock::now();
    u64 checksum=1;
    for(u32 batch=0;batch<LISTS;++batch){
        stream.Record([&,batch](auto* l){ThrowIfFailed(allocators[batch]->Reset(),"reset allocator");ThrowIfFailed(l->Reset(allocators[batch].Get(),nullptr),"reset list");});
        stream.SetPipelineState(pso.Get());stream.SetGraphicsRootSignature(root.Get());
        const D3D12_VIEWPORT viewport{0,0,1,1,0,1};const D3D12_RECT scissor{0,0,1,1};
        stream.RSSetViewports(1,&viewport);stream.RSSetScissorRects(1,&scissor);stream.OMSetRenderTargets(1,&handle,FALSE,nullptr);stream.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        for(u32 i=0;i<DRAWS;++i){
            for(u32 k=0;k<burn;++k)checksum=(checksum^(checksum>>13))*0x9e3779b97f4a7c15ULL+k;
            stream.SetGraphicsRoot32BitConstants(0,1,&i,0);stream.DrawInstanced(3,1,0,0);
        }
        if(batch+1==LISTS){
            TransitionResource(&stream,target.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE);
            const D3D12_TEXTURE_COPY_LOCATION dst{.pResource=readback.Get(),.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
                .PlacedFootprint={.Offset=0,.Footprint={.Format=DXGI_FORMAT_R8G8B8A8_UINT,.Width=1,.Height=1,.Depth=1,.RowPitch=256}}};
            const D3D12_TEXTURE_COPY_LOCATION src{.pResource=target.Get(),.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,.SubresourceIndex=0};
            stream.CopyTextureRegion(&dst,0,0,0,&src,nullptr);
        }
        stream.Record([&,batch](auto* l){ThrowIfFailed(l->Close(),"close");ID3D12CommandList* lists[]={l};device.Queue()->ExecuteCommandLists(1,lists);ThrowIfFailed(device.Queue()->Signal(fence.Get(),batch+1),"signal");});
        recorder.Dispatch();
    }
    auto produced=std::chrono::steady_clock::now();recorder.WaitIdle();auto drained=std::chrono::steady_clock::now();
    ThrowIfFailed(fence->SetEventOnCompletion(LISTS,event),"wait fence");assert(WaitForSingleObject(event,INFINITE)==WAIT_OBJECT_0);
    auto completed=std::chrono::steady_clock::now();const u64 cpuend=Cpu();CloseHandle(event);
    void* mapped{};const D3D12_RANGE range{0,4};ThrowIfFailed(readback->Map(0,&range,&mapped),"map");
    auto* pixel=static_cast<const u8*>(mapped);assert(pixel[0]==((DRAWS-1)&255)&&pixel[1]==((DRAWS-1)>>8)&&pixel[2]==91&&pixel[3]==255);
    const D3D12_RANGE no_write{};readback->Unmap(0,&no_write);
    if(messages)for(UINT64 i=0;i<messages->GetNumStoredMessages();++i){SIZE_T n{};messages->GetMessage(i,nullptr,&n);std::vector<std::byte> bytes(n);auto* m=reinterpret_cast<D3D12_MESSAGE*>(bytes.data());ThrowIfFailed(messages->GetMessage(i,m,&n),"message");if(m->Severity<=D3D12_MESSAGE_SEVERITY_WARNING)throw std::runtime_error(m->pDescription);}
    if(debug){const auto stats=recorder.ReadStatistics();std::cout<<"PASS actual GPU draws async="<<threaded<<" last pixel exact, 8 lists/32000 draws, chunks="<<stats.chunks<<" bytes="<<stats.bytes<<" pending="<<stats.max_pending<<" handoff_ms="<<stats.handoff_ns/1e6<<" execute_ms="<<stats.execute_ns/1e6<<" drain_ms="<<stats.drain_ns/1e6<<" checksum="<<checksum<<"\n";}
    auto ms=[](auto d){return std::chrono::duration<double,std::milli>(d).count();};
    return {ms(produced-start),ms(drained-produced),ms(completed-start),(cpuend-cpu)/10000.0};
}
int main(){
    Common::Log::Initialize();Common::Log::Start();VideoCore::Perf::SetDetailedGpuProfile(false);
    Run(false,true,0);Run(true,true,0);
    for(u32 burn:{0u,128u,1024u}){
        std::array<std::array<Result,7>,2> samples;
        for(u32 s=0;s<7;++s)for(u32 turn=0;turn<2;++turn){bool mode=(s+turn)%2;samples[mode][s]=Run(mode,false,burn);}
        for(u32 mode=0;mode<2;++mode){
            auto print=[&](const char* label,auto field){std::array<double,7> a;for(u32 s=0;s<7;++s)a[s]=field(samples[mode][s]);std::sort(a.begin(),a.end());std::cout<<" "<<label<<"="<<a[3]<<"["<<a.front()<<","<<a.back()<<"]";};
            std::cout<<"burn="<<burn<<" async="<<mode<<" ms median[min,max]";
            print("producer",[](auto r){return r.producer;});print("drain",[](auto r){return r.drain;});print("complete",[](auto r){return r.complete;});print("processCPU",[](auto r){return r.cpu;});std::cout<<"\n";
        }
    }
    std::cout<<"PASS FIFO GPU commands/lists/constants, resource lifetime, no debug warnings. Synthetic load, not gameplay FPS.\n";
}
