// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Desktop-only GPU probe: actual dumped VS bytecodes, all outputs kept live by stream output.
#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <wrl/client.h>
#include <array>
#include <bit>
#include <cstring>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>
using Microsoft::WRL::ComPtr;
static void Check(HRESULT hr) { if (FAILED(hr)) throw std::runtime_error("HRESULT " + std::to_string(static_cast<unsigned>(hr))); }
static std::vector<char> Read(const char* path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) throw std::runtime_error("missing bytecode");
    std::vector<char> bytes(static_cast<size_t>(f.tellg()));
    f.seekg(0); f.read(bytes.data(), bytes.size());
    if (!f) throw std::runtime_error("short bytecode");
    return bytes;
}
int main(int argc, char** argv) try {
    if (argc != 3) throw std::runtime_error("usage: multipass-position prepass.dxil color.dxil");
    ComPtr<ID3D12Debug> debug; Check(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))); debug->EnableDebugLayer();
    ComPtr<ID3D12Device> device; Check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
    ComPtr<ID3D12InfoQueue> messages; Check(device.As(&messages));
    ComPtr<ID3D12CommandQueue> queue;
    D3D12_COMMAND_QUEUE_DESC q{}; Check(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)));
    ComPtr<ID3D12CommandAllocator> allocator; Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
    ComPtr<ID3D12GraphicsCommandList> commands; Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&commands)));
    auto buffer = [&](UINT64 bytes, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state) {
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = heap;
        D3D12_RESOURCE_DESC rd{}; rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width = bytes;
        rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> resource; Check(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr, IID_PPV_ARGS(&resource)));
        return resource;
    };
    auto transition = [&](ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after}; commands->ResourceBarrier(1, &b);
    };
    std::array<D3D12_ROOT_PARAMETER, 5> parameters{};
    for (UINT i = 0; i < 5; ++i) {
        parameters[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        parameters[i].Descriptor = {i == 4 ? 0U : i, i == 4 ? 31U : 0U};
    }
    D3D12_ROOT_SIGNATURE_DESC root_desc{5, parameters.data(), 0, nullptr,
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT | D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT};
    ComPtr<ID3DBlob> root_blob, root_error;
    Check(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &root_blob, &root_error));
    ComPtr<ID3D12RootSignature> root; Check(device->CreateRootSignature(0, root_blob->GetBufferPointer(), root_blob->GetBufferSize(), IID_PPV_ARGS(&root)));
    constexpr UINT BATCH = 16384, BATCHES = 4, COUNT = BATCH * BATCHES;
    constexpr UINT VERTEX_STRIDE = 96, CB_SIZE = 65536, CB_BATCH = 5 * CB_SIZE;
    constexpr UINT VERTEX_BASE = BATCHES * CB_BATCH;
    auto upload = buffer(VERTEX_BASE + UINT64(COUNT) * VERTEX_STRIDE, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    void* data{}; D3D12_RANGE no_read{0, 0}; Check(upload->Map(0, &no_read, &data));
    std::memset(data, 0, static_cast<size_t>(upload->GetDesc().Width));
    std::mt19937 rng{0x513abc}; std::uniform_real_distribution<float> f{-1.0f, 1.0f};
    for (UINT batch = 0; batch < BATCHES; ++batch) {
        // Matrices/constants vary across batches; no undefined loads or unbound resources.
        auto* constants = reinterpret_cast<float*>(static_cast<char*>(data) + batch * CB_BATCH);
        for (UINT i = 0; i < 4 * CB_SIZE / 4; ++i) constants[i] = f(rng);
        auto* runtime = reinterpret_cast<UINT*>(static_cast<char*>(data) + batch * CB_BATCH + 4 * CB_SIZE);
        runtime[3] = ((batch & 1) ? 1U : 0U) | ((batch & 2) ? 65536U : 0U);
    }
    for (UINT v = 0; v < COUNT; ++v) {
        auto* attributes = reinterpret_cast<float*>(static_cast<char*>(data) + VERTEX_BASE + v * VERTEX_STRIDE);
        for (UINT i = 0; i < 24; ++i) attributes[i] = f(rng) * (v % 3 == 0 ? 10000.0f : 10.0f);
        reinterpret_cast<UINT*>(attributes)[4] = rng();
    }
    const D3D12_VERTEX_BUFFER_VIEW vb{upload->GetGPUVirtualAddress() + VERTEX_BASE, COUNT * VERTEX_STRIDE, VERTEX_STRIDE};
    std::array<ComPtr<ID3D12Resource>, 2> outputs, readbacks;
    std::array<ComPtr<ID3D12PipelineState>, 2> pipelines;
    std::array<UINT, 2> strides{64, 160};
    std::array<D3D12_INPUT_ELEMENT_DESC, 6> ia{};
    const std::array<UINT, 6> semantics{0, 1, 2, 8, 9, 10};
    for (UINT i = 0; i < ia.size(); ++i) ia[i] = {"TEXCOORD", semantics[i], i == 1 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R32G32B32A32_FLOAT, 0, i * 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0};
    for (UINT pass = 0; pass < 2; ++pass) {
        const auto dxil = Read(argv[pass + 1]);
        std::vector<D3D12_SO_DECLARATION_ENTRY> so;
        for (UINT i = 0; i < strides[pass] / 16 - 1; ++i) so.push_back({0, "TEXCOORD", i, 0, 4, 0});
        so.push_back({0, "SV_Position", 0, 0, 4, 0});
        D3D12_GRAPHICS_PIPELINE_STATE_DESC p{}; p.pRootSignature = root.Get(); p.VS = {dxil.data(), dxil.size()};
        p.StreamOutput = {so.data(), static_cast<UINT>(so.size()), &strides[pass], 1, D3D12_SO_NO_RASTERIZED_STREAM};
        p.InputLayout = {ia.data(), pass == 0 ? 4U : 6U}; p.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
        p.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; p.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; p.RasterizerState.DepthClipEnable = TRUE;
        p.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        p.SampleMask = UINT_MAX; p.SampleDesc.Count = 1;
        p.BlendState.RenderTarget[0] = {FALSE, FALSE, D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD, D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD, D3D12_LOGIC_OP_NOOP, D3D12_COLOR_WRITE_ENABLE_ALL};
        Check(device->CreateGraphicsPipelineState(&p, IID_PPV_ARGS(&pipelines[pass])));
        const UINT64 bytes = UINT64(COUNT) * strides[pass];
        outputs[pass] = buffer(bytes + 8, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
        readbacks[pass] = buffer(bytes + 8, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        commands->CopyBufferRegion(outputs[pass].Get(), bytes, upload.Get(), 4 * CB_SIZE + 16, 8);
        transition(outputs[pass].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_STREAM_OUT);
        commands->SetPipelineState(pipelines[pass].Get()); commands->SetGraphicsRootSignature(root.Get());
        commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST); commands->IASetVertexBuffers(0, 1, &vb);
        const D3D12_STREAM_OUTPUT_BUFFER_VIEW view{outputs[pass]->GetGPUVirtualAddress(), bytes, outputs[pass]->GetGPUVirtualAddress() + bytes};
        commands->SOSetTargets(0, 1, &view);
        for (UINT batch = 0; batch < BATCHES; ++batch) {
            for (UINT cb = 0; cb < 5; ++cb) commands->SetGraphicsRootConstantBufferView(cb, upload->GetGPUVirtualAddress() + batch * CB_BATCH + cb * CB_SIZE);
            commands->DrawInstanced(BATCH, 1, batch * BATCH, 0);
        }
        transition(outputs[pass].Get(), D3D12_RESOURCE_STATE_STREAM_OUT, D3D12_RESOURCE_STATE_COPY_SOURCE);
        commands->CopyResource(readbacks[pass].Get(), outputs[pass].Get());
    }
    Check(commands->Close()); ID3D12CommandList* lists[]{commands.Get()}; queue->ExecuteCommandLists(1, lists);
    ComPtr<ID3D12Fence> fence; Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))); Check(queue->Signal(fence.Get(), 1));
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr); if (!event) throw std::runtime_error("event");
    Check(fence->SetEventOnCompletion(1, event)); const auto waited = WaitForSingleObject(event, 30000); CloseHandle(event);
    if (waited != WAIT_OBJECT_0) throw std::runtime_error("GPU timeout");
    std::array<void*, 2> mapped{};
    for (UINT pass = 0; pass < 2; ++pass) {
        Check(readbacks[pass]->Map(0, nullptr, &mapped[pass]));
        UINT64 filled{}; std::memcpy(&filled, static_cast<char*>(mapped[pass]) + UINT64(COUNT) * strides[pass], 8);
        if (filled != UINT64(COUNT) * strides[pass]) throw std::runtime_error("SO count mismatch");
    }
    UINT differing = 0;
    for (UINT vertex = 0; vertex < COUNT; ++vertex) {
        std::array<UINT, 4> a{}, b{};
        std::memcpy(a.data(), static_cast<char*>(mapped[0]) + (vertex + 1) * strides[0] - 16, 16);
        std::memcpy(b.data(), static_cast<char*>(mapped[1]) + (vertex + 1) * strides[1] - 16, 16);
        if (a != b) {
            if (differing < 8) std::cout << "DIFF vertex " << vertex << " z " << std::hex << a[2] << '/' << b[2] << " w " << a[3] << '/' << b[3] << std::dec << '\n';
            ++differing;
        }
    }
    UINT errors = 0;
    for (UINT64 i = 0; i < messages->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
        SIZE_T size{}; messages->GetMessage(i, nullptr, &size); std::vector<char> storage(size);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data()); Check(messages->GetMessage(i, message, &size));
        if (message->Severity <= D3D12_MESSAGE_SEVERITY_WARNING) { std::cerr << message->pDescription << '\n'; ++errors; }
    }
    Check(device->GetDeviceRemovedReason());
    std::cout << "GPU position probe: " << differing << '/' << COUNT << " vertices differ; debug warnings/errors=" << errors << '\n';
    return errors ? 2 : differing ? 1 : 0;
} catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 2; }
