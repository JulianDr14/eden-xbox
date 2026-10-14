// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
// Native differential probe, no guest shader or game data embedded.
// Input: float4 clip position + float4 affine screen attributes per triangle vertex.
// Optional captured mode instead reads float3 positions at offset 0, stride 32.
// Its fixed binding layout matches the projected-mask replay documented in xbox_sm3dw_lighting.md;
// it is not a general capture player. All guest assets are supplied externally.
#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <array>
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>
using Microsoft::WRL::ComPtr;
static void Check(HRESULT hr) {
    if (FAILED(hr)) throw std::runtime_error("D3D12 clip probe failure " + std::to_string(hr));
}
int main(int argc, char** argv) try {
    if (argc != 3 && argc != 5 && argc != 7 && argc != 12 && argc != 13) throw std::runtime_error(
        "usage: noperspective-clip FLOAT8_TRIANGLES OUTPUT_DIR [FIXTURE_VS FIXTURE_PS | VS_DXIL CB0 CB1 PS_DXIL [CB2 CB3 CB4 RG16_RAW BC4_M0_RAW [GS_DXIL]]]");
    const bool fixture = argc == 5;
    const bool captured_vs = argc >= 7;
    const bool captured_ps = argc >= 12;
    const unsigned constant_count = captured_ps ? 6 : 3;
    const auto size = std::filesystem::file_size(argv[1]);
    if (!size || size % 96 || size > 65536) throw std::runtime_error("invalid triangle input");
    std::vector<char> input(size);
    std::ifstream stream(argv[1], std::ios::binary); stream.read(input.data(), size);
    if (!stream) throw std::runtime_error("incomplete triangle input");
    constexpr UINT W = 1920, H = 1080;
    ComPtr<ID3D12Debug> debug; Check(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));
    debug->EnableDebugLayer();
    ComPtr<ID3D12Device> device;
    Check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
    ComPtr<IDXGIFactory4> factory; Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter1> adapter;
    Check(factory->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&adapter)));
    DXGI_ADAPTER_DESC1 ad{}; Check(adapter->GetDesc1(&ad));
    std::wcout << L"adapter: " << ad.Description << L'\n';
    ComPtr<ID3D12InfoQueue> messages; Check(device.As(&messages));
    D3D12_COMMAND_QUEUE_DESC qd{};
    ComPtr<ID3D12CommandQueue> queue; Check(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)));
    ComPtr<ID3D12CommandAllocator> allocator;
    Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
    ComPtr<ID3D12GraphicsCommandList> cmd;
    Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&cmd)));
    const auto buffer = [&](UINT64 bytes, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state) {
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = heap;
        D3D12_RESOURCE_DESC desc{}; desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = bytes; desc.Height = desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> result;
        Check(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&result)));
        return result;
    };
    auto vb = buffer(size, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    void* mapped{}; D3D12_RANGE empty{};
    Check(vb->Map(0, &empty, &mapped)); std::memcpy(mapped, input.data(), size); vb->Unmap(0, nullptr);
    std::array<D3D12_ROOT_PARAMETER, 7> parameters{};
    std::array<ComPtr<ID3D12Resource>, 6> constants;
    if (captured_vs) {
        for (unsigned i = 0; i < constant_count; ++i) {
            parameters[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            parameters[i].ShaderVisibility = captured_ps && i >= 2 && i < 5 ? D3D12_SHADER_VISIBILITY_PIXEL : D3D12_SHADER_VISIBILITY_VERTEX;
            parameters[i].Descriptor = {i == constant_count-1 ? 0u : i, i == constant_count-1 ? 31u : 0u};
            constants[i] = buffer(65536, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
            Check(constants[i]->Map(0, &empty, &mapped)); std::memset(mapped, 0, 65536);
            if (i < constant_count-1) {
                const char* cb_path = argv[i < 2 ? 4+i : 5+i];
                const auto bytes = std::filesystem::file_size(cb_path);
                if (bytes > 65536) throw std::runtime_error("captured cbuffer exceeds 64 KiB");
                std::ifstream cb(cb_path, std::ios::binary);
                cb.read(static_cast<char*>(mapped), bytes);
                if (!cb) throw std::runtime_error("incomplete cbuffer");
            } else {
                // This capture uses no Y flip or reverse Z.
                static_cast<UINT*>(mapped)[3] = 0;
            }
            constants[i]->Unmap(0, nullptr);
        }
    }
    D3D12_ROOT_SIGNATURE_DESC rd{}; rd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 5, 0, 0};
    std::array<D3D12_STATIC_SAMPLER_DESC, 2> samplers{};
    if (captured_ps) {
        parameters[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[6].DescriptorTable = {1, &range}; parameters[6].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        for (unsigned i=0; i<2; ++i) {
            auto& sampler=samplers[i]; sampler.ShaderRegister=5+i; sampler.ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
            sampler.Filter=i ? D3D12_FILTER_MIN_MAG_MIP_LINEAR : D3D12_FILTER_MIN_MAG_POINT_MIP_LINEAR;
            sampler.AddressU=sampler.AddressV=sampler.AddressW=i ? D3D12_TEXTURE_ADDRESS_MODE_WRAP : D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            sampler.MaxLOD=14; sampler.MaxAnisotropy=1; sampler.ComparisonFunc=D3D12_COMPARISON_FUNC_NEVER;
        }
        rd.NumStaticSamplers=2; rd.pStaticSamplers=samplers.data();
    }
    if (captured_vs) { rd.NumParameters = captured_ps ? 7 : 3; rd.pParameters = parameters.data(); }
    ComPtr<ID3DBlob> blob, errors;
    Check(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors));
    ComPtr<ID3D12RootSignature> root;
    Check(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root)));
    const char source[] = R"(
struct I { float4 p:POSITION; float4 a:TEXCOORD0; };
struct N { float4 p:SV_Position; noperspective float4 a:TEXCOORD0; };
struct S { float4 p:SV_Position; float4 a:TEXCOORD0; };
N VN(I i) { N o; o.p=i.p; o.p.z=(i.p.z+i.p.w)*0.5; o.a=i.a; return o; }
S VS(I i) { S o; o.p=i.p; o.p.z=(i.p.z+i.p.w)*0.5; o.a=i.a*i.p.w; return o; }
float4 PN(N i):SV_Target { return i.a; }
float4 PS(S i):SV_Target { return i.a/i.p.w; }
struct C { float4 p:SV_Position; noperspective float4 a:TEXCOORD0; noperspective float4 b:TEXCOORD1; };
float4 PC(C i):SV_Target { return float4(i.a.xy,i.b.xy); }
)";
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2};
    ComPtr<ID3D12DescriptorHeap> rtvs; Check(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvs)));
    std::array<ComPtr<ID3D12Resource>, 2> targets, downloads, roi_downloads;
    std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, 2> roi_footprints{};
    std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, 2> footprints{};
    std::array<ComPtr<ID3D12PipelineState>, 2> psos;
    const D3D12_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,0,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"TEXCOORD",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,16,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0}};
    cmd->SetGraphicsRootSignature(root.Get());
    if (captured_vs) for (unsigned i = 0; i < constant_count; ++i)
        cmd->SetGraphicsRootConstantBufferView(i, constants[i]->GetGPUVirtualAddress());
    ComPtr<ID3D12DescriptorHeap> resources;
    std::vector<ComPtr<ID3D12Resource>> texture_lifetime;
    if (captured_ps) {
        D3D12_DESCRIPTOR_HEAP_DESC desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,2,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
        Check(device->CreateDescriptorHeap(&desc,IID_PPV_ARGS(&resources)));
        for (unsigned texture=0;texture<2;++texture) {
            const unsigned width=texture ? 512 : W, height=texture ? 512 : H, levels=texture ? 10 : 1;
            D3D12_RESOURCE_DESC td{};td.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            td.Width=width;td.Height=height;td.DepthOrArraySize=td.SampleDesc.Count=1;td.MipLevels=levels;
            td.Format=texture ? DXGI_FORMAT_BC4_UNORM : DXGI_FORMAT_R16G16_FLOAT;
            D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;
            ComPtr<ID3D12Resource> image;
            Check(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&td,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&image)));
            std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layouts(levels);UINT64 bytes{};
            device->GetCopyableFootprints(&td,0,levels,0,layouts.data(),nullptr,nullptr,&bytes);
            auto upload=buffer(bytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
            Check(upload->Map(0,&empty,&mapped));std::memset(mapped,0,bytes);
            for (unsigned mip=0;mip<levels;++mip) {
                std::string path=argv[10+texture];
                if (texture) {
                    const auto marker=path.rfind("_m0_");
                    if (marker==std::string::npos) throw std::runtime_error("BC4 path requires _m0_ marker");
                    path.replace(marker,4,"_m"+std::to_string(mip)+"_");
                }
                const unsigned mw=std::max(1u,width>>mip),mh=std::max(1u,height>>mip);
                const unsigned rows=texture ? (mh+3)/4 : mh,row_bytes=texture ? (mw+3)/4*8 : mw*4;
                if (std::filesystem::file_size(path)!=size_t(rows)*row_bytes) throw std::runtime_error("native texture length mismatch");
                std::ifstream file(path,std::ios::binary);
                for (unsigned row=0;row<rows;++row) file.read(static_cast<char*>(mapped)+layouts[mip].Offset+row*layouts[mip].Footprint.RowPitch,row_bytes);
                if (!file) throw std::runtime_error("native texture read failure");
            }
            upload->Unmap(0,nullptr);
            for (unsigned mip=0;mip<levels;++mip) {
                D3D12_TEXTURE_COPY_LOCATION from{upload.Get(),D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};from.PlacedFootprint=layouts[mip];
                D3D12_TEXTURE_COPY_LOCATION to{image.Get(),D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};to.SubresourceIndex=mip;
                cmd->CopyTextureRegion(&to,0,0,0,&from,nullptr);
            }
            D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition={image.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE};
            cmd->ResourceBarrier(1,&barrier);
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=td.Format;srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Texture2D.MipLevels=levels;srv.Shader4ComponentMapping=texture ? 0x1000 : 0x1b08;
            auto handle=resources->GetCPUDescriptorHandleForHeapStart();handle.ptr+=texture*device->GetDescriptorHandleIncrementSize(desc.Type);
            device->CreateShaderResourceView(image.Get(),&srv,handle);
            texture_lifetime.push_back(std::move(image));texture_lifetime.push_back(std::move(upload));
        }
        ID3D12DescriptorHeap* heaps[]{resources.Get()};cmd->SetDescriptorHeaps(1,heaps);
        cmd->SetGraphicsRootDescriptorTable(6,resources->GetGPUDescriptorHandleForHeapStart());
    }
    const D3D12_VERTEX_BUFFER_VIEW view{vb->GetGPUVirtualAddress(), static_cast<UINT>(size), 32};
    cmd->IASetVertexBuffers(0, 1, &view); cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const D3D12_VIEWPORT vp{0,0,float(W),float(H),0,1}; cmd->RSSetViewports(1,&vp);
    const D3D12_RECT sc{0,0,W,H}; cmd->RSSetScissorRects(1,&sc);
    const unsigned target_count = captured_vs || fixture ? 1 : 2;
    for (unsigned i=0; i<target_count; ++i) {
        ComPtr<ID3DBlob> vs, ps;
        const auto compile = [&](const char* entry, const char* profile, ID3DBlob** out) {
            const auto hr = D3DCompile(source, sizeof(source), nullptr, nullptr, nullptr, entry, profile,
                                      D3DCOMPILE_IEEE_STRICTNESS, 0, out, &errors);
            if (FAILED(hr) && errors) std::cerr << static_cast<char*>(errors->GetBufferPointer());
            Check(hr);
        };
        compile(i ? "VS" : "VN", "vs_5_0", &vs); compile(captured_vs ? "PC" : i ? "PS" : "PN", "ps_5_0", &ps);
        std::vector<char> captured_bytecode, captured_pixel_bytecode, geometry_bytecode;
        if (captured_vs || fixture) {
            const auto bytes = std::filesystem::file_size(argv[3]);
            if (bytes < 32 || bytes > 16 * 1024 * 1024) throw std::runtime_error("invalid VS DXIL size");
            captured_bytecode.resize(bytes);
            std::ifstream shader(argv[3], std::ios::binary); shader.read(captured_bytecode.data(), bytes);
            if (!shader) throw std::runtime_error("incomplete VS DXIL");
            const char* pixel_path = argv[fixture ? 4 : 6];
            const auto pixel_bytes = std::filesystem::file_size(pixel_path);
            if (pixel_bytes < 32 || pixel_bytes > 16 * 1024 * 1024) throw std::runtime_error("invalid PS DXIL size");
            captured_pixel_bytecode.resize(pixel_bytes);
            std::ifstream pixel(pixel_path, std::ios::binary); pixel.read(captured_pixel_bytecode.data(), pixel_bytes);
            if (!pixel) throw std::runtime_error("incomplete PS DXIL");
        }
        D3D12_RESOURCE_DESC td{}; td.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width=W; td.Height=H; td.DepthOrArraySize=td.MipLevels=td.SampleDesc.Count=1;
        td.Format=DXGI_FORMAT_R32G32B32A32_FLOAT; td.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_HEAP_PROPERTIES hp{}; hp.Type=D3D12_HEAP_TYPE_DEFAULT;
        D3D12_CLEAR_VALUE optimized{}; optimized.Format=td.Format;
        for (float& channel : optimized.Color) channel=-99999;
        Check(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&td,D3D12_RESOURCE_STATE_RENDER_TARGET,
                                              &optimized,IID_PPV_ARGS(&targets[i])));
        UINT64 bytes{}; device->GetCopyableFootprints(&td,0,1,0,&footprints[i],nullptr,nullptr,&bytes);
        downloads[i]=buffer(bytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
        auto handle=rtvs->GetCPUDescriptorHandleForHeapStart();
        handle.ptr+=i*device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        device->CreateRenderTargetView(targets[i].Get(),nullptr,handle);
        const float clear[]{-99999,-99999,-99999,-99999}; cmd->ClearRenderTargetView(handle,clear,0,nullptr);
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{}; pd.pRootSignature=root.Get();
        pd.VS={vs->GetBufferPointer(),vs->GetBufferSize()}; pd.PS={ps->GetBufferPointer(),ps->GetBufferSize()};
        if (argc == 13) {
            const auto bytes = std::filesystem::file_size(argv[12]);
            if (bytes < 32 || bytes > 16 * 1024 * 1024) throw std::runtime_error("invalid GS DXIL size");
            geometry_bytecode.resize(bytes);
            std::ifstream geometry(argv[12], std::ios::binary);
            geometry.read(geometry_bytecode.data(), bytes);
            if (!geometry) throw std::runtime_error("incomplete GS DXIL");
            pd.GS = {geometry_bytecode.data(), geometry_bytecode.size()};
        }
        if (captured_vs || fixture) {
            pd.VS = {captured_bytecode.data(), captured_bytecode.size()};
            pd.PS = {captured_pixel_bytecode.data(), captured_pixel_bytecode.size()};
        }
        D3D12_INPUT_ELEMENT_DESC captured_layout = layout[1]; captured_layout.AlignedByteOffset = 0;
        pd.InputLayout={layout,2};
        const D3D12_INPUT_ELEMENT_DESC fixture_layout[] = {
            {"TEXCOORD",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,0,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
            {"TEXCOORD",1,DXGI_FORMAT_R32G32B32A32_FLOAT,0,16,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0}};
        if (fixture) pd.InputLayout = {fixture_layout, 2};
        if (captured_vs) pd.InputLayout = {&captured_layout, 1}; pd.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode=D3D12_CULL_MODE_FRONT; pd.RasterizerState.FrontCounterClockwise=TRUE;
        if (fixture) pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable=TRUE; pd.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_ALWAYS;
        pd.DepthStencilState.FrontFace={D3D12_STENCIL_OP_KEEP,D3D12_STENCIL_OP_KEEP,D3D12_STENCIL_OP_KEEP,D3D12_COMPARISON_FUNC_ALWAYS};
        pd.DepthStencilState.BackFace=pd.DepthStencilState.FrontFace;
        auto& blend=pd.BlendState.RenderTarget[0]; blend.SrcBlend=blend.SrcBlendAlpha=D3D12_BLEND_ONE;
        blend.DestBlend=blend.DestBlendAlpha=D3D12_BLEND_ZERO; blend.BlendOp=blend.BlendOpAlpha=D3D12_BLEND_OP_ADD;
        blend.RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL; pd.SampleMask=UINT_MAX;
        pd.SampleDesc.Count=pd.NumRenderTargets=1; pd.RTVFormats[0]=td.Format;
        pd.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        const HRESULT pipeline_hr = device->CreateGraphicsPipelineState(&pd,IID_PPV_ARGS(&psos[i]));
        if (FAILED(pipeline_hr)) {
            for (UINT64 m = 0; m < messages->GetNumStoredMessages(); ++m) {
                SIZE_T bytes{}; messages->GetMessage(m, nullptr, &bytes);
                std::vector<char> data(bytes);
                auto* message = reinterpret_cast<D3D12_MESSAGE*>(data.data());
                if (SUCCEEDED(messages->GetMessage(m, message, &bytes)))
                    std::cerr << message->pDescription << '\n';
            }
        }
        Check(pipeline_hr);
        cmd->SetPipelineState(psos[i].Get()); cmd->OMSetRenderTargets(1,&handle,FALSE,nullptr);
        cmd->DrawInstanced(static_cast<UINT>(size/32),1,0,0);
        D3D12_RESOURCE_BARRIER barrier{}; barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition={targets[i].Get(),0,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE};
        cmd->ResourceBarrier(1,&barrier);
        D3D12_TEXTURE_COPY_LOCATION from{targets[i].Get(),D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        D3D12_TEXTURE_COPY_LOCATION to{downloads[i].Get(),D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        to.PlacedFootprint=footprints[i]; cmd->CopyTextureRegion(&to,0,0,0,&from,nullptr);
        auto roi_desc=td;roi_desc.Width=256;roi_desc.Height=128;
        UINT64 roi_bytes{};device->GetCopyableFootprints(&roi_desc,0,1,0,&roi_footprints[i],nullptr,nullptr,&roi_bytes);
        roi_downloads[i]=buffer(roi_bytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION roi_to{roi_downloads[i].Get(),D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        roi_to.PlacedFootprint=roi_footprints[i];
        const D3D12_BOX box{972,506,0,1228,634,1};
        cmd->CopyTextureRegion(&roi_to,0,0,0,&from,&box);
    }
    Check(cmd->Close()); ID3D12CommandList* lists[]{cmd.Get()}; queue->ExecuteCommandLists(1,lists);
    ComPtr<ID3D12Fence> fence; Check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));
    Check(queue->Signal(fence.Get(),1)); HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    if (!event) throw std::runtime_error("fence event creation failed");
    Check(fence->SetEventOnCompletion(1,event)); auto wait=WaitForSingleObject(event,30000); CloseHandle(event);
    if (wait!=WAIT_OBJECT_0) throw std::runtime_error("clip probe timeout");
    std::filesystem::create_directories(argv[2]);
    for (unsigned i=0;i<target_count;++i) {
        Check(downloads[i]->Map(0,nullptr,&mapped)); UINT covered=0, valid=0; double max_error=0;
        std::ofstream out(std::filesystem::path(argv[2])/(i?"emulated.raw":"native.raw"),std::ios::binary);
        for (UINT y=0;y<H;++y) {
            const auto* row=reinterpret_cast<const float*>(static_cast<char*>(mapped)+y*footprints[i].Footprint.RowPitch);
            out.write(reinterpret_cast<const char*>(row),W*16);
            for (UINT x=0;x<W;++x) if (fixture ? row[x*4] != -99999
                                             : row[x*4+(captured_ps ? 3 : 0)]>-90000) {
                ++covered; const double err=std::max(std::abs(row[x*4]-(x+.5)/W),std::abs(row[x*4+1]-(y+.5)/H));
                if (!captured_ps && !fixture) { valid+=err<.0001; max_error=std::max(max_error,err); }
            }
        }
        void* roi_mapped{};Check(roi_downloads[i]->Map(0,nullptr,&roi_mapped));
        std::ofstream roi_out(std::filesystem::path(argv[2])/(i?"emulated-roi.raw":"native-roi.raw"),std::ios::binary);
        for (unsigned row=0;row<128;++row) {
            const auto* roi_row=static_cast<char*>(roi_mapped)+row*roi_footprints[i].Footprint.RowPitch;
            const auto* full=static_cast<char*>(mapped)+(row+506)*footprints[i].Footprint.RowPitch+972*16;
            if (std::memcmp(roi_row,full,256*16)) throw std::runtime_error("ROI GPU copy differs from full readback");
            roi_out.write(roi_row,256*16);
        }
        roi_downloads[i]->Unmap(0,&empty);
        if (!roi_out) throw std::runtime_error("ROI output write failed");
        downloads[i]->Unmap(0,&empty);
        std::cout<<(i?"emulated":"native")<<" covered="<<covered;
        if (!captured_ps && !fixture) std::cout<<" affine="<<valid<<" max_error="<<max_error;
        std::cout<<'\n';
        if (!out) throw std::runtime_error("clip output write failed");
    }
    Check(device->GetDeviceRemovedReason());
    for (UINT64 i=0;i<messages->GetNumStoredMessagesAllowedByRetrievalFilter();++i) {
        SIZE_T bytes{}; messages->GetMessage(i,nullptr,&bytes); std::vector<char> data(bytes);
        auto* message=reinterpret_cast<D3D12_MESSAGE*>(data.data()); Check(messages->GetMessage(i,message,&bytes));
        if (message->Severity<=D3D12_MESSAGE_SEVERITY_WARNING) throw std::runtime_error(message->pDescription);
    }
    std::cout<<"PASS native readback; debug clean (numerical comparison is separate)\n"; return 0;
} catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
