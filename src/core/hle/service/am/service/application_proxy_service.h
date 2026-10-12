// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "core/hle/service/cmif_types.h"
#include "core/hle/service/am/am_pulse.h"
#include "core/hle/service/service.h"

namespace Service {

namespace AM {

struct Applet;
class IApplicationProxy;
class WindowSystem;

class IApplicationProxyService final : public ServiceFramework<IApplicationProxyService> {
public:
    explicit IApplicationProxyService(Core::System& system_, WindowSystem& window_system);
    ~IApplicationProxyService() override;

    // Pulse: the application applet's state and the events its interfaces hand out; rebuilds
    // those interfaces, which all reference this root (am_pulse.h).
    std::string_view PulseKind() const override {
        return "IApplicationProxyService";
    }
    void PulseCapture(Pulse::HandlerCapture& capture) const override;
    SessionRequestHandlerPtr PulseRestore(std::string_view kind,
                                          Pulse::HandlerRestore& restore) override;
    Kernel::KEvent* PulseEvent(u32 slot) override;

private:
    Result OpenApplicationProxy(Out<SharedPointer<IApplicationProxy>> out_application_proxy,
                                ClientProcessId pid, InCopyHandle<Kernel::KProcess> process_handle);

private:
    std::shared_ptr<Applet> GetAppletFromProcessId(ProcessId pid);

    WindowSystem& m_window_system;
    std::shared_ptr<Applet> m_pulse_applet;
};

} // namespace AM
} // namespace Service
