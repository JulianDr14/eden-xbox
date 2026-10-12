// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/hle/service/cmif_serialization.h"
#include "core/hle/kernel/k_readable_event.h"
#include "core/hle/service/nvdrv/nvdrv_interface.h"
#include "core/hle/service/nvnflinger/buffer_queue_producer.h"
#include "core/hle/service/nvnflinger/hos_binder_driver.h"
#include "core/hle/service/nvnflinger/hos_binder_driver_server.h"
#include "core/hle/service/sm/sm.h"
#include "core/hle/service/nvnflinger/parcel.h"
#include "core/hle/service/os/event.h"
#include "core/hle/service/vi/application_display_service.h"
#include "core/hle/service/vi/container.h"
#include "core/hle/service/vi/manager_display_service.h"
#include "core/hle/service/vi/system_display_service.h"
#include "core/hle/service/vi/vi_results.h"
#include "service_creator.h"

namespace Service::VI {

IApplicationDisplayService::IApplicationDisplayService(Core::System& system_,
                                                       std::shared_ptr<Container> container)
    : ServiceFramework{system_, "IApplicationDisplayService"},
      m_container{std::move(container)}, m_context{system, "IApplicationDisplayService"} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {100, C<&IApplicationDisplayService::GetRelayService>, "GetRelayService"},
        {101, C<&IApplicationDisplayService::GetSystemDisplayService>, "GetSystemDisplayService"},
        {102, C<&IApplicationDisplayService::GetManagerDisplayService>, "GetManagerDisplayService"},
        {103, C<&IApplicationDisplayService::GetIndirectDisplayTransactionService>, "GetIndirectDisplayTransactionService"},
        {1000, C<&IApplicationDisplayService::ListDisplays>, "ListDisplays"},
        {1010, C<&IApplicationDisplayService::OpenDisplay>, "OpenDisplay"},
        {1011, C<&IApplicationDisplayService::OpenDefaultDisplay>, "OpenDefaultDisplay"},
        {1020, C<&IApplicationDisplayService::CloseDisplay>, "CloseDisplay"},
        {1101, C<&IApplicationDisplayService::SetDisplayEnabled>, "SetDisplayEnabled"},
        {1102, C<&IApplicationDisplayService::GetDisplayResolution>, "GetDisplayResolution"},
        {2020, C<&IApplicationDisplayService::OpenLayer>, "OpenLayer"},
        {2021, C<&IApplicationDisplayService::CloseLayer>, "CloseLayer"},
        {2030, C<&IApplicationDisplayService::CreateStrayLayer>, "CreateStrayLayer"},
        {2031, C<&IApplicationDisplayService::DestroyStrayLayer>, "DestroyStrayLayer"},
        {2101, C<&IApplicationDisplayService::SetLayerScalingMode>, "SetLayerScalingMode"},
        {2102, C<&IApplicationDisplayService::ConvertScalingMode>, "ConvertScalingMode"},
        {2103, C<&IApplicationDisplayService::Cmd2103>, "Cmd2103"},
        {2450, C<&IApplicationDisplayService::GetIndirectLayerImageMap>, "GetIndirectLayerImageMap"},
        {2451, nullptr, "GetIndirectLayerImageCropMap"},
        {2460, C<&IApplicationDisplayService::GetIndirectLayerImageRequiredMemoryInfo>, "GetIndirectLayerImageRequiredMemoryInfo"},
        {5202, C<&IApplicationDisplayService::GetDisplayVsyncEvent>, "GetDisplayVsyncEvent"},
        {5203, nullptr, "GetDisplayVsyncEventForDebug"},
    };
    // clang-format on

    RegisterHandlers(functions);
}

IApplicationDisplayService::~IApplicationDisplayService() {
    for (auto& [display_id, event] : m_display_vsync_events) {
        m_container->UnlinkVsyncEvent(display_id, &event);
    }
    for (const auto layer_id : m_open_layer_ids) {
        m_container->CloseLayer(layer_id);
    }
    for (const auto layer_id : m_stray_layer_ids) {
        m_container->DestroyStrayLayer(layer_id);
    }
}

void IApplicationDisplayService::PulseCapture(Pulse::HandlerCapture& capture) const {
    std::scoped_lock lk{m_lock};
    auto& out = capture.state;

    // The nvmap handles behind the layers' buffers are restored first.
    std::vector<u64> nvdrv_refs;
    for (const char* port : {"nvdrv", "nvdrv:s", "nvdrv:t"}) {
        const auto nvdrv = system.ServiceManager().GetService<Nvidia::NVDRV>(port);
        if (nvdrv && nvdrv->PulseInitialized()) nvdrv_refs.push_back(capture.Reference(nvdrv));
    }
    out.U32(static_cast<u32>(nvdrv_refs.size()));
    for (const u64 ref : nvdrv_refs) out.U64(ref);

    // The managed layers opened here are am's: it recreates them, closed, before this reopens them.
    SessionRequestHandlerPtr am;
    if (!m_open_layer_ids.empty()) {
        am = system.ServiceManager().GetService<SessionRequestHandler>("appletOE");
        if (!am) throw Pulse::ServiceRejected(false, "vi managed layers without am");
    }
    out.U64(capture.Reference(am));
    out.U32(static_cast<u32>(m_open_layer_ids.size()));
    for (const u64 layer_id : m_open_layer_ids) out.U64(layer_id);

    const auto [next_layer, last_binder] = m_container->PulseNextIds();
    out.U64(next_layer);
    out.S32(last_binder);
    out.Bool(m_vsync_event_fetched);

    out.U32(static_cast<u32>(m_display_vsync_events.size()));
    u32 slot{};
    for (const auto& [display_id, event] : m_display_vsync_events) {
        out.U64(display_id);
        capture.ExportEvent(slot++, const_cast<Event&>(event).GetHandle()->GetParent());
    }

    std::shared_ptr<Nvnflinger::IHOSBinderDriver> driver;
    m_container->GetBinderDriver(&driver);
    const auto server = driver->GetServer();
    out.U32(static_cast<u32>(m_stray_layer_ids.size()));
    for (const u64 layer_id : m_stray_layer_ids) {
        const auto layer = m_container->PulseGetLayer(layer_id);
        if (!layer) throw Pulse::ServiceRejected(false, "a vi stray layer is not on a display");
        const auto producer = std::static_pointer_cast<android::BufferQueueProducer>(
            server->TryGetBinder(layer->producer_binder_id));
        if (!producer) throw Pulse::ServiceRejected(false, "a vi layer lost its producer binder");
        out.U64(layer_id);
        out.U64(layer->display_id);
        out.S32(layer->consumer_binder_id);
        out.S32(layer->producer_binder_id);
        out.Bool(layer->visible);
        out.U32(layer->blending);
        out.S32(layer->z_index);
        out.Bool(layer->is_overlay);
        out.U32(layer->layer_stack_mask);
        for (const s32 id : {layer->consumer_binder_id, layer->producer_binder_id}) {
            const auto [strong, weak] = server->PulseRefcounts(id);
            out.S32(strong);
            out.S32(weak);
        }
        Pulse::StateWriter queue;
        producer->PulseCapture(queue);
        out.U32(static_cast<u32>(queue.bytes.size()));
        out.Bytes(queue.bytes);
    }
}

Kernel::KEvent* IApplicationDisplayService::PulseEvent(u32 slot) {
    std::scoped_lock lk{m_lock};
    if (slot >= m_display_vsync_events.size()) return nullptr;
    auto it = m_display_vsync_events.begin();
    std::advance(it, slot);
    return it->second.GetHandle()->GetParent();
}

SessionRequestHandlerPtr IApplicationDisplayService::PulseRestoreFrom(
    Core::System& system, std::shared_ptr<Container> container, Pulse::HandlerRestore& restore) {
    auto& in = restore.state;
    const auto refs = in.Count(8);
    for (u32 i = 0; i < refs; ++i) restore.Resolve(in.U64());
    restore.Resolve(in.U64());
    std::vector<u64> open_layers(in.Count(8));
    for (auto& layer_id : open_layers) layer_id = in.U64();

    auto service = std::make_shared<IApplicationDisplayService>(system, container);
    const u64 next_layer = in.U64();
    const s32 last_binder = in.S32();
    service->m_vsync_event_fetched = in.Bool();

    const auto vsyncs = in.Count(8);
    for (u32 i = 0; i < vsyncs; ++i) {
        const u64 display_id = in.U64();
        auto [it, created] = service->m_display_vsync_events.emplace(display_id, service->m_context);
        if (!created) throw std::invalid_argument("Pulse: vi vsync event twice");
        container->LinkVsyncEvent(display_id, &it->second);
    }

    std::shared_ptr<Nvnflinger::IHOSBinderDriver> driver;
    container->GetBinderDriver(&driver);
    const auto server = driver->GetServer();
    const auto layers = in.Count(8 + 8 + 4 + 4 + 1 + 4 + 4 + 1 + 4 + 16 + 4);
    for (u32 i = 0; i < layers; ++i) {
        const u64 layer_id = in.U64();
        Container::PulseLayer layer{};
        layer.display_id = in.U64();
        layer.consumer_binder_id = in.S32();
        layer.producer_binder_id = in.S32();
        layer.visible = in.Bool();
        layer.blending = in.U32();
        layer.z_index = in.S32();
        layer.is_overlay = in.Bool();
        layer.layer_stack_mask = in.U32();
        s32 counts[4];
        for (auto& count : counts) count = in.S32();

        const auto producer = container->PulseCreateStrayLayer(
            layer_id, layer.display_id, layer.consumer_binder_id, layer.producer_binder_id);
        service->m_stray_layer_ids.insert(layer_id);
        container->PulseSetLayer(layer_id, layer);

        std::vector<u8> queue(in.Count(1));
        in.Bytes(queue);
        Pulse::StateReader queue_in{queue};
        producer->PulseRestore(queue_in);

        server->PulseSetRefcounts(layer.consumer_binder_id, counts[0], counts[1]);
        server->PulseSetRefcounts(layer.producer_binder_id, counts[2], counts[3]);
    }
    for (const u64 layer_id : open_layers) {
        container->PulseOpenLayer(layer_id);
        service->m_open_layer_ids.insert(layer_id);
    }
    in.End();
    container->PulseRaiseNextIds(next_layer, last_binder);
    return service;
}

Result IApplicationDisplayService::GetRelayService(
    Out<SharedPointer<Nvnflinger::IHOSBinderDriver>> out_relay_service) {
    LOG_WARNING(Service_VI, "(STUBBED) called");
    R_RETURN(m_container->GetBinderDriver(out_relay_service));
}

Result IApplicationDisplayService::GetSystemDisplayService(
    Out<SharedPointer<ISystemDisplayService>> out_system_display_service) {
    LOG_WARNING(Service_VI, "(STUBBED) called");
    *out_system_display_service = std::make_shared<ISystemDisplayService>(system, m_container);
    R_SUCCEED();
}

Result IApplicationDisplayService::GetManagerDisplayService(
    Out<SharedPointer<IManagerDisplayService>> out_manager_display_service) {
    LOG_WARNING(Service_VI, "(STUBBED) called");
    *out_manager_display_service = std::make_shared<IManagerDisplayService>(system, m_container);
    R_SUCCEED();
}

Result IApplicationDisplayService::GetIndirectDisplayTransactionService(
    Out<SharedPointer<Nvnflinger::IHOSBinderDriver>> out_indirect_display_transaction_service) {
    LOG_WARNING(Service_VI, "(STUBBED) called");
    R_RETURN(m_container->GetBinderDriver(out_indirect_display_transaction_service));
}

Result IApplicationDisplayService::OpenDisplay(Out<u64> out_display_id, DisplayName display_name) {
    LOG_DEBUG(Service_VI, "called with display_name={}", display_name.data());

    // Ensure the display name is null-terminated
    display_name[display_name.size() - 1] = '\0';

    // According to switchbrew, only "Default", "External", "Edid", "Internal" and "Null" are valid
    const std::array<std::string_view, 5> valid_names = {
        "Default", "External", "Edid", "Internal", "Null"
    };

    bool valid_name = false;
    for (const auto& name : valid_names) {
        if (name == display_name.data()) {
            valid_name = true;
            break;
        }
    }

    R_UNLESS(valid_name, ResultOperationFailed);

    R_RETURN(m_container->OpenDisplay(out_display_id, display_name));
}

Result IApplicationDisplayService::OpenDefaultDisplay(Out<u64> out_display_id) {
    LOG_DEBUG(Service_VI, "called");
    R_RETURN(this->OpenDisplay(out_display_id, DisplayName{"Default"}));
}

Result IApplicationDisplayService::CloseDisplay(u64 display_id) {
    LOG_DEBUG(Service_VI, "called");
    R_RETURN(m_container->CloseDisplay(display_id));
}

Result IApplicationDisplayService::SetDisplayEnabled(u32 state, u64 display_id) {
    LOG_DEBUG(Service_VI, "called");

    // This literally does nothing internally in the actual service itself,
    // and just returns a successful result code regardless of the input.
    R_SUCCEED();
}

Result IApplicationDisplayService::GetDisplayResolution(Out<s64> out_width, Out<s64> out_height,
                                                        u64 display_id) {
    LOG_DEBUG(Service_VI, "called. display_id={}", display_id);

    // This only returns the fixed values of 1280x720 and makes no distinguishing
    // between docked and undocked dimensions.
    *out_width = static_cast<s64>(DisplayResolution::UndockedWidth);
    *out_height = static_cast<s64>(DisplayResolution::UndockedHeight);
    R_SUCCEED();
}

Result IApplicationDisplayService::SetLayerScalingMode(NintendoScaleMode scale_mode, u64 layer_id) {
    LOG_DEBUG(Service_VI, "called. scale_mode={}, unknown={:#016x}", scale_mode, layer_id);

    if (scale_mode > NintendoScaleMode::PreserveAspectRatio) {
        LOG_ERROR(Service_VI, "Invalid scaling mode provided.");
        R_THROW(VI::ResultOperationFailed);
    }

    if (scale_mode != NintendoScaleMode::ScaleToWindow &&
        scale_mode != NintendoScaleMode::PreserveAspectRatio) {
        LOG_ERROR(Service_VI, "Unsupported scaling mode supplied.");
        R_THROW(VI::ResultNotSupported);
    }

    R_SUCCEED();
}

Result IApplicationDisplayService::ListDisplays(
    Out<u64> out_count, OutArray<DisplayInfo, BufferAttr_HipcMapAlias> out_displays) {
    LOG_WARNING(Service_VI, "(STUBBED) called");

    if (out_displays.size() > 0) {
        out_displays[0] = DisplayInfo{};
        *out_count = 1;
    } else {
        *out_count = 0;
    }

    R_SUCCEED();
}

Result IApplicationDisplayService::OpenLayer(Out<u64> out_size,
                                             OutBuffer<BufferAttr_HipcMapAlias> out_native_window,
                                             DisplayName display_name, u64 layer_id,
                                             ClientAppletResourceUserId aruid) {
    display_name[display_name.size() - 1] = '\0';

    LOG_DEBUG(Service_VI, "called. layer_id={}, aruid={:#x}", layer_id, aruid.pid);

    u64 display_id;
    R_TRY(m_container->OpenDisplay(&display_id, display_name));

    s32 producer_binder_id;
    R_TRY(m_container->OpenLayer(&producer_binder_id, layer_id, aruid.pid));

    {
        std::scoped_lock lk{m_lock};
        m_open_layer_ids.insert(layer_id);
    }

    android::OutputParcel parcel;
    parcel.WriteInterface(NativeWindow{producer_binder_id});

    const auto buffer = parcel.Serialize();
    std::memcpy(out_native_window.data(), buffer.data(),
                (std::min)(out_native_window.size(), buffer.size()));
    *out_size = buffer.size();

    R_SUCCEED();
}

Result IApplicationDisplayService::CloseLayer(u64 layer_id) {
    LOG_DEBUG(Service_VI, "called. layer_id={}", layer_id);

    {
        std::scoped_lock lk{m_lock};
        R_UNLESS(m_open_layer_ids.contains(layer_id), VI::ResultNotFound);
        m_open_layer_ids.erase(layer_id);
    }

    R_RETURN(m_container->CloseLayer(layer_id));
}

Result IApplicationDisplayService::CreateStrayLayer(
    Out<u64> out_layer_id, Out<u64> out_size, OutBuffer<BufferAttr_HipcMapAlias> out_native_window,
    u32 flags, u64 display_id) {
    LOG_DEBUG(Service_VI, "called. flags={}, display_id={}", flags, display_id);

    s32 producer_binder_id;
    R_TRY(m_container->CreateStrayLayer(&producer_binder_id, out_layer_id, display_id));

    std::scoped_lock lk{m_lock};
    m_stray_layer_ids.insert(*out_layer_id);

    android::OutputParcel parcel;
    parcel.WriteInterface(NativeWindow{producer_binder_id});

    const auto buffer = parcel.Serialize();
    std::memcpy(out_native_window.data(), buffer.data(),
                (std::min)(out_native_window.size(), buffer.size()));

    *out_size = buffer.size();

    R_SUCCEED();
}

Result IApplicationDisplayService::DestroyStrayLayer(u64 layer_id) {
    LOG_WARNING(Service_VI, "(STUBBED) called. layer_id={}", layer_id);

    {
        std::scoped_lock lk{m_lock};
        R_UNLESS(m_stray_layer_ids.contains(layer_id), VI::ResultNotFound);
        m_stray_layer_ids.erase(layer_id);
    }

    R_RETURN(m_container->DestroyStrayLayer(layer_id));
}

Result IApplicationDisplayService::GetDisplayVsyncEvent(
    OutCopyHandle<Kernel::KReadableEvent> out_vsync_event, u64 display_id) {
    LOG_DEBUG(Service_VI, "called. display_id={}", display_id);

    std::scoped_lock lk{m_lock};

    auto [it, created] = m_display_vsync_events.emplace(display_id, m_context);
    R_UNLESS(created, VI::ResultPermissionDenied);

    m_container->LinkVsyncEvent(display_id, &it->second);
    *out_vsync_event = it->second.GetHandle();

    R_SUCCEED();
}

Result IApplicationDisplayService::ConvertScalingMode(Out<ConvertedScaleMode> out_scaling_mode,
                                                      NintendoScaleMode mode) {
    LOG_DEBUG(Service_VI, "called mode={}", mode);

    switch (mode) {
    case NintendoScaleMode::None:
        *out_scaling_mode = ConvertedScaleMode::None;
        R_SUCCEED();
    case NintendoScaleMode::Freeze:
        *out_scaling_mode = ConvertedScaleMode::Freeze;
        R_SUCCEED();
    case NintendoScaleMode::ScaleToWindow:
        *out_scaling_mode = ConvertedScaleMode::ScaleToWindow;
        R_SUCCEED();
    case NintendoScaleMode::ScaleAndCrop:
        *out_scaling_mode = ConvertedScaleMode::ScaleAndCrop;
        R_SUCCEED();
    case NintendoScaleMode::PreserveAspectRatio:
        *out_scaling_mode = ConvertedScaleMode::PreserveAspectRatio;
        R_SUCCEED();
    default:
        LOG_ERROR(Service_VI, "Invalid scaling mode specified, mode={}", mode);
        R_THROW(VI::ResultOperationFailed);
    }
}

Result IApplicationDisplayService::Cmd2103(Out<std::array<u8, 0x18>> out_unk18) {
    LOG_WARNING(Service_VI, "(stubbed)");
    R_SUCCEED();
}

Result IApplicationDisplayService::GetIndirectLayerImageMap(
    Out<u64> out_size, Out<u64> out_stride,
    OutBuffer<BufferAttr_HipcMapTransferAllowsNonSecure | BufferAttr_HipcMapAlias> out_buffer,
    s64 width, s64 height, u64 indirect_layer_consumer_handle, ClientAppletResourceUserId aruid) {
    LOG_WARNING(
        Service_VI,
        "(STUBBED) called, width={}, height={}, indirect_layer_consumer_handle={}, aruid={:#x}",
        width, height, indirect_layer_consumer_handle, aruid.pid);
    *out_size = 0;
    *out_stride = 0;
    R_SUCCEED();
}

Result IApplicationDisplayService::GetIndirectLayerImageRequiredMemoryInfo(Out<s64> out_size,
                                                                           Out<s64> out_alignment,
                                                                           s64 width, s64 height) {
    LOG_DEBUG(Service_VI, "called width={}, height={}", width, height);

    constexpr u64 base_size = 0x20000;
    const auto texture_size = width * height * 4;

    *out_alignment = 0x1000;
    *out_size = (texture_size + base_size - 1) / base_size * base_size;

    R_SUCCEED();
}

} // namespace Service::VI
