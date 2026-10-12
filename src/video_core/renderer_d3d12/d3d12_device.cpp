// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <vector>

#include <fmt/format.h>

#include "common/bug_tracker.h"
#include "common/logging.h"
#include "common/memory_ledger.h"
#include "common/settings.h"
#include "video_core/renderer_d3d12/d3d12_command_stream.h"
#include "video_core/renderer_d3d12/d3d12_debug_messages.h"
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
        // `what` is a literal: with the HRESULT it tells the failing call sites apart.
        BUG_TRACK_KEY(ApiCallFailed,
                      static_cast<u64>(static_cast<u32>(hr)) << 32 ^
                          reinterpret_cast<std::uintptr_t>(what),
                      "{} failed (HRESULT 0x{:08X})", what, static_cast<u32>(hr));
        throw std::runtime_error(
            fmt::format("D3D12: {} failed (HRESULT 0x{:08X})", what, static_cast<u32>(hr)));
    }
}

namespace removal_tripwire {
std::atomic_bool removal_tripped{false};
std::atomic_bool check_descriptors{false};

void ReportRemovedAfter(ID3D12Device* device, HRESULT reason, const std::string& what) {
    if (!removal_tripped.exchange(true)) {
        BUG_TRACK_KEY(DeviceRemoved, static_cast<u32>(reason),
                      "device removed (reason 0x{:08X}) right after {}", static_cast<u32>(reason),
                      what);
        {
            const Common::BugTracker::TapMute bug_tracker_mute;
            LOG_CRITICAL(Render, "D3D12: device removed (reason 0x{:08X}) right after {}",
                         static_cast<u32>(reason), what);
        }
        DumpDeviceDiagnostics(device);
        Common::BugTracker::RequestFlush(); // the process may not survive the recovery
    }
}
} // namespace removal_tripwire

void SetDescriptorRemovalChecks(bool enabled) {
    removal_tripwire::check_descriptors.store(enabled, std::memory_order_relaxed);
}

namespace {
bool gpu_based_validation = false;
bool dred_enabled = false;
std::mutex diagnostic_mutex;
std::array<std::string, 128> diagnostic_history;
// Separate from submissions/waits: a busy draw cannot erase the fence history.
// Fixed 32 MiB, no retained resource references; truncation/overwrites reported on dump.
std::array<std::array<char, 512>, 65536> diagnostic_details{};
u64 detail_sequence{};
u64 diagnostic_sequence{};
std::atomic_flag diagnostic_dumped{};
std::atomic<AppMemoryQuery> app_memory_query{nullptr};
bool breadcrumbs_enabled = false;
// Two tokens: [0] the last command the GPU started, [1] the last it finished. A readback heap
// stays mapped, and its memory is still readable after the device is removed.
ComPtr<ID3D12Resource> breadcrumb_buffer;
const volatile u32* breadcrumb_values{};
std::atomic<D3D12_GPU_VIRTUAL_ADDRESS> breadcrumb_address{};
} // Anonymous namespace

void SetGpuBasedValidation(bool enabled) {
    gpu_based_validation = enabled;
}

void SetDredEnabled(bool enabled) {
    dred_enabled = enabled;
    diagnostic_dumped.clear();
    std::scoped_lock lock{diagnostic_mutex};
    diagnostic_sequence = 0;
    detail_sequence = 0;
    diagnostic_history.fill({});
}

bool GpuDiagnosticsEnabled() { return dred_enabled; }

u64 TraceGpuDetail(std::string_view description) {
    if (!dred_enabled) return 0;
    std::scoped_lock lock{diagnostic_mutex};
    const u64 sequence = detail_sequence++;
    auto& entry = diagnostic_details[sequence % diagnostic_details.size()];
    const size_t length = std::min(description.size(), entry.size() - 1);
    std::copy_n(description.data(), length, entry.data());
    entry[length] = '\0';
    if (description.size() > length) {
        entry[length - 3] = '.';
        entry[length - 2] = '.';
        entry[length - 1] = '.';
    }
    return sequence + 1;
}

void SetGpuBreadcrumbsEnabled(bool enabled) {
    breadcrumbs_enabled = enabled;
}

D3D12_GPU_VIRTUAL_ADDRESS GpuBreadcrumbAddress() {
    return breadcrumb_address.load(std::memory_order_relaxed);
}

void WriteGpuBreadcrumb(ID3D12GraphicsCommandList* list, D3D12_GPU_VIRTUAL_ADDRESS address,
                        u32 token, bool finished) {
    ComPtr<ID3D12GraphicsCommandList2> list2;
    if (FAILED(list->QueryInterface(IID_PPV_ARGS(&list2)))) {
        return;
    }
    // MARKER_IN writes as the GPU reaches the command; MARKER_OUT once all work before it is done.
    const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER parameter{
        .Dest = address + (finished ? sizeof(u32) : 0),
        .Value = token,
    };
    const D3D12_WRITEBUFFERIMMEDIATE_MODE mode = finished
                                                     ? D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_OUT
                                                     : D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_IN;
    list2->WriteBufferImmediate(1, &parameter, &mode);
}

void TraceGpuOperation(std::string description) {
    if (!dred_enabled) return;
    std::scoped_lock lock{diagnostic_mutex};
    const u64 sequence = diagnostic_sequence++;
    diagnostic_history[sequence % diagnostic_history.size()] =
        fmt::format("seq={} {}", sequence, description);
}

void MarkGpuCommands(CommandStream* cmd, const char* kind, u64 first, u64 second) {
    if (!dred_enabled) return;
    // PIX marker format 0: a wide string, which DRED keeps as the breadcrumb context.
    wchar_t text[64];
    const int length = swprintf(text, std::size(text), L"%hs %016llx %016llx", kind,
                                static_cast<unsigned long long>(first),
                                static_cast<unsigned long long>(second));
    if (length > 0) {
        cmd->SetMarker(0, text, static_cast<UINT>((length + 1) * sizeof(wchar_t)));
    }
}

void SetAppMemoryQuery(AppMemoryQuery query) {
    app_memory_query.store(query, std::memory_order_release);
}

namespace {
void CreateBreadcrumbBuffer(ID3D12Device* device) {
    breadcrumb_address.store(0, std::memory_order_relaxed);
    breadcrumb_values = nullptr;
    breadcrumb_buffer.Reset();
    if (!dred_enabled || !breadcrumbs_enabled) {
        return;
    }
    const D3D12_HEAP_PROPERTIES heap{.Type = D3D12_HEAP_TYPE_READBACK};
    const D3D12_RESOURCE_DESC desc{
        .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
        .Width = 256,
        .Height = 1,
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .SampleDesc = {.Count = 1},
        .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
    };
    // WriteBufferImmediate needs COPY_DEST, which is also a readback buffer's only state.
    ComPtr<ID3D12Resource> buffer;
    void* mapped = nullptr;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                               D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                               IID_PPV_ARGS(&buffer))) ||
        FAILED(buffer->Map(0, nullptr, &mapped))) {
        LOG_WARNING(Render, "D3D12: GPU breadcrumbs unavailable (no readback buffer)");
        return;
    }
    std::memset(mapped, 0, static_cast<size_t>(desc.Width));
    breadcrumb_buffer = std::move(buffer);
    breadcrumb_values = static_cast<const volatile u32*>(mapped);
    breadcrumb_address.store(breadcrumb_buffer->GetGPUVirtualAddress(), std::memory_order_relaxed);
    LOG_INFO(Render, "D3D12: GPU breadcrumbs on (diagnostic, slower GPU)");
}
} // Anonymous namespace

Device::Device() {
    removal_tripwire::removal_tripped.store(false);
    ThrowIfFailed(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2");
    // The debug layer (D3D12SDKLayers.dll, the "Graphics Tools" optional feature) is a PC-only aid:
    // it must be enabled before the device exists.
    const bool debug_layer = Settings::values.renderer_debug.GetValue();
    if (debug_layer) {
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
            debug->EnableDebugLayer();
            // GPU-based validation checks every descriptor and resource access as the GPU runs it;
            // far slower, for chasing page faults and hangs.
            ComPtr<ID3D12Debug1> debug1;
            if (gpu_based_validation && SUCCEEDED(debug.As(&debug1))) {
                debug1->SetEnableGPUBasedValidation(TRUE);
                LOG_INFO(Render, "D3D12: GPU-based validation on");
            }
        } else {
            LOG_WARNING(Render, "D3D12: debug layer requested but not installed");
        }
    }
    // DRED (part of the runtime, not the SDK layers, so also on the console) is invaluable after a
    // device removal, but it is not free: Microsoft documents 2-5% typical overhead for automatic
    // breadcrumbs, plus object create/destroy overhead for page-fault tracking. Keep it diagnostic.
    {
        ComPtr<ID3D12DeviceRemovedExtendedDataSettings> dred;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dred)))) {
            const auto setting = dred_enabled ? D3D12_DRED_ENABLEMENT_FORCED_ON
                                              : D3D12_DRED_ENABLEMENT_FORCED_OFF;
            dred->SetAutoBreadcrumbsEnablement(setting);
            dred->SetPageFaultEnablement(setting);
            // DRED 1.2: keep the marker strings (MarkGpuCommands) with the breadcrumbs.
            ComPtr<ID3D12DeviceRemovedExtendedDataSettings1> dred1;
            if (SUCCEEDED(dred.As(&dred1))) {
                dred1->SetBreadcrumbContextEnablement(setting);
            }
            LOG_INFO(Render, "D3D12: DRED breadcrumbs and page faults {}",
                     dred_enabled ? "on" : "off (boot.cfg dred=1 enables diagnostics)");
        } else {
            LOG_INFO(Render, "D3D12: DRED not available");
        }
    }
    // Feature level 11.0 is all Xbox Series UWP offers; desktop GPUs accept it too.
    ThrowIfFailed(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)),
                  "D3D12CreateDevice");
    if (debug_layer && SUCCEEDED(device.As(&info_queue))) {
        // Guest images are created before the guest says what it clears them to, so every
        // clear is "slower than it could be" by design; hide that one. Guest pixel shaders often
        // write more outputs than the draw binds (harmless, writes are discarded): hide it too.
        std::array<D3D12_MESSAGE_ID, 3> hidden{
            D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE,
            D3D12_MESSAGE_ID_CLEARDEPTHSTENCILVIEW_MISMATCHINGCLEARVALUE,
            D3D12_MESSAGE_ID_CREATEGRAPHICSPIPELINESTATE_RENDERTARGETVIEW_NOT_SET,
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

    CreateBreadcrumbBuffer(device.Get());
    LogCapabilities();
    initial_budget = QueryVideoMemory().Budget;
    // On a unified architecture (the Xbox) texture and buffer heaps are app commit too.
    D3D12_FEATURE_DATA_ARCHITECTURE1 arch{};
    Common::SetGpuMemoryUnified(
        SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE1, &arch, sizeof(arch))) &&
        arch.UMA);
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
        if (IsNullRtvFormatFalsePositive(*message)) {
            ++null_rtv_format_reports;
            if (null_rtv_format_reports == 1 || null_rtv_format_reports % 1024 == 0) {
                LOG_INFO(Render,
                         "D3D12 debug layer: {} known null RTV/UNKNOWN format false positives "
                         "counted separately (not charged to the error limit); example: {}",
                         null_rtv_format_reports, text);
            }
            continue;
        }
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

/// The detail sequence number a breadcrumb token (sequence + 1, truncated to 32 bits) stands for:
/// the newest one recorded with those low bits.
std::optional<u64> TokenSequence(u32 token) {
    if (token == 0) {
        return std::nullopt;
    }
    u64 value = (detail_sequence & ~u64{0xFFFFFFFF}) | token;
    if (value > detail_sequence) {
        value -= u64{1} << 32;
    }
    return value - 1;
}

/// The details that CommandStream brackets with breadcrumbs.
bool IsBreadcrumbedCommand(std::string_view text) {
    constexpr std::array<std::string_view, 6> prefixes{
        "DrawInstanced ", "DrawIndexed ", "Dispatch ", "ExecuteIndirect ", "clear ", "Copy",
    };
    return std::ranges::any_of(prefixes, [text](std::string_view prefix) {
        return text.starts_with(prefix);
    });
}

/// With gpu_breadcrumbs=1: the command the GPU never finished and the details around it. Called
/// with diagnostic_mutex held.
void DumpBreadcrumbs(u64 detail_first) {
    if (breadcrumb_values == nullptr) {
        return;
    }
    const auto detail = [](u64 sequence) -> std::string_view {
        return diagnostic_details[sequence % diagnostic_details.size()].data();
    };
    const auto describe = [](const std::optional<u64>& sequence) {
        return sequence ? fmt::to_string(*sequence) : std::string{"none"};
    };
    const std::optional<u64> started = TokenSequence(breadcrumb_values[0]);
    const std::optional<u64> finished = TokenSequence(breadcrumb_values[1]);
    LOG_CRITICAL(Render, "D3D12 GPU breadcrumbs: last command started seq={}, last finished seq={}",
                 describe(started), describe(finished));

    // Commands finish in order, so the first one after the last finished is where the GPU stopped.
    std::optional<u64> unfinished;
    for (u64 i = finished ? (std::max)(*finished + 1, detail_first) : detail_first;
         i < detail_sequence; ++i) {
        if (IsBreadcrumbedCommand(detail(i))) {
            unfinished = i;
            break;
        }
    }
    if (!unfinished) {
        LOG_CRITICAL(Render, "D3D12 GPU hang: every recorded command finished");
        return;
    }
    LOG_CRITICAL(Render, "D3D12 GPU hang: the GPU never finished seq={} {}", *unfinished,
                 detail(*unfinished));
    // The line naming its pipeline and shaders is recorded a few details before the command.
    constexpr u64 PIPELINE_LOOKBEHIND = 256;
    for (u64 i = *unfinished; i > detail_first && *unfinished - i < PIPELINE_LOOKBEHIND; --i) {
        const std::string_view text = detail(i - 1);
        if (text.starts_with("draw tick=") || text.starts_with("dispatch tick=") ||
            text.starts_with("compute image tick=")) {
            LOG_CRITICAL(Render, "D3D12 GPU hang: its pipeline, seq={} {}", i - 1, text);
            break;
        }
    }

    constexpr u64 WINDOW_BEFORE = 48;
    constexpr u64 WINDOW_MAX = 400;
    const u64 window_first = (std::max)(detail_first, *unfinished > WINDOW_BEFORE
                                                          ? *unfinished - WINDOW_BEFORE
                                                          : u64{0});
    const u64 window_end =
        (std::min)({detail_sequence, (std::max)(*unfinished, started.value_or(0)) + 8,
                    window_first + WINDOW_MAX});
    const Common::BugTracker::TapMute window_mute;
    for (u64 i = window_first; i < window_end; ++i) {
        const char* const mark = i == *unfinished ? "  <== NEVER FINISHED"
                                 : i == finished  ? "  <== last finished"
                                 : i == started   ? "  <== last started"
                                                  : "";
        LOG_CRITICAL(Render, "D3D12 GPU hang window: seq={} {}{}", i, detail(i), mark);
    }
}

} // Anonymous namespace

void Device::ReportDeviceRemoved() {
    if (removal_reported.test_and_set()) {
        return;
    }
    const HRESULT reason = device->GetDeviceRemovedReason();
    BUG_TRACK_KEY(DeviceRemoved, static_cast<u32>(reason),
                  "device removed, reason 0x{:08X} (DRED details in eden_log.txt with dred=1)",
                  static_cast<u32>(reason));
    {
        const Common::BugTracker::TapMute bug_tracker_mute;
        LOG_CRITICAL(Render, "D3D12: device removed, reason 0x{:08X}", static_cast<u32>(reason));
    }
    LogDebugMessages();

    DumpDeviceDiagnostics(device.Get());
    Common::BugTracker::RequestFlush();
}

void DumpDeviceDiagnostics(ID3D12Device* device) {
    removal_tripwire::removal_tripped.store(true);
    if (diagnostic_dumped.test_and_set()) return;
    {
        std::scoped_lock lock{diagnostic_mutex};
        const u64 first = diagnostic_sequence > diagnostic_history.size()
                              ? diagnostic_sequence - diagnostic_history.size() : 0;
        for (u64 i = first; i < diagnostic_sequence; ++i)
            LOG_CRITICAL(Render, "D3D12 GPU history: {}",
                         diagnostic_history[i % diagnostic_history.size()]);
        const u64 detail_first = detail_sequence > diagnostic_details.size()
                                     ? detail_sequence - diagnostic_details.size() : 0;
        LOG_CRITICAL(Render, "D3D12 GPU details: {} total, {} overwritten; entries max 511 bytes",
                     detail_sequence, detail_first);
        {
            const Common::BugTracker::TapMute detail_mute;
            for (u64 i = detail_first; i < detail_sequence; ++i) {
                LOG_CRITICAL(Render, "D3D12 GPU detail: seq={} {}", i,
                             diagnostic_details[i % diagnostic_details.size()].data());
            }
        }
        DumpBreadcrumbs(detail_first);
    }
    ComPtr<ID3D12DeviceRemovedExtendedData> dred;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dred)))) {
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
        // Empty/incomplete telemetry cannot prove that the GPU executed no failing command.
        LOG_CRITICAL(Render, "D3D12 DRED: {} command lists recorded, {} unfinished", lists,
                     unfinished);
    }
    // Which pipeline each unfinished list was running: the last marker at or before the first
    // operation that did not complete.
    ComPtr<ID3D12DeviceRemovedExtendedData1> dred1;
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 breadcrumbs1{};
    if (SUCCEEDED(dred.As(&dred1)) && SUCCEEDED(dred1->GetAutoBreadcrumbsOutput1(&breadcrumbs1))) {
        for (const D3D12_AUTO_BREADCRUMB_NODE1* node = breadcrumbs1.pHeadAutoBreadcrumbNode;
             node != nullptr; node = node->pNext) {
            const u32 completed = node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0;
            if (completed >= node->BreadcrumbCount) {
                continue;
            }
            const D3D12_DRED_BREADCRUMB_CONTEXT* last = nullptr;
            for (u32 i = 0; i < node->BreadcrumbContextsCount; ++i) {
                const D3D12_DRED_BREADCRUMB_CONTEXT& context = node->pBreadcrumbContexts[i];
                if (context.BreadcrumbIndex <= completed &&
                    (!last || context.BreadcrumbIndex >= last->BreadcrumbIndex)) {
                    last = &context;
                }
            }
            if (!last || !last->pContextString) {
                LOG_CRITICAL(Render, "D3D12 DRED: unfinished list without a marker ({} contexts)",
                             node->BreadcrumbContextsCount);
                continue;
            }
            std::string context;
            for (const wchar_t* c = last->pContextString; *c != 0; ++c) {
                context.push_back(static_cast<char>(*c));
            }
            LOG_CRITICAL(Render, "D3D12 DRED: unfinished list was running {} (marker at op {}, "
                                 "stopped at op {})",
                         context, last->BreadcrumbIndex, completed);
        }
    } else {
        LOG_CRITICAL(Render, "D3D12 DRED: no breadcrumb contexts (DRED 1.2) on this runtime");
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
    if (queue && fence && SUCCEEDED(device->GetDeviceRemovedReason())) {
        try { WaitIdle(); }
        catch (const std::exception& e) { LOG_ERROR(Render, "Device teardown: {}", e.what()); }
    }
    if (fence_event != nullptr) {
        CloseHandle(fence_event);
    }
}

u64 Device::Signal() {
    std::scoped_lock lock{queue_mutex};
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

CacheMemorySnapshot Device::QueryCacheMemoryPressure() const {
    const auto local = QueryVideoMemory();
    CacheMemorySnapshot result{.gpu_used = local.CurrentUsage, .gpu_budget = local.Budget};
    if (const auto query = app_memory_query.load(std::memory_order_acquire)) {
        if (!query(result.app_used, result.app_limit)) {
            result.app_used = result.app_limit = 0;
        }
    }
    return result;
}

u64 Device::CacheMemoryUsage() const {
    return CacheMemoryUsage(QueryCacheMemoryPressure());
}

u64 Device::CacheMemoryUsage(const CacheMemorySnapshot& snapshot) const {
    if (initial_budget == 0 || snapshot.app_limit == 0) {
        return snapshot.gpu_used;
    }
    const u64 app_free = snapshot.AppFree();
    const u64 usage = initial_budget > app_free ? initial_budget - app_free : 0;
    return (std::max)(usage, snapshot.gpu_used);
}

u64 Device::CacheMemoryBudget() const {
    constexpr u64 AppHeadroom = 1536ULL * 1024 * 1024;
    const u64 dxgi_budget = QueryVideoMemory().Budget;
    const AppMemoryQuery query = app_memory_query.load(std::memory_order_acquire);
    u64 used{};
    u64 limit{};
    if (query == nullptr || !query(used, limit) || limit <= AppHeadroom) {
        return dxgi_budget;
    }
    const u64 app_cache_budget = limit - AppHeadroom;
    return dxgi_budget != 0 ? (std::min)(dxgi_budget, app_cache_budget) : app_cache_budget;
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
