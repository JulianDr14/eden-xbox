// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <stdexcept>

#include <fmt/format.h>

#include "common/logging.h"
#include "video_core/renderer_d3d12/d3d12_device.h"

namespace D3D12 {

namespace {

std::string Narrow(const wchar_t* wide) {
    std::string out;
    for (; *wide != L'\0'; ++wide) {
        out.push_back(*wide < 0x80 ? static_cast<char>(*wide) : '?');
    }
    return out;
}

const char* YesNo(BOOL value) {
    return value ? "yes" : "no";
}

template <typename T>
bool Query(ID3D12Device* device, D3D12_FEATURE feature, T& data) {
    return SUCCEEDED(device->CheckFeatureSupport(feature, &data, sizeof(data)));
}

} // Anonymous namespace

void ThrowIfFailed(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        throw std::runtime_error(
            fmt::format("D3D12: {} failed (HRESULT 0x{:08X})", what, static_cast<u32>(hr)));
    }
}

Device::Device() {
    ThrowIfFailed(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2");
    // Feature level 11.0 is all Xbox Series UWP offers; desktop GPUs accept it too.
    ThrowIfFailed(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)),
                  "D3D12CreateDevice");

    ComPtr<IDXGIAdapter1> found;
    if (SUCCEEDED(factory->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&found)))) {
        adapter = found;
        // QueryInterface, never a pointer cast: IDXGIAdapter3 adds vtable entries.
        if (FAILED(adapter.As(&adapter3))) {
            adapter3.Reset();
        }
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(adapter->GetDesc1(&desc))) {
            adapter_name = Narrow(desc.Description);
        }
    }
    if (adapter_name.empty()) {
        adapter_name = "Direct3D 12";
    }

    const D3D12_COMMAND_QUEUE_DESC queue_desc{
        .Type = D3D12_COMMAND_LIST_TYPE_DIRECT,
        .Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL,
        .Flags = D3D12_COMMAND_QUEUE_FLAG_NONE,
        .NodeMask = 0,
    };
    ThrowIfFailed(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)),
                  "CreateCommandQueue");
    ThrowIfFailed(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)),
                  "CreateFence");
    fence_event = CreateEventExW(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
    if (fence_event == nullptr) {
        throw std::runtime_error("D3D12: CreateEventExW failed");
    }

    LogCapabilities();
}

Device::~Device() {
    if (queue && fence) {
        WaitIdle();
    }
    if (fence_event != nullptr) {
        CloseHandle(fence_event);
    }
}

u64 Device::Signal() {
    const u64 value = next_fence_value++;
    ThrowIfFailed(queue->Signal(fence.Get(), value), "ID3D12CommandQueue::Signal");
    return value;
}

void Device::Wait(u64 value) {
    if (fence->GetCompletedValue() >= value) {
        return;
    }
    ThrowIfFailed(fence->SetEventOnCompletion(value, fence_event),
                  "ID3D12Fence::SetEventOnCompletion");
    WaitForSingleObjectEx(fence_event, INFINITE, FALSE);
}

void Device::WaitIdle() {
    Wait(Signal());
}

void Device::LogCapabilities() const {
    ID3D12Device* const dev = device.Get();
    LOG_INFO(Render, "D3D12: adapter '{}'", adapter_name);

    // Highest shader model: the query fails with E_INVALIDARG for models the runtime does not know,
    // so walk down from the newest this SDK names.
    D3D12_FEATURE_DATA_SHADER_MODEL shader_model{};
    for (int sm = D3D_HIGHEST_SHADER_MODEL; sm >= D3D_SHADER_MODEL_5_1; --sm) {
        shader_model.HighestShaderModel = static_cast<D3D_SHADER_MODEL>(sm);
        if (Query(dev, D3D12_FEATURE_SHADER_MODEL, shader_model)) {
            break;
        }
        shader_model.HighestShaderModel = static_cast<D3D_SHADER_MODEL>(0);
    }
    LOG_INFO(Render, "D3D12: highest shader model {}.{}",
             static_cast<int>(shader_model.HighestShaderModel) >> 4,
             static_cast<int>(shader_model.HighestShaderModel) & 0xF);

    D3D12_FEATURE_DATA_ROOT_SIGNATURE root_signature{D3D_ROOT_SIGNATURE_VERSION_1_2};
    while (!Query(dev, D3D12_FEATURE_ROOT_SIGNATURE, root_signature) &&
           root_signature.HighestVersion > D3D_ROOT_SIGNATURE_VERSION_1_0) {
        root_signature.HighestVersion =
            static_cast<D3D_ROOT_SIGNATURE_VERSION>(root_signature.HighestVersion - 1);
    }
    LOG_INFO(Render, "D3D12: root signature version 0x{:x}",
             static_cast<int>(root_signature.HighestVersion));

    D3D12_FEATURE_DATA_D3D12_OPTIONS o{};
    if (Query(dev, D3D12_FEATURE_D3D12_OPTIONS, o)) {
        LOG_INFO(Render,
                 "D3D12: binding tier {}, resource heap tier {}, tiled resources tier {}, "
                 "conservative raster tier {}, typed UAV load extra formats {}, ROVs {}, "
                 "logic op {}, VP/RT index without GS {}, PS stencil ref {}, fp64 {}",
                 static_cast<int>(o.ResourceBindingTier), static_cast<int>(o.ResourceHeapTier),
                 static_cast<int>(o.TiledResourcesTier),
                 static_cast<int>(o.ConservativeRasterizationTier),
                 YesNo(o.TypedUAVLoadAdditionalFormats), YesNo(o.ROVsSupported),
                 YesNo(o.OutputMergerLogicOp),
                 YesNo(o.VPAndRTArrayIndexFromAnyShaderFeedingRasterizerSupportedWithoutGSEmulation),
                 YesNo(o.PSSpecifiedStencilRefSupported), YesNo(o.DoublePrecisionFloatShaderOps));
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS1 o1{};
    if (Query(dev, D3D12_FEATURE_D3D12_OPTIONS1, o1)) {
        LOG_INFO(Render, "D3D12: wave ops {} (lanes {}-{}), int64 {}", YesNo(o1.WaveOps),
                 o1.WaveLaneCountMin, o1.WaveLaneCountMax, YesNo(o1.Int64ShaderOps));
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS2 o2{};
    if (Query(dev, D3D12_FEATURE_D3D12_OPTIONS2, o2)) {
        LOG_INFO(Render, "D3D12: depth bounds test {}, programmable sample positions tier {}",
                 YesNo(o2.DepthBoundsTestSupported),
                 static_cast<int>(o2.ProgrammableSamplePositionsTier));
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS3 o3{};
    if (Query(dev, D3D12_FEATURE_D3D12_OPTIONS3, o3)) {
        LOG_INFO(Render, "D3D12: casting fully typed formats {}, barycentrics {}",
                 YesNo(o3.CastingFullyTypedFormatSupported), YesNo(o3.BarycentricsSupported));
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS4 o4{};
    if (Query(dev, D3D12_FEATURE_D3D12_OPTIONS4, o4)) {
        LOG_INFO(Render, "D3D12: native 16-bit shader ops {}",
                 YesNo(o4.Native16BitShaderOpsSupported));
    }

    // The runtime-level additions (Agility SDK era). Each query simply fails on an older runtime.
    D3D12_FEATURE_DATA_D3D12_OPTIONS12 o12{};
    D3D12_FEATURE_DATA_D3D12_OPTIONS14 o14{};
    D3D12_FEATURE_DATA_D3D12_OPTIONS15 o15{};
    D3D12_FEATURE_DATA_D3D12_OPTIONS16 o16{};
    D3D12_FEATURE_DATA_D3D12_OPTIONS17 o17{};
    const bool has12 = Query(dev, D3D12_FEATURE_D3D12_OPTIONS12, o12);
    const bool has14 = Query(dev, D3D12_FEATURE_D3D12_OPTIONS14, o14);
    const bool has15 = Query(dev, D3D12_FEATURE_D3D12_OPTIONS15, o15);
    const bool has16 = Query(dev, D3D12_FEATURE_D3D12_OPTIONS16, o16);
    const bool has17 = Query(dev, D3D12_FEATURE_D3D12_OPTIONS17, o17);
    LOG_INFO(Render,
             "D3D12: enhanced barriers {}, independent front/back stencil {}, triangle fans {}, "
             "dynamic depth bias {}, non-normalized samplers {}",
             has12 ? YesNo(o12.EnhancedBarriersSupported) : "n/a",
             has14 ? YesNo(o14.IndependentFrontAndBackStencilRefMaskSupported) : "n/a",
             has15 ? YesNo(o15.TriangleFanSupported) : "n/a",
             has16 ? YesNo(o16.DynamicDepthBiasSupported) : "n/a",
             has17 ? YesNo(o17.NonNormalizedCoordinateSamplersSupported) : "n/a");

    D3D12_FEATURE_DATA_ARCHITECTURE1 arch{};
    if (Query(dev, D3D12_FEATURE_ARCHITECTURE1, arch)) {
        LOG_INFO(Render, "D3D12: UMA {}, cache-coherent UMA {}", YesNo(arch.UMA),
                 YesNo(arch.CacheCoherentUMA));
    }

    // Typed UAV loads decide how the shader backend lowers format-less storage image reads.
    constexpr std::array<std::pair<DXGI_FORMAT, const char*>, 6> uav_formats{{
        {DXGI_FORMAT_R8G8B8A8_UNORM, "RGBA8_UNORM"},
        {DXGI_FORMAT_R16G16B16A16_FLOAT, "RGBA16F"},
        {DXGI_FORMAT_R32G32B32A32_FLOAT, "RGBA32F"},
        {DXGI_FORMAT_R11G11B10_FLOAT, "R11G11B10F"},
        {DXGI_FORMAT_R32G32_UINT, "RG32UI"},
        {DXGI_FORMAT_B8G8R8A8_UNORM, "BGRA8_UNORM"},
    }};
    std::string typed_loads;
    for (const auto& [format, name] : uav_formats) {
        D3D12_FEATURE_DATA_FORMAT_SUPPORT support{format};
        const bool ok = Query(dev, D3D12_FEATURE_FORMAT_SUPPORT, support) &&
                        (support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD) != 0;
        typed_loads += fmt::format(" {}={}", name, ok ? "yes" : "no");
    }
    LOG_INFO(Render, "D3D12: typed UAV loads:{}", typed_loads);

    if (adapter3) {
        const DXGI_QUERY_VIDEO_MEMORY_INFO local = QueryVideoMemory();
        LOG_INFO(Render, "D3D12: local video memory budget {} MiB, in use {} MiB",
                 local.Budget >> 20, local.CurrentUsage >> 20);
    }
}

DXGI_QUERY_VIDEO_MEMORY_INFO Device::QueryVideoMemory() const {
    DXGI_QUERY_VIDEO_MEMORY_INFO info{};
    if (!adapter3 ||
        FAILED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
        return {};
    }
    return info;
}

} // namespace D3D12
