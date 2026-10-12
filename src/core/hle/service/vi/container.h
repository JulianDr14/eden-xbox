// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <mutex>
#include <optional>

#include "core/hle/service/vi/conductor.h"
#include "core/hle/service/vi/display_list.h"
#include "core/hle/service/vi/layer_list.h"
#include "core/hle/service/vi/shared_buffer_manager.h"

union Result;

namespace Service::android {
class BufferQueueProducer;
}

namespace Service::Nvnflinger {
class IHOSBinderDriver;
class SurfaceFlinger;
} // namespace Service::Nvnflinger

namespace Service {
class Event;
}

namespace Service::Pulse {
class StateReader;
class StateWriter;
} // namespace Service::Pulse

namespace Service::VI {

class SharedBufferManager;

class Container {
public:
    explicit Container(Core::System& system);
    ~Container();

    void OnTerminate();

    SharedBufferManager* GetSharedBufferManager();

    Result GetBinderDriver(std::shared_ptr<Nvnflinger::IHOSBinderDriver>* out_binder_driver);
    Result GetLayerProducerHandle(std::shared_ptr<android::BufferQueueProducer>* out_producer,
                                  u64 layer_id);

    Result OpenDisplay(u64* out_display_id, const DisplayName& display_name);
    Result CloseDisplay(u64 display_id);

    // Managed layers are created by the interaction between am and ommdisp
    // on behalf of an applet. Their lifetime ends with the lifetime of the
    // applet's ISelfController.
    Result CreateManagedLayer(u64* out_layer_id, u64 display_id, u64 owner_aruid);
    Result DestroyManagedLayer(u64 layer_id);
    Result OpenLayer(s32* out_producer_binder_id, u64 layer_id, u64 aruid);
    Result CloseLayer(u64 layer_id);

    // Stray layers are created by non-applet sysmodules. Their lifetime ends
    // with the lifetime of the IApplicationDisplayService which created them.
    Result CreateStrayLayer(s32* out_producer_binder_id, u64* out_layer_id, u64 display_id);
    Result DestroyStrayLayer(u64 layer_id);

    Result SetLayerVisibility(u64 layer_id, bool visible);
    Result SetLayerBlending(u64 layer_id, bool enabled);
    Result SetLayerZIndex(u64 layer_id, s32 z_index);
    Result GetLayerZIndex(u64 layer_id, s32* out_z_index);
    Result SetLayerIsOverlay(u64 layer_id, bool is_overlay);
    Result SetLayerStackMask(u64 layer_id, u32 layer_stack_mask);

    // Pulse: a stray layer with its captured ids, open on its display, with its producer.
    std::shared_ptr<android::BufferQueueProducer> PulseCreateStrayLayer(u64 layer_id, u64 display_id,
                                                                       s32 consumer_binder_id,
                                                                       s32 producer_binder_id);
    std::shared_ptr<android::BufferQueueProducer> PulseCreateLayer(u64 layer_id, u64 display_id,
                                                                  s32 consumer_binder_id,
                                                                  s32 producer_binder_id,
                                                                  u64 owner_aruid, bool open);
    // Pulse: a managed layer (am creates it for an aruid, vi opens it), saved whole and recreated
    // closed with its ids, properties, binder refcounts and buffer queue.
    void PulseSaveLayer(u64 layer_id, Pulse::StateWriter& out);
    u64 PulseLoadLayer(Pulse::StateReader& in);
    void PulseOpenLayer(u64 layer_id);
    struct PulseLayer {
        u64 display_id;
        s32 consumer_binder_id;
        s32 producer_binder_id;
        bool visible;
        u32 blending;
        s32 z_index;
        bool is_overlay;
        u32 layer_stack_mask;
    };
    std::optional<PulseLayer> PulseGetLayer(u64 layer_id);
    void PulseSetLayer(u64 layer_id, const PulseLayer& state);
    std::pair<u64, s32> PulseNextIds();
    void PulseRaiseNextIds(u64 layer_id, s32 binder_id);

    void LinkVsyncEvent(u64 display_id, Event* event);
    void UnlinkVsyncEvent(u64 display_id, Event* event);

private:
    Result CreateLayerLocked(u64* out_layer_id, u64 display_id, u64 owner_aruid);
    Result DestroyLayerLocked(u64 layer_id);
    Result OpenLayerLocked(s32* out_producer_binder_id, u64 layer_id, u64 aruid);
    Result CloseLayerLocked(u64 layer_id);

public:
    bool ComposeOnDisplay(s32* out_swap_interval, f32* out_compose_speed_scale, u64 display_id);

private:
    std::mutex m_lock{};
    DisplayList m_displays{};
    LayerList m_layers{};
    std::shared_ptr<Nvnflinger::IHOSBinderDriver> m_binder_driver{};
    std::shared_ptr<Nvnflinger::SurfaceFlinger> m_surface_flinger{};
    std::optional<SharedBufferManager> m_shared_buffer_manager{};
    std::optional<Conductor> m_conductor{};
    bool m_is_shut_down{};
};

} // namespace Service::VI
