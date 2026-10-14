// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
// Isolates RTV conversion: the pixel shader only copies exact float32 CB values.
#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <DirectXPackedVector.h>
#include <wrl/client.h>
#include <array>
#include <bit>
#include <cstring>
#include <climits>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>
#include "video_core/renderer_d3d12/diagnostics/d3d12_material_trace.h"
using Microsoft::WRL::ComPtr;
static void Check(HRESULT hr) { if (FAILED(hr)) throw std::runtime_error("D3D12 probe failure"); }
static unsigned HalfRtz(float value) {
    unsigned bits = std::bit_cast<unsigned>(value), exp = (bits >> 23) & 255;
    if (exp < 113 || exp > 142 || bits >> 31) throw std::runtime_error("normal positive float required");
    return ((exp - 112) << 10) | ((bits >> 13) & 1023);
}
static unsigned SmallRtz(float value, unsigned mantissa) {
    const unsigned bits = std::bit_cast<unsigned>(value), exp = (bits >> 23) & 255;
    if (value == 0) return 0;
    if (exp < 113 || exp > 142 || bits >> 31) throw std::runtime_error("normal positive float required");
    return ((exp - 112) << mantissa) | ((bits >> (23 - mantissa)) & ((1u << mantissa) - 1));
}

static void CheckBlockCopies(ID3D12Device* device, ID3D12CommandQueue* queue) {
    unsigned cases = 0;
    for (const auto format : {DXGI_FORMAT_BC1_UNORM_SRGB, DXGI_FORMAT_BC4_UNORM,
                              DXGI_FORMAT_BC5_SNORM, DXGI_FORMAT_BC7_UNORM}) {
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = 16; desc.Height = 8; desc.DepthOrArraySize = 2; desc.MipLevels = 4;
        desc.Format = format; desc.SampleDesc.Count = 1;
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        ComPtr<ID3D12Resource> texture;
        Check(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture)));
        std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, 8> footprints{};
        std::array<UINT, 8> rows{};
        std::array<UINT64, 8> row_bytes{};
        UINT64 total{};
        device->GetCopyableFootprints(&desc, 0, 8, 0, footprints.data(), rows.data(), row_bytes.data(), &total);
        const auto make_buffer = [&](D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state) {
            auto d = desc; d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            d.Width = total; d.Height = d.DepthOrArraySize = d.MipLevels = 1;
            d.Format = DXGI_FORMAT_UNKNOWN; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            D3D12_HEAP_PROPERTIES props{}; props.Type = heap;
            ComPtr<ID3D12Resource> result;
            Check(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &d, state,
                nullptr, IID_PPV_ARGS(&result)));
            return result;
        };
        auto upload = make_buffer(D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        auto download = make_buffer(D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        std::vector<unsigned char> expected(static_cast<size_t>(total), 0xcd);
        for (unsigned s = 0; s < 8; ++s) {
            const unsigned mip = s % 4;
            const auto w = (16u >> mip), h = (8u >> mip);
            const auto bytes = D3D12::MaterialBlockBytes(format);
            if (row_bytes[s] != ((w + 3) / 4) * bytes || rows[s] != (h + 3) / 4)
                throw std::runtime_error("BC footprint block dimensions mismatch");
            for (unsigned r = 0; r < rows[s]; ++r)
                for (unsigned b = 0; b < row_bytes[s]; ++b)
                    expected[footprints[s].Offset + size_t{r} * footprints[s].Footprint.RowPitch + b] =
                        static_cast<unsigned char>(s * 37 + r * 19 + b);
        }
        void* mapped{}; D3D12_RANGE empty{};
        Check(upload->Map(0, &empty, &mapped)); std::memcpy(mapped, expected.data(), expected.size());
        upload->Unmap(0, nullptr);
        ComPtr<ID3D12CommandAllocator> allocator;
        Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
        ComPtr<ID3D12GraphicsCommandList> cmd;
        Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&cmd)));
        for (unsigned s = 0; s < 8; ++s) {
            D3D12_TEXTURE_COPY_LOCATION src{upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
            src.PlacedFootprint = footprints[s];
            D3D12_TEXTURE_COPY_LOCATION dst{texture.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
            dst.SubresourceIndex = s;
            cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }
        D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = {texture.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE};
        cmd->ResourceBarrier(1, &barrier);
        for (unsigned s = 0; s < 8; ++s) {
            D3D12_TEXTURE_COPY_LOCATION src{texture.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
            src.SubresourceIndex = s;
            D3D12_TEXTURE_COPY_LOCATION dst{download.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
            dst.PlacedFootprint = footprints[s];
            cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }
        Check(cmd->Close()); ID3D12CommandList* lists[]{cmd.Get()}; queue->ExecuteCommandLists(1, lists);
        ComPtr<ID3D12Fence> fence; Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
        Check(queue->Signal(fence.Get(), 1)); HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event) throw std::runtime_error("BC fence event failed");
        Check(fence->SetEventOnCompletion(1, event)); const auto wait = WaitForSingleObject(event, 30000);
        CloseHandle(event); if (wait != WAIT_OBJECT_0) throw std::runtime_error("BC copy timeout");
        Check(download->Map(0, nullptr, &mapped));
        for (unsigned s = 0; s < 8; ++s) {
            for (unsigned r = 0; r < rows[s]; ++r) {
                const auto offset = footprints[s].Offset + size_t{r} * footprints[s].Footprint.RowPitch;
                if (std::memcmp(static_cast<unsigned char*>(mapped) + offset,
                    expected.data() + offset, static_cast<size_t>(row_bytes[s])))
                    throw std::runtime_error("BC native block row mismatch");
            }
            ++cases;
        }
        download->Unmap(0, &empty);
    }
    std::cout << "PASS native BC block copies: " << cases << " mip/layer subresources, including <4px mips\n";
}
int main(int argc, char** argv) try {
    if (argc != 3) throw std::runtime_error("usage: float-store FLOAT4_INPUT OUTPUT_DIRECTORY");
    const auto bytes = std::filesystem::file_size(argv[1]);
    if (!bytes || bytes % 16 || bytes > 16384) throw std::runtime_error("invalid input size");
    const unsigned count = static_cast<unsigned>(bytes / 16);
    std::vector<std::array<float, 4>> values(1024);
    std::ifstream input(argv[1], std::ios::binary);
    input.read(reinterpret_cast<char*>(values.data()), bytes);
    if (!input) throw std::runtime_error("input read failed");
    ComPtr<ID3D12Debug> debug; Check(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))); debug->EnableDebugLayer();
    ComPtr<ID3D12Device> device; Check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
    ComPtr<IDXGIFactory4> factory; Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter1> adapter; Check(factory->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&adapter)));
    DXGI_ADAPTER_DESC1 adapter_desc{}; Check(adapter->GetDesc1(&adapter_desc));
    std::wcout << L"adapter: " << adapter_desc.Description << L'\n';
    ComPtr<ID3D12InfoQueue> messages; Check(device.As(&messages));
    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    ComPtr<ID3D12CommandQueue> queue; Check(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
    ComPtr<ID3D12CommandAllocator> allocator;
    Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
    ComPtr<ID3D12GraphicsCommandList> cmd;
    Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&cmd)));
    const auto buffer = [&](UINT64 size, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state) {
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = heap;
        D3D12_RESOURCE_DESC d{}; d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = size;
        d.Height = d.DepthOrArraySize = d.MipLevels = d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> result;
        Check(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&result)));
        return result;
    };
    auto upload = buffer(16384, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    void* mapped{}; D3D12_RANGE empty{}; Check(upload->Map(0, &empty, &mapped));
    std::memcpy(mapped, values.data(), 16384); upload->Unmap(0, nullptr);
    D3D12_ROOT_PARAMETER parameter{}; parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC root_desc{1, &parameter};
    ComPtr<ID3DBlob> root_blob, errors;
    Check(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &root_blob, &errors));
    ComPtr<ID3D12RootSignature> root;
    Check(device->CreateRootSignature(0, root_blob->GetBufferPointer(), root_blob->GetBufferSize(), IID_PPV_ARGS(&root)));
    const char shader[] =
        "cbuffer Input : register(b0) { float4 v[1024]; };"
        "float4 VS(uint id:SV_VertexID):SV_Position {"
        "return float4(id==2?3:-1,id==1?3:-1,0,1); }"
        "float4 PS(float4 p:SV_Position):SV_Target { return v[(uint)p.x]; }";
    ComPtr<ID3DBlob> vs, ps;
    Check(D3DCompile(shader, sizeof(shader), nullptr, nullptr, nullptr, "VS", "vs_5_0", 0, 0, &vs, &errors));
    Check(D3DCompile(shader, sizeof(shader), nullptr, nullptr, nullptr, "PS", "ps_5_0", 0, 0, &ps, &errors));
    D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2};
    ComPtr<ID3D12DescriptorHeap> rtv; Check(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtv)));
    std::array<ComPtr<ID3D12Resource>, 2> textures, readbacks;
    std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, 2> footprints{};
    std::array<ComPtr<ID3D12PipelineState>, 2> pipelines;
    const std::array formats{DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R11G11B10_FLOAT};
    cmd->SetGraphicsRootSignature(root.Get()); cmd->SetGraphicsRootConstantBufferView(0, upload->GetGPUVirtualAddress());
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const D3D12_VIEWPORT vp{0, 0, static_cast<float>(count), 1, 0, 1}; cmd->RSSetViewports(1, &vp);
    const D3D12_RECT sc{0, 0, static_cast<LONG>(count), 1}; cmd->RSSetScissorRects(1, &sc);
    for (unsigned i = 0; i < 2; ++i) {
        D3D12_RESOURCE_DESC d{}; d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = count;
        d.Height = d.DepthOrArraySize = d.MipLevels = d.SampleDesc.Count = 1;
        d.Format = formats[i]; d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        Check(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_RENDER_TARGET,
            nullptr, IID_PPV_ARGS(&textures[i])));
        UINT64 total{}; device->GetCopyableFootprints(&d, 0, 1, 0, &footprints[i], nullptr, nullptr, &total);
        readbacks[i] = buffer(total, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        auto handle = rtv->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += i * device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        device->CreateRenderTargetView(textures[i].Get(), nullptr, handle);
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{}; pso.pRootSignature = root.Get();
        pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()}; pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
        pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pso.RasterizerState.DepthClipEnable = TRUE;
        pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        pso.DepthStencilState.FrontFace = {D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP,
            D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS};
        pso.DepthStencilState.BackFace = pso.DepthStencilState.FrontFace;
        auto& blend = pso.BlendState.RenderTarget[0];
        blend.SrcBlend = blend.SrcBlendAlpha = D3D12_BLEND_ONE;
        blend.DestBlend = blend.DestBlendAlpha = D3D12_BLEND_ZERO;
        blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pso.SampleMask = UINT_MAX; pso.SampleDesc.Count = 1; pso.NumRenderTargets = 1;
        pso.RTVFormats[0] = formats[i]; pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        Check(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&pipelines[i])));
        cmd->SetPipelineState(pipelines[i].Get()); cmd->OMSetRenderTargets(1, &handle, FALSE, nullptr);
        cmd->DrawInstanced(3, 1, 0, 0);
        D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = {textures[i].Get(), 0, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE};
        cmd->ResourceBarrier(1, &barrier);
        D3D12_TEXTURE_COPY_LOCATION src{textures[i].Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        D3D12_TEXTURE_COPY_LOCATION dst{readbacks[i].Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        dst.PlacedFootprint = footprints[i]; cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    Check(cmd->Close()); ID3D12CommandList* lists[]{cmd.Get()}; queue->ExecuteCommandLists(1, lists);
    ComPtr<ID3D12Fence> fence; Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    Check(queue->Signal(fence.Get(), 1)); HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    Check(fence->SetEventOnCompletion(1, event)); const auto wait = WaitForSingleObject(event, 30000); CloseHandle(event);
    if (wait != WAIT_OBJECT_0) throw std::runtime_error("GPU timeout");
    std::filesystem::create_directories(argv[2]);
    for (unsigned i = 0; i < 2; ++i) {
        Check(readbacks[i]->Map(0, nullptr, &mapped));
        auto* actual = static_cast<const unsigned*>(mapped); unsigned rne = 0, rtz = 0;
        for (unsigned x = 0; x < count; ++x) {
            const auto& v = values[x]; unsigned nearest, truncated;
            if (i == 0) {
                nearest = DirectX::PackedVector::XMConvertFloatToHalf(v[0]) |
                    (unsigned{DirectX::PackedVector::XMConvertFloatToHalf(v[1])} << 16);
                truncated = HalfRtz(v[0]) | (HalfRtz(v[1]) << 16);
            } else {
                DirectX::PackedVector::XMFLOAT3PK packed;
                DirectX::PackedVector::XMStoreFloat3PK(&packed, DirectX::XMVectorSet(v[0], v[1], v[2], 1));
                nearest = packed.v;
                truncated = SmallRtz(v[0], 6) | (SmallRtz(v[1], 6) << 11) | (SmallRtz(v[2], 5) << 22);
            }
            rne += actual[x] == nearest; rtz += actual[x] == truncated;
        }
        const auto name = i == 0 ? "rg16" : "r11g11b10";
        std::ofstream out(std::filesystem::path(argv[2]) / (std::string(name) + ".raw"), std::ios::binary);
        out.write(static_cast<const char*>(mapped), count * 4); readbacks[i]->Unmap(0, nullptr);
        std::cout << name << " samples=" << count << " RNE=" << rne << " RTZ=" << rtz << '\n';
    }
    CheckBlockCopies(device.Get(), queue.Get());
    for (UINT64 i = 0; i < messages->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
        SIZE_T size{}; messages->GetMessage(i, nullptr, &size); std::vector<char> bytes(size);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(bytes.data()); Check(messages->GetMessage(i, message, &size));
        if (message->Severity <= D3D12_MESSAGE_SEVERITY_WARNING) {
            std::cerr << message->pDescription << '\n'; throw std::runtime_error("debug layer message");
        }
    }
    Check(device->GetDeviceRemovedReason()); std::cout << "PASS native RTV-only conversion; debug clean\n";
    return 0;
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
