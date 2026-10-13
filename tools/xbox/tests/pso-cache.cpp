// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>
#include <barrier>
#include <cassert>
#include <chrono>
#include <cstring>
#include <future>
#include <iostream>
#include <thread>
#include <d3d12sdklayers.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include "video_core/renderer_d3d12/d3d12_live_pipeline_cache.h"
#include "video_core/renderer_d3d12/d3d12_pso_key.h"

using namespace D3D12;
using Microsoft::WRL::ComPtr;
static void Check(HRESULT result) { assert(SUCCEEDED(result)); }
struct Collision { size_t operator()(const GraphicsPsoKey&) const { return 0; } };

static void Identity() {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    const auto original = MakeGraphicsPsoKey(desc, {});
    size_t cases{};
    const auto different = [&](auto change) {
        auto altered = desc;
        change(altered);
        assert(!(original == MakeGraphicsPsoKey(altered, {})));
        ++cases;
    };
    different([](auto& d) { d.pRootSignature = reinterpret_cast<ID3D12RootSignature*>(1); });
    different([](auto& d) { d.BlendState.AlphaToCoverageEnable = TRUE; });
    different([](auto& d) { d.BlendState.IndependentBlendEnable = TRUE; });
    for (int i = 0; i < 8; ++i) {
        different([&](auto& d) { d.RTVFormats[i] = DXGI_FORMAT_R8G8B8A8_UNORM; });
        different([&](auto& d) { d.BlendState.RenderTarget[i].RenderTargetWriteMask = 1; });
        different([&](auto& d) { d.BlendState.RenderTarget[i].SrcBlend = D3D12_BLEND_ONE; });
        different([&](auto& d) { d.BlendState.RenderTarget[i].BlendOp = D3D12_BLEND_OP_ADD; });
        different([&](auto& d) { d.BlendState.RenderTarget[i].LogicOpEnable = TRUE; });
    }
    different([](auto& d) { d.SampleMask = 1; });
    different([](auto& d) { d.SampleDesc.Count = 4; });
    different([](auto& d) { d.SampleDesc.Quality = 1; });
    different([](auto& d) { d.RasterizerState.DepthBias = -1; });
    different([](auto& d) { d.RasterizerState.DepthBiasClamp = 0.5f; });
    different([](auto& d) { d.RasterizerState.SlopeScaledDepthBias = -0.5f; });
    different([](auto& d) { d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; });
    different([](auto& d) { d.RasterizerState.CullMode = D3D12_CULL_MODE_BACK; });
    different([](auto& d) { d.RasterizerState.DepthClipEnable = TRUE; });
    different([](auto& d) { d.DepthStencilState.DepthEnable = TRUE; });
    different([](auto& d) { d.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL; });
    different([](auto& d) { d.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS; });
    different([](auto& d) { d.DepthStencilState.StencilReadMask = 255; });
    different([](auto& d) { d.DepthStencilState.StencilWriteMask = 255; });
    different([](auto& d) { d.DepthStencilState.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP; });
    different([](auto& d) { d.DepthStencilState.BackFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS; });
    different([](auto& d) { d.NumRenderTargets = 1; });
    different([](auto& d) { d.DSVFormat = DXGI_FORMAT_D32_FLOAT; });
    different([](auto& d) { d.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF; });
    different([](auto& d) { d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE; });
    different([](auto& d) { d.NodeMask = 1; });
    different([](auto& d) { d.Flags = D3D12_PIPELINE_STATE_FLAG_TOOL_DEBUG; });
    std::array<std::uint8_t, 4> bytes{1, 2, 3, 4}, same = bytes;
    desc.VS = {bytes.data(), bytes.size()};
    auto equal = desc;
    equal.VS = {same.data(), same.size()};
    assert(MakeGraphicsPsoKey(desc, {}) == MakeGraphicsPsoKey(equal, {}));
    same[3] = 9;
    assert(!(MakeGraphicsPsoKey(desc, {}) == MakeGraphicsPsoKey(equal, {})));
    const char first[] = "TEXCOORD", second[] = "TEXCOORD";
    D3D12_INPUT_ELEMENT_DESC a{first, 0, DXGI_FORMAT_R32_FLOAT, 0, 0,
                               D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0};
    auto b = a;
    b.SemanticName = second;
    desc.InputLayout = {&a, 1}; equal = desc; equal.InputLayout = {&b, 1};
    assert(MakeGraphicsPsoKey(desc, {}) == MakeGraphicsPsoKey(equal, {}));
    for (int field = 0; field < 6; ++field) {
        b = a;
        switch (field) {
        case 0: b.SemanticName = "POSITION"; break;
        case 1: b.SemanticIndex = 1; break;
        case 2: b.InputSlot = 1; break;
        case 3: b.AlignedByteOffset = 4; break;
        case 4: b.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA; break;
        case 5: b.InstanceDataStepRate = 1; break;
        }
        assert(!(MakeGraphicsPsoKey(desc, {}) == MakeGraphicsPsoKey(equal, {})));
    }
    std::cout << "Identity: " << cases + 8 << " mutations/relocated bytecodes and semantic strings PASS\n";
}

static void CheckConcurrency() {
    using Cache = LivePipelineCache<GraphicsPsoKey, int, Collision>;
    Cache cache;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    auto key = MakeGraphicsPsoKey(desc, {});
    std::atomic<int> builds{};
    std::barrier gate{16};
    std::array<std::shared_ptr<const Cache::Entry>, 16> results;
    std::vector<std::jthread> threads;
    for (size_t i = 0; i < results.size(); ++i) {
        threads.emplace_back([&, i] {
            gate.arrive_and_wait();
            results[i] = cache.Get(key, [&] { ++builds; return 17; }).entry;
        });
    }
    threads.clear();
    assert(builds == 1);
    for (const auto& result : results) { assert(result == results[0] && *result->value == 17); }
    desc.SampleMask = 1;
    auto other = cache.Get(MakeGraphicsPsoKey(desc, {}), [] { return 22; }).entry;
    assert(other != results[0] && *other->value == 22); // Forced hash collision, exact comparison.
    std::weak_ptr<const Cache::Entry> lifetime = results[0];
    for (auto& result : results) { result.reset(); }
    assert(lifetime.expired());
    assert(cache.Get(key, [&] { ++builds; return 18; }).outcome == Cache::Outcome::Created);
    assert(builds == 2);
    Cache failed;
    std::promise<void> started, release;
    auto released = release.get_future().share();
    std::atomic<int> exceptions{}, failed_builds{};
    std::jthread owner([&] {
        try { failed.Get(key, [&]() -> int {
            ++failed_builds; started.set_value(); released.wait(); throw std::runtime_error("test");
        }); } catch (const std::runtime_error&) { ++exceptions; }
    });
    started.get_future().wait();
    std::jthread waiter([&] {
        try { failed.Get(key, [&]() -> int { ++failed_builds; throw std::runtime_error("test"); }); }
        catch (const std::runtime_error&) { ++exceptions; }
    });
    release.set_value(); owner.join(); waiter.join();
    assert(exceptions == 2); // Waiter may enter after failed entry was removed: both paths safe.
    assert(*failed.Get(key, [] { return 99; }).entry->value == 99);
    Cache bounded{1};
    auto kept = bounded.Get(key, [] { return 1; }).entry;
    assert(bounded.Get(MakeGraphicsPsoKey(desc, {}), [] { return 2; }).outcome == Cache::Outcome::Bypassed);
    assert(bounded.Get(key, [] { return 3; }).entry == kept);
    Cache disabled{0};
    assert(disabled.Get(key, [] { return 1; }).outcome == Cache::Outcome::Bypassed);
    // Different keys compile concurrently; no index lock is held by a compiler.
    Cache parallel;
    std::barrier builders{2};
    std::jthread one([&] { parallel.Get(key, [&] { builders.arrive_and_wait(); return 1; }); });
    std::jthread two([&] { parallel.Get(MakeGraphicsPsoKey(desc, {}), [&] {
        builders.arrive_and_wait(); return 2;
    }); });
    one.join(); two.join();
    for (int i = 0; i < 10000; ++i) {
        desc.SampleMask = i;
        assert(*bounded.Get(MakeGraphicsPsoKey(desc, {}), [&] { return i; }).entry->value == i || i == 0);
    }
    std::cout << "Concurrency: 16 identical, collisions, distinct parallel, retry, cap, weak retirement/churn PASS\n";
}

static void Gpu() {
    ComPtr<ID3D12Debug> debug;
    Check(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))); debug->EnableDebugLayer();
    ComPtr<ID3D12Device> device;
    Check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
    ComPtr<ID3D12InfoQueue> info; Check(device.As(&info));
    D3D12_FEATURE_DATA_SHADER_CACHE support{};
    const HRESULT support_hr = device->CheckFeatureSupport(D3D12_FEATURE_SHADER_CACHE, &support, sizeof(support));
    std::cout << "Desktop shader cache support HRESULT=" << std::hex << support_hr
              << " flags=" << support.SupportFlags << std::dec << " (not Series certification)\n";
    ComPtr<ID3DBlob> signature, errors;
    D3D12_ROOT_SIGNATURE_DESC root_desc{};
    root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    Check(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &errors));
    ComPtr<ID3D12RootSignature> root;
    Check(device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&root)));
    const char source[] = "float4 vs(uint v:SV_VertexID):SV_Position {"
        "return float4(v==1?3:-1,v==2?3:-1,0,1);}"
        "float4 ps():SV_Target {return float4(1,0,0,1);}";
    ComPtr<ID3DBlob> vs, ps;
    Check(D3DCompile(source, sizeof(source), nullptr, nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vs, &errors));
    Check(D3DCompile(source, sizeof(source), nullptr, nullptr, nullptr, "ps", "ps_5_0", 0, 0, &ps, &errors));
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = root.Get();
    desc.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    desc.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = UINT_MAX; desc.SampleDesc.Count = 1;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1; desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    using Cache = LivePipelineCache<GraphicsPsoKey, ComPtr<ID3D12PipelineState>, GraphicsPsoKeyHash>;
    Cache cache;
    int creates{};
    const auto create = [&] {
        ++creates; ComPtr<ID3D12PipelineState> pso;
        Check(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso))); return pso;
    };
    auto resident = cache.Get(MakeGraphicsPsoKey(desc, {}), create).entry;
    const auto start_old = std::chrono::steady_clock::now();
    for (int i = 0; i < 300; ++i) { auto pso = create(); }
    const double old_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start_old).count();
    const auto start_new = std::chrono::steady_clock::now();
    for (int i = 0; i < 300; ++i) {
        const auto result = cache.Get(MakeGraphicsPsoKey(desc, {}), create);
        assert(result.entry == resident && result.outcome == Cache::Outcome::Hit);
    }
    const double new_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start_new).count();
    assert(creates == 301);
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    auto distinct = cache.Get(MakeGraphicsPsoKey(desc, {}), create).entry;
    assert(distinct != resident && distinct->value->Get() != resident->value->Get());
    // Actual draws, readback and production GraphicsPipeline lifetime are exercised by
    // run-draw-targets.ps1 alongside this cache/key gate.
    for (UINT64 i = 0; i < info->GetNumStoredMessages(); ++i) {
        SIZE_T size{}; info->GetMessage(i, nullptr, &size);
        std::vector<std::uint8_t> bytes(size);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(bytes.data());
        Check(info->GetMessage(i, message, &size));
        if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
            std::cerr << message->pDescription << '\n'; assert(false);
        }
    }
    std::cout << "GPU: 300 equivalent requests, driver calls 300 -> 0; "
              << old_ms << " -> " << new_ms << " ms; distinct PSO/debug PASS\n";
}

int main() { Identity(); CheckConcurrency(); Gpu(); }
