// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <stdexcept>
#include <string_view>
#include <vector>

#include <fmt/format.h>

#include "common/logging.h"
#include "common/settings.h"
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

namespace removal_tripwire {
std::atomic_bool removal_tripped{false};

void ReportRemovedAfter(HRESULT reason, const std::string& what) {
    if (!removal_tripped.exchange(true)) {
        LOG_CRITICAL(Render, "D3D12: device removed (reason 0x{:08X}) right after {}",
                     static_cast<u32>(reason), what);
    }
}
} // namespace removal_tripwire

Device::Device() {
    ThrowIfFailed(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2");
    // The debug layer (D3D12SDKLayers.dll, the "Graphics Tools" optional feature) is a PC-only aid:
    // it must be enabled before the device exists.
    const bool debug_layer = Settings::values.renderer_debug.GetValue();
    if (debug_layer) {
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
            debug->EnableDebugLayer();
        } else {
            LOG_WARNING(Render, "D3D12: debug layer requested but not installed");
        }
    }
    // DRED (part of the runtime, not the SDK layers, so also on the console): on a device removal,
    // the last GPU operations of each command list and the faulting address (ReportDeviceRemoved).
    // Its cost is small next to what a removal on the console costs to diagnose without it.
    {
        ComPtr<ID3D12DeviceRemovedExtendedDataSettings> dred;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dred)))) {
            dred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            LOG_INFO(Render, "D3D12: DRED breadcrumbs and page faults on");
        } else {
            LOG_INFO(Render, "D3D12: DRED not available");
        }
    }
    // Feature level 11.0 is all Xbox Series UWP offers; desktop GPUs accept it too.
    ThrowIfFailed(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)),
                  "D3D12CreateDevice");
    if (debug_layer && SUCCEEDED(device.As(&info_queue))) {
        // Guest images are created before the guest says what it clears them to, so every
        // clear is "slower than it could be" by design; hide that one.
        std::array<D3D12_MESSAGE_ID, 2> hidden{
            D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE,
            D3D12_MESSAGE_ID_CLEARDEPTHSTENCILVIEW_MISMATCHINGCLEARVALUE,
        };
        D3D12_INFO_QUEUE_FILTER filter{};
        filter.DenyList.NumIDs = static_cast<UINT>(hidden.size());
        filter.DenyList.pIDList = hidden.data();
        info_queue->PushStorageFilter(&filter);
        LOG_INFO(Render, "D3D12: debug layer active; its messages go to this log");
    }

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

void Device::LogDebugMessages() {
    if (!info_queue) {
        return;
    }
    constexpr u32 MAX_LOGGED = 500;
    const u64 count = info_queue->GetNumStoredMessages();
    for (u64 index = 0; index < count && debug_messages_logged < MAX_LOGGED; ++index) {
        SIZE_T size = 0;
        if (FAILED(info_queue->GetMessage(index, nullptr, &size)) || size == 0) {
            continue;
        }
        std::vector<u8> storage(size);
        auto* const message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        if (FAILED(info_queue->GetMessage(index, message, &size))) {
            continue;
        }
        const std::string_view text{message->pDescription, message->DescriptionByteLength};
        switch (message->Severity) {
        case D3D12_MESSAGE_SEVERITY_CORRUPTION:
        case D3D12_MESSAGE_SEVERITY_ERROR:
            LOG_ERROR(Render, "D3D12 debug layer [{}]: {}", static_cast<u32>(message->ID), text);
            ++debug_messages_logged;
            break;
        case D3D12_MESSAGE_SEVERITY_WARNING:
            // Duplicate transitions in one ResourceBarrier call (Image::Transition's write-after-
            // write pair) are only inefficient, and would fill the log before any real error.
            if (message->ID == D3D12_MESSAGE_ID_RESOURCE_BARRIER_DUPLICATE_SUBRESOURCE_TRANSITIONS) {
                break;
            }
            LOG_WARNING(Render, "D3D12 debug layer [{}]: {}", static_cast<u32>(message->ID), text);
            ++debug_messages_logged;
            break;
        default:
            break;
        }
        if (debug_messages_logged == MAX_LOGGED) {
            LOG_WARNING(Render, "D3D12 debug layer: {} messages logged, the rest are dropped",
                        MAX_LOGGED);
        }
    }
    info_queue->ClearStoredMessages();
}

namespace {

const char* BreadcrumbOpName(D3D12_AUTO_BREADCRUMB_OP op) {
    switch (op) {
    case D3D12_AUTO_BREADCRUMB_OP_DRAWINSTANCED:
        return "DrawInstanced";
    case D3D12_AUTO_BREADCRUMB_OP_DRAWINDEXEDINSTANCED:
        return "DrawIndexedInstanced";
    case D3D12_AUTO_BREADCRUMB_OP_EXECUTEINDIRECT:
        return "ExecuteIndirect";
    case D3D12_AUTO_BREADCRUMB_OP_DISPATCH:
        return "Dispatch";
    case D3D12_AUTO_BREADCRUMB_OP_COPYBUFFERREGION:
        return "CopyBufferRegion";
    case D3D12_AUTO_BREADCRUMB_OP_COPYTEXTUREREGION:
        return "CopyTextureRegion";
    case D3D12_AUTO_BREADCRUMB_OP_COPYRESOURCE:
        return "CopyResource";
    case D3D12_AUTO_BREADCRUMB_OP_RESOLVESUBRESOURCE:
        return "ResolveSubresource";
    case D3D12_AUTO_BREADCRUMB_OP_CLEARRENDERTARGETVIEW:
        return "ClearRenderTargetView";
    case D3D12_AUTO_BREADCRUMB_OP_CLEARUNORDEREDACCESSVIEW:
        return "ClearUnorderedAccessView";
    case D3D12_AUTO_BREADCRUMB_OP_CLEARDEPTHSTENCILVIEW:
        return "ClearDepthStencilView";
    case D3D12_AUTO_BREADCRUMB_OP_RESOURCEBARRIER:
        return "ResourceBarrier";
    case D3D12_AUTO_BREADCRUMB_OP_PRESENT:
        return "Present";
    case D3D12_AUTO_BREADCRUMB_OP_RESOLVEQUERYDATA:
        return "ResolveQueryData";
    case D3D12_AUTO_BREADCRUMB_OP_BEGINSUBMISSION:
        return "BeginSubmission";
    case D3D12_AUTO_BREADCRUMB_OP_ENDSUBMISSION:
        return "EndSubmission";
    default:
        return nullptr;
    }
}

} // Anonymous namespace

void Device::ReportDeviceRemoved() {
    if (removal_reported.test_and_set()) {
        return;
    }
    LOG_CRITICAL(Render, "D3D12: device removed, reason 0x{:08X}",
                 static_cast<u32>(device->GetDeviceRemovedReason()));
    LogDebugMessages();

    ComPtr<ID3D12DeviceRemovedExtendedData> dred;
    if (FAILED(device.As(&dred))) {
        LOG_CRITICAL(Render, "D3D12: no DRED data on this device");
        return;
    }
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT breadcrumbs{};
    if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&breadcrumbs))) {
        u32 lists = 0;
        u32 unfinished = 0;
        for (const D3D12_AUTO_BREADCRUMB_NODE* node = breadcrumbs.pHeadAutoBreadcrumbNode;
             node != nullptr; node = node->pNext) {
            ++lists;
            const u32 completed = node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0;
            if (completed >= node->BreadcrumbCount) {
                continue; // this list finished on the GPU
            }
            ++unfinished;
            // The operations around the first one that did not complete.
            std::string ops;
            const u32 first = completed > 6 ? completed - 6 : 0;
            const u32 last = std::min(node->BreadcrumbCount, completed + 4);
            for (u32 i = first; i < last; ++i) {
                const D3D12_AUTO_BREADCRUMB_OP op = node->pCommandHistory[i];
                const char* name = BreadcrumbOpName(op);
                ops += fmt::format("{}{}{}", i == completed ? " >>" : " ",
                                   name ? name : "op", name ? "" : std::to_string(op));
            }
            LOG_CRITICAL(Render, "D3D12 DRED: list with {} ops stopped after {}:{}",
                         node->BreadcrumbCount, completed, ops);
        }
        // Every recorded list done means the GPU was not the one that failed: a CPU-side call
        // with parameters the driver rejects did (see the "right after" line of CheckRemovedAfter).
        LOG_CRITICAL(Render, "D3D12 DRED: {} command lists recorded, {} unfinished", lists,
                     unfinished);
    }
    D3D12_DRED_PAGE_FAULT_OUTPUT page_fault{};
    if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&page_fault)) && page_fault.PageFaultVA != 0) {
        LOG_CRITICAL(Render, "D3D12 DRED: page fault at GPU VA 0x{:X}", page_fault.PageFaultVA);
        u32 listed = 0;
        for (const D3D12_DRED_ALLOCATION_NODE* node = page_fault.pHeadRecentFreedAllocationNode;
             node != nullptr && listed < 8; node = node->pNext, ++listed) {
            LOG_CRITICAL(Render, "D3D12 DRED: recently freed allocation there: type {}",
                         static_cast<u32>(node->AllocationType));
        }
        listed = 0;
        for (const D3D12_DRED_ALLOCATION_NODE* node = page_fault.pHeadExistingAllocationNode;
             node != nullptr && listed < 8; node = node->pNext, ++listed) {
            LOG_CRITICAL(Render, "D3D12 DRED: live allocation there: type {}",
                         static_cast<u32>(node->AllocationType));
        }
    }
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
