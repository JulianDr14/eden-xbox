// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "core/hle/service/kernel_helpers.h"
#include "core/hle/service/nfc/nfc_types.h"
#include "core/hle/service/service.h"

namespace Service::Set {
class ISystemSettingsServer;
}

namespace Service::NFC {
class DeviceManager;

class NfcInterface : public ServiceFramework<NfcInterface> {
public:
    explicit NfcInterface(Core::System& system_, const char* name, BackendType service_backend);
    ~NfcInterface();

    void Initialize(HLERequestContext& ctx);
    void Finalize(HLERequestContext& ctx);
    void GetState(HLERequestContext& ctx);
    void IsNfcEnabled(HLERequestContext& ctx);
    void ListDevices(HLERequestContext& ctx);
    void GetDeviceState(HLERequestContext& ctx);
    void GetNpadId(HLERequestContext& ctx);
    void AttachAvailabilityChangeEvent(HLERequestContext& ctx);
    void StartDetection(HLERequestContext& ctx);
    void StopDetection(HLERequestContext& ctx);
    void GetTagInfo(HLERequestContext& ctx);
    void AttachActivateEvent(HLERequestContext& ctx);
    void AttachDeactivateEvent(HLERequestContext& ctx);
    void ReadMifare(HLERequestContext& ctx);
    void SetNfcEnabled(HLERequestContext& ctx);
    void WriteMifare(HLERequestContext& ctx);
    void SendCommandByPassThrough(HLERequestContext& ctx);

    // Pulse: the initialization and each device's detection; a tag in use is not stored (the
    // guest would see it vanish mid-read).
    void PulseCapture(Pulse::HandlerCapture& capture) const override;
    Kernel::KEvent* PulseEvent(u32 slot) override;
    void PulseApply(Pulse::StateReader& in);

protected:
    std::shared_ptr<DeviceManager> GetManager();
    BackendType GetBackendType() const;
    Result TranslateResultToServiceError(Result result) const;
    Result TranslateResultToNfp(Result result) const;
    Result TranslateResultToMifare(Result result) const;

    KernelHelpers::ServiceContext service_context;

    BackendType backend_type;
    State state{State::NonInitialized};
    std::shared_ptr<DeviceManager> device_manager = nullptr;
    std::shared_ptr<Service::Set::ISystemSettingsServer> m_set_sys;
};

// Pulse: the nfc and nfp ports are stateless roots that each hand out one interface kind.
template <typename Interface, typename Root>
SessionRequestHandlerPtr PulseRestoreInterface(Root& root, Core::System& system,
                                               std::string_view kind,
                                               Pulse::HandlerRestore& restore) {
    auto& in = restore.state;
    if (kind == root.PulseKind()) {
        in.End();
        return root.shared_from_this();
    }
    if (kind != Interface::PulseInterfaceKind) {
        return nullptr;
    }
    auto service = std::make_shared<Interface>(system);
    service->PulseApply(in);
    in.End();
    return service;
}

} // namespace Service::NFC
