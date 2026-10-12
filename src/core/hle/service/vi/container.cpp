// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <stdexcept>

#include "core/core.h"
#include "video_core/frame_trace.h"
#include "core/hle/service/nvdrv/nvdrv_interface.h"
#include "core/hle/service/nvnflinger/buffer_queue_producer.h"
#include "core/hle/service/nvnflinger/hos_binder_driver.h"
#include "core/hle/service/nvnflinger/hos_binder_driver_server.h"
#include "core/hle/service/nvnflinger/display.h"
#include "core/hle/service/nvnflinger/surface_flinger.h"
#include "core/hle/service/pulse_service.h"
#include "core/hle/service/sm/sm.h"
#include "core/hle/service/vi/container.h"
#include "core/hle/service/vi/vi_results.h"

namespace Service::VI {

Container::Container(Core::System& system) {
    m_displays.CreateDisplay(DisplayName{"Default"});
    m_displays.CreateDisplay(DisplayName{"External"});
    m_displays.CreateDisplay(DisplayName{"Edid"});
    m_displays.CreateDisplay(DisplayName{"Internal"});
    m_displays.CreateDisplay(DisplayName{"Null"});

    m_binder_driver =
        system.ServiceManager().GetService<Nvnflinger::IHOSBinderDriver>("dispdrv", true);
    m_surface_flinger = m_binder_driver->GetSurfaceFlinger();

    const auto nvdrv =
        system.ServiceManager().GetService<Nvidia::NVDRV>("nvdrv:s", true)->GetModule();
    m_shared_buffer_manager.emplace(system, *this, nvdrv);

    m_displays.ForEachDisplay(
        [&](auto& display) { m_surface_flinger->AddDisplay(display.GetId()); });

    m_conductor.emplace(system, *this, m_displays);
}

Container::~Container() {
    this->OnTerminate();
}

void Container::OnTerminate() {
    std::scoped_lock lk{m_lock};

    m_is_shut_down = true;

    m_layers.ForEachLayer([&](auto& layer) { this->DestroyLayerLocked(layer.GetId()); });

    m_displays.ForEachDisplay(
        [&](auto& display) { m_surface_flinger->RemoveDisplay(display.GetId()); });
}

SharedBufferManager* Container::GetSharedBufferManager() {
    return std::addressof(*m_shared_buffer_manager);
}

Result Container::GetBinderDriver(
    std::shared_ptr<Nvnflinger::IHOSBinderDriver>* out_binder_driver) {
    *out_binder_driver = m_binder_driver;
    R_SUCCEED();
}

Result Container::GetLayerProducerHandle(
    std::shared_ptr<android::BufferQueueProducer>* out_producer, u64 layer_id) {
    std::scoped_lock lk{m_lock};

    auto* const layer = m_layers.GetLayerById(layer_id);
    R_UNLESS(layer != nullptr, VI::ResultNotFound);

    const auto binder = m_binder_driver->GetServer()->TryGetBinder(layer->GetProducerBinderId());
    R_UNLESS(binder != nullptr, VI::ResultNotFound);

    *out_producer = std::static_pointer_cast<android::BufferQueueProducer>(binder);
    R_SUCCEED();
}

Result Container::OpenDisplay(u64* out_display_id, const DisplayName& display_name) {
    auto* const display = m_displays.GetDisplayByName(display_name);
    R_UNLESS(display != nullptr, VI::ResultNotFound);

    *out_display_id = display->GetId();
    R_SUCCEED();
}

Result Container::CloseDisplay(u64 display_id) {
    R_SUCCEED();
}

Result Container::CreateManagedLayer(u64* out_layer_id, u64 display_id, u64 owner_aruid) {
    std::scoped_lock lk{m_lock};
    R_RETURN(this->CreateLayerLocked(out_layer_id, display_id, owner_aruid));
}

Result Container::DestroyManagedLayer(u64 layer_id) {
    std::scoped_lock lk{m_lock};

    // Try to close, if open, but don't fail if not.
    this->CloseLayerLocked(layer_id);

    R_RETURN(this->DestroyLayerLocked(layer_id));
}

Result Container::OpenLayer(s32* out_producer_binder_id, u64 layer_id, u64 aruid) {
    std::scoped_lock lk{m_lock};
    R_RETURN(this->OpenLayerLocked(out_producer_binder_id, layer_id, aruid));
}

Result Container::CloseLayer(u64 layer_id) {
    std::scoped_lock lk{m_lock};
    R_RETURN(this->CloseLayerLocked(layer_id));
}

Result Container::SetLayerVisibility(u64 layer_id, bool visible) {
    std::scoped_lock lk{m_lock};

    auto* const layer = m_layers.GetLayerById(layer_id);
    R_UNLESS(layer != nullptr, VI::ResultNotFound);

    m_surface_flinger->SetLayerVisibility(layer->GetConsumerBinderId(), visible);
    R_SUCCEED();
}

Result Container::SetLayerBlending(u64 layer_id, bool enabled) {
    std::scoped_lock lk{m_lock};

    auto* const layer = m_layers.GetLayerById(layer_id);
    R_UNLESS(layer != nullptr, VI::ResultNotFound);

    m_surface_flinger->SetLayerBlending(layer->GetConsumerBinderId(),
                                        enabled ? Nvnflinger::LayerBlending::Coverage
                                                : Nvnflinger::LayerBlending::None);
    R_SUCCEED();
}

Result Container::SetLayerZIndex(u64 layer_id, s32 z_index) {
    std::scoped_lock lk{m_lock};

    auto* const layer = m_layers.GetLayerById(layer_id);
    R_UNLESS(layer != nullptr, VI::ResultNotFound);

    if (auto layer_ref = m_surface_flinger->FindLayer(layer->GetConsumerBinderId())) {
        LOG_DEBUG(Service_VI, "called, SetLayerZIndex layer_id={} z={} (cid={})", layer_id,
                 z_index, layer->GetConsumerBinderId());
        layer_ref->z_index = z_index;
    } else {
        LOG_DEBUG(Service_VI, "called, SetLayerZIndex failed to find layer for layer_id={} (cid={})",
                 layer_id, layer->GetConsumerBinderId());
    }

    R_SUCCEED();
}

Result Container::GetLayerZIndex(u64 layer_id, s32* out_z_index) {
    std::scoped_lock lk{m_lock};

    auto* const layer = m_layers.GetLayerById(layer_id);
    R_UNLESS(layer != nullptr, VI::ResultNotFound);

    if (auto layer_ref = m_surface_flinger->FindLayer(layer->GetConsumerBinderId())) {
        *out_z_index = layer_ref->z_index;
        R_SUCCEED();
    }

    R_RETURN(VI::ResultNotFound);
}

Result Container::SetLayerStackMask(u64 layer_id, u32 layer_stack_mask) {
    std::scoped_lock lk{m_lock};

    auto* const layer = m_layers.GetLayerById(layer_id);
    R_UNLESS(layer != nullptr, VI::ResultNotFound);

    m_surface_flinger->SetLayerStackMask(layer->GetConsumerBinderId(), layer_stack_mask);
    R_SUCCEED();
}

Result Container::SetLayerIsOverlay(u64 layer_id, bool is_overlay) {
    std::scoped_lock lk{m_lock};

    auto* const layer = m_layers.GetLayerById(layer_id);
    R_UNLESS(layer != nullptr, VI::ResultNotFound);

    m_surface_flinger->SetLayerIsOverlay(layer->GetConsumerBinderId(), is_overlay);
    R_SUCCEED();
}

std::shared_ptr<android::BufferQueueProducer> Container::PulseCreateStrayLayer(
    u64 layer_id, u64 display_id, s32 consumer_binder_id, s32 producer_binder_id) {
    return this->PulseCreateLayer(layer_id, display_id, consumer_binder_id, producer_binder_id, {},
                                  true);
}

std::shared_ptr<android::BufferQueueProducer> Container::PulseCreateLayer(
    u64 layer_id, u64 display_id, s32 consumer_binder_id, s32 producer_binder_id, u64 owner_aruid,
    bool open) {
    std::scoped_lock lk{m_lock};
    const auto server = m_binder_driver->GetServer();
    if (layer_id == 0 || m_layers.GetLayerById(layer_id) ||
        producer_binder_id != consumer_binder_id + 1 || consumer_binder_id <= 0 ||
        server->TryGetBinder(consumer_binder_id) || server->TryGetBinder(producer_binder_id)) {
        throw std::invalid_argument("Pulse: vi layer ids taken");
    }
    const auto last_binder = server->PulseLastId();
    const auto next_layer = m_layers.PulseNextId();
    server->PulseSetLastId(consumer_binder_id - 1);
    m_layers.PulseSetNextId(layer_id - 1);
    u64 created_layer{};
    s32 created_producer{};
    bool ok = this->CreateLayerLocked(&created_layer, display_id, owner_aruid).IsSuccess();
    if (ok && open) {
        ok = this->OpenLayerLocked(&created_producer, created_layer, owner_aruid).IsSuccess();
    } else if (ok) {
        created_producer = m_layers.GetLayerById(created_layer)->GetProducerBinderId();
    }
    server->PulseSetLastId((std::max)(last_binder, server->PulseLastId()));
    m_layers.PulseSetNextId((std::max)(next_layer, m_layers.PulseNextId()));
    if (!ok || created_layer != layer_id || created_producer != producer_binder_id) {
        throw std::invalid_argument("Pulse: vi layer not recreated with its ids");
    }
    return std::static_pointer_cast<android::BufferQueueProducer>(
        server->TryGetBinder(producer_binder_id));
}

std::optional<Container::PulseLayer> Container::PulseGetLayer(u64 layer_id) {
    std::scoped_lock lk{m_lock};
    auto* const layer = m_layers.GetLayerById(layer_id);
    if (!layer || !layer->IsOpen() || !layer->GetDisplay()) return std::nullopt;
    const auto flinger_layer = m_surface_flinger->FindLayer(layer->GetConsumerBinderId());
    if (!flinger_layer) return std::nullopt;
    return PulseLayer{
        .display_id = layer->GetDisplay()->GetId(),
        .consumer_binder_id = layer->GetConsumerBinderId(),
        .producer_binder_id = layer->GetProducerBinderId(),
        .visible = flinger_layer->visible,
        .blending = static_cast<u32>(flinger_layer->blending),
        .z_index = flinger_layer->z_index,
        .is_overlay = flinger_layer->is_overlay,
        .layer_stack_mask = flinger_layer->layer_stack_mask,
    };
}

void Container::PulseSetLayer(u64 layer_id, const PulseLayer& state) {
    std::scoped_lock lk{m_lock};
    auto* const layer = m_layers.GetLayerById(layer_id);
    const auto flinger_layer =
        layer ? m_surface_flinger->FindLayer(layer->GetConsumerBinderId()) : nullptr;
    if (!flinger_layer) throw std::invalid_argument("Pulse: vi layer lost");
    flinger_layer->visible = state.visible;
    flinger_layer->blending = static_cast<Nvnflinger::LayerBlending>(state.blending);
    flinger_layer->z_index = state.z_index;
    flinger_layer->is_overlay = state.is_overlay;
    flinger_layer->layer_stack_mask = state.layer_stack_mask;
}

void Container::PulseSaveLayer(u64 layer_id, Pulse::StateWriter& out) {
    std::shared_ptr<android::BufferQueueProducer> producer;
    {
        std::scoped_lock lk{m_lock};
        auto* const layer = m_layers.GetLayerById(layer_id);
        const auto flinger_layer =
            layer ? m_surface_flinger->FindLayer(layer->GetConsumerBinderId()) : nullptr;
        if (!flinger_layer || !layer->GetDisplay()) {
            throw Pulse::ServiceRejected(false, "a vi managed layer is not on a display");
        }
        const auto server = m_binder_driver->GetServer();
        producer = std::static_pointer_cast<android::BufferQueueProducer>(
            server->TryGetBinder(layer->GetProducerBinderId()));
        if (!producer) throw Pulse::ServiceRejected(false, "a vi layer lost its producer binder");
        out.U64(layer_id);
        out.U64(layer->GetOwnerAruid());
        out.U64(layer->GetDisplay()->GetId());
        out.S32(layer->GetConsumerBinderId());
        out.S32(layer->GetProducerBinderId());
        out.Bool(flinger_layer->visible);
        out.U32(static_cast<u32>(flinger_layer->blending));
        out.S32(flinger_layer->z_index);
        out.Bool(flinger_layer->is_overlay);
        out.U32(flinger_layer->layer_stack_mask);
        for (const s32 id : {layer->GetConsumerBinderId(), layer->GetProducerBinderId()}) {
            const auto [strong, weak] = server->PulseRefcounts(id);
            out.S32(strong);
            out.S32(weak);
        }
    }
    Pulse::StateWriter queue;
    producer->PulseCapture(queue);
    out.U32(static_cast<u32>(queue.bytes.size()));
    out.Bytes(queue.bytes);
}

u64 Container::PulseLoadLayer(Pulse::StateReader& in) {
    const u64 layer_id = in.U64();
    const u64 owner_aruid = in.U64();
    PulseLayer layer{};
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
    std::vector<u8> queue(in.Count(1));
    in.Bytes(queue);

    const auto producer =
        this->PulseCreateLayer(layer_id, layer.display_id, layer.consumer_binder_id,
                               layer.producer_binder_id, owner_aruid, false);
    this->PulseSetLayer(layer_id, layer);
    Pulse::StateReader queue_in{queue};
    producer->PulseRestore(queue_in);

    std::scoped_lock lk{m_lock};
    const auto server = m_binder_driver->GetServer();
    server->PulseSetRefcounts(layer.consumer_binder_id, counts[0], counts[1]);
    server->PulseSetRefcounts(layer.producer_binder_id, counts[2], counts[3]);
    return layer_id;
}

void Container::PulseOpenLayer(u64 layer_id) {
    std::scoped_lock lk{m_lock};
    auto* const layer = m_layers.GetLayerById(layer_id);
    s32 producer_binder_id{};
    if (!layer ||
        this->OpenLayerLocked(&producer_binder_id, layer_id, layer->GetOwnerAruid()).IsError()) {
        throw std::invalid_argument("Pulse: vi managed layer not reopened");
    }
}

std::pair<u64, s32> Container::PulseNextIds() {
    std::scoped_lock lk{m_lock};
    return {m_layers.PulseNextId(), m_binder_driver->GetServer()->PulseLastId()};
}

void Container::PulseRaiseNextIds(u64 layer_id, s32 binder_id) {
    std::scoped_lock lk{m_lock};
    const auto server = m_binder_driver->GetServer();
    m_layers.PulseSetNextId((std::max)(layer_id, m_layers.PulseNextId()));
    server->PulseSetLastId((std::max)(binder_id, server->PulseLastId()));
}

void Container::LinkVsyncEvent(u64 display_id, Event* event) {
    std::scoped_lock lk{m_lock};
    m_conductor->LinkVsyncEvent(display_id, event);
}

void Container::UnlinkVsyncEvent(u64 display_id, Event* event) {
    std::scoped_lock lk{m_lock};
    m_conductor->UnlinkVsyncEvent(display_id, event);
}

Result Container::CreateStrayLayer(s32* out_producer_binder_id, u64* out_layer_id, u64 display_id) {
    std::scoped_lock lk{m_lock};
    R_TRY(this->CreateLayerLocked(out_layer_id, display_id, {}));
    R_RETURN(this->OpenLayerLocked(out_producer_binder_id, *out_layer_id, {}));
}

Result Container::DestroyStrayLayer(u64 layer_id) {
    std::scoped_lock lk{m_lock};
    R_TRY(this->CloseLayerLocked(layer_id));
    R_RETURN(this->DestroyLayerLocked(layer_id));
}

Result Container::CreateLayerLocked(u64* out_layer_id, u64 display_id, u64 owner_aruid) {
    auto* const display = m_displays.GetDisplayById(display_id);
    R_UNLESS(display != nullptr, VI::ResultNotFound);

    s32 consumer_binder_id, producer_binder_id;
    m_surface_flinger->CreateBufferQueue(&consumer_binder_id, &producer_binder_id);

    auto* const layer =
        m_layers.CreateLayer(owner_aruid, display, consumer_binder_id, producer_binder_id);
    R_UNLESS(layer != nullptr, VI::ResultNotFound);

    m_surface_flinger->CreateLayer(consumer_binder_id);

    *out_layer_id = layer->GetId();
    R_SUCCEED();
}

Result Container::DestroyLayerLocked(u64 layer_id) {
    auto* const layer = m_layers.GetLayerById(layer_id);
    R_UNLESS(layer != nullptr, VI::ResultNotFound);

    m_surface_flinger->DestroyLayer(layer->GetConsumerBinderId());
    m_surface_flinger->DestroyBufferQueue(layer->GetConsumerBinderId(),
                                          layer->GetProducerBinderId());
    m_layers.DestroyLayer(layer_id);

    R_SUCCEED();
}

Result Container::OpenLayerLocked(s32* out_producer_binder_id, u64 layer_id, u64 aruid) {
    R_UNLESS(!m_is_shut_down, VI::ResultOperationFailed);

    auto* const layer = m_layers.GetLayerById(layer_id);
    R_UNLESS(layer != nullptr, VI::ResultNotFound);
    R_UNLESS(!layer->IsOpen(), VI::ResultOperationFailed);
    R_UNLESS(layer->GetOwnerAruid() == aruid, VI::ResultPermissionDenied);

    layer->Open();

    if (auto* display = layer->GetDisplay(); display != nullptr) {
        m_surface_flinger->AddLayerToDisplayStack(display->GetId(), layer->GetConsumerBinderId());
    }

    *out_producer_binder_id = layer->GetProducerBinderId();

    R_SUCCEED();
}

Result Container::CloseLayerLocked(u64 layer_id) {
    auto* const layer = m_layers.GetLayerById(layer_id);
    R_UNLESS(layer != nullptr, VI::ResultNotFound);
    R_UNLESS(layer->IsOpen(), VI::ResultOperationFailed);

    if (auto* display = layer->GetDisplay(); display != nullptr) {
        m_surface_flinger->RemoveLayerFromDisplayStack(display->GetId(),
                                                       layer->GetConsumerBinderId());
    }

    layer->Close();

    R_SUCCEED();
}

bool Container::ComposeOnDisplay(s32* out_swap_interval, f32* out_compose_speed_scale,
                                 u64 display_id) {
    VideoCore::FrameTrace::ScopedSpan trace_lock{
        VideoCore::FrameTrace::Event::DisplayComposeLock, display_id};
    std::scoped_lock lk{m_lock};
    trace_lock.Finish();
    return m_surface_flinger->ComposeDisplay(out_swap_interval, out_compose_speed_scale,
                                             display_id);
}

} // namespace Service::VI
