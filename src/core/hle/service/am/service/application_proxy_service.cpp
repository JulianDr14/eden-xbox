// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <stdexcept>

#include "core/core.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_readable_event.h"
#include "core/hle/service/am/am.h"
#include "core/hle/service/am/applet.h"
#include "core/hle/service/am/applet_manager.h"
#include "core/hle/service/am/service/application_functions.h"
#include "core/hle/service/am/service/application_proxy.h"
#include "core/hle/service/am/service/application_proxy_service.h"
#include "core/hle/service/am/service/audio_controller.h"
#include "core/hle/service/am/service/common_state_getter.h"
#include "core/hle/service/am/service/debug_functions.h"
#include "core/hle/service/am/service/display_controller.h"
#include "core/hle/service/am/service/library_applet_creator.h"
#include "core/hle/service/am/service/process_winding_controller.h"
#include "core/hle/service/am/service/self_controller.h"
#include "core/hle/service/am/service/storage.h"
#include "core/hle/service/am/service/window_controller.h"
#include "core/hle/service/am/window_system.h"
#include "core/hle/service/cmif_serialization.h"
#include "core/hle/service/sm/sm.h"

namespace Service::AM {

IApplicationProxyService::IApplicationProxyService(Core::System& system_,
                                                   WindowSystem& window_system)
    : ServiceFramework{system_, "appletOE"}, m_window_system{window_system} {
    static const FunctionInfo functions[] = {
        {0, D<&IApplicationProxyService::OpenApplicationProxy>, "OpenApplicationProxy"},
    };
    RegisterHandlers(functions);
}

IApplicationProxyService::~IApplicationProxyService() = default;

Result IApplicationProxyService::OpenApplicationProxy(
    Out<SharedPointer<IApplicationProxy>> out_application_proxy, ClientProcessId pid,
    InCopyHandle<Kernel::KProcess> process_handle) {
    LOG_DEBUG(Service_AM, "called");

    if (const auto applet = this->GetAppletFromProcessId(pid)) {
        *out_application_proxy = std::make_shared<IApplicationProxy>(
            system, applet, process_handle.Get(), m_window_system);
        R_SUCCEED();
    } else {
        UNIMPLEMENTED();
        R_THROW(ResultUnknown);
    }
}

std::shared_ptr<Applet> IApplicationProxyService::GetAppletFromProcessId(ProcessId process_id) {
    return m_window_system.GetByAppletResourceUserId(process_id.pid);
}

namespace {

// The applet events the application's interfaces hand out, under stable slots.
constexpr u32 AppletEventSlots = 15;

Event* AppletEvent(Applet& applet, u32 slot) {
    switch (slot) {
    case 0:
        return &applet.lifecycle_manager.GetSystemEvent();
    case 1:
        return &applet.lifecycle_manager.GetOperationModeChangedSystemEvent();
    case 2:
        return &applet.lifecycle_manager.GetHDCPStateChangedEvent();
    case 3:
        return &applet.sleep_lock_event;
    case 4:
        return &applet.acquired_sleep_lock_event;
    case 5:
        return &applet.library_applet_launchable_event;
    case 6:
        return &applet.accumulated_suspended_tick_changed_event;
    case 7:
        return &applet.gpu_error_detected_event;
    case 8:
        return &applet.friend_invitation_storage_channel_event;
    case 9:
        return &applet.notification_storage_channel_event;
    case 10:
        return &applet.health_warning_disappeared_system_event;
    case 11:
        return &applet.unknown_event;
    case 12:
        return &applet.pop_from_general_channel_event;
    case 13:
        return &applet.state_changed_event;
    case 14:
        return &applet.overlay_event;
    default:
        return nullptr;
    }
}

constexpr std::array<std::string_view, 11> ChildKinds{
    "IApplicationProxy",     "ICommonStateGetter",        "ISelfController",
    "IWindowController",     "IAudioController",          "IDisplayController",
    "IDebugFunctions",       "IProcessWindingController", "ILibraryAppletCreator",
    "IApplicationFunctions", "IStorage",
};

} // namespace

void PulseReferenceRoot(Core::System& system, Pulse::HandlerCapture& capture) {
    const auto root = system.ServiceManager().GetService<IApplicationProxyService>("appletOE");
    if (!root) throw Pulse::ServiceRejected(false, "am: appletOE is not registered");
    capture.state.U64(capture.Reference(root));
}

void IApplicationProxyService::PulseCapture(Pulse::HandlerCapture& capture) const {
    auto* const process = system.ApplicationProcess();
    const auto applet =
        process ? m_window_system.GetByAppletResourceUserId(process->GetProcessId()) : nullptr;
    capture.state.Bool(applet != nullptr);
    if (!applet) return;
    applet->PulseSave(system, capture.state);
    for (u32 slot = 0; slot < AppletEventSlots; ++slot) {
        capture.ExportEvent(slot, AppletEvent(*applet, slot)->GetHandle()->GetParent());
    }
}

SessionRequestHandlerPtr IApplicationProxyService::PulseRestore(std::string_view kind,
                                                                Pulse::HandlerRestore& restore) {
    auto& in = restore.state;
    if (kind == PulseKind()) {
        if (in.Bool()) {
            auto applet = restore.process ? m_window_system.GetByAppletResourceUserId(
                                                restore.process->GetProcessId())
                                          : nullptr;
            if (!applet) throw std::invalid_argument("Pulse: am application applet");
            applet->PulseLoad(system, restore.process, in);
            m_pulse_applet = std::move(applet);
        }
        in.End();
        return shared_from_this();
    }
    if (std::find(ChildKinds.begin(), ChildKinds.end(), kind) == ChildKinds.end()) {
        return SessionRequestHandler::PulseRestore(kind, restore);
    }

    // Every child references this root, restored first with the applet.
    if (restore.Resolve(in.U64()).get() != this || !m_pulse_applet) {
        throw std::invalid_argument("Pulse: am interface without its applet");
    }
    const auto& applet = m_pulse_applet;
    SessionRequestHandlerPtr out;
    if (kind == "IApplicationProxy") {
        out = std::make_shared<IApplicationProxy>(system, applet, restore.process, m_window_system);
    } else if (kind == "ICommonStateGetter") {
        out = std::make_shared<ICommonStateGetter>(system, applet);
    } else if (kind == "ISelfController") {
        out = std::make_shared<ISelfController>(system, applet, restore.process);
    } else if (kind == "IWindowController") {
        out = std::make_shared<IWindowController>(system, applet, m_window_system);
    } else if (kind == "IAudioController") {
        auto audio = std::make_shared<IAudioController>(system);
        audio->PulseLoad(in);
        out = std::move(audio);
    } else if (kind == "IDisplayController") {
        out = std::make_shared<IDisplayController>(system, applet);
    } else if (kind == "IDebugFunctions") {
        out = std::make_shared<IDebugFunctions>(system);
    } else if (kind == "IProcessWindingController") {
        out = std::make_shared<IProcessWindingController>(system, applet);
    } else if (kind == "ILibraryAppletCreator") {
        out = std::make_shared<ILibraryAppletCreator>(system, applet, m_window_system);
    } else if (kind == "IApplicationFunctions") {
        out = std::make_shared<IApplicationFunctions>(system, applet);
    } else {
        std::vector<u8> data(in.Count(1));
        in.Bytes(data);
        out = std::make_shared<IStorage>(system, std::move(data));
    }
    in.End();
    return out;
}

Kernel::KEvent* IApplicationProxyService::PulseEvent(u32 slot) {
    auto* const event = m_pulse_applet ? AppletEvent(*m_pulse_applet, slot) : nullptr;
    return event ? event->GetHandle()->GetParent() : nullptr;
}

} // namespace Service::AM
