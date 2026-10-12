// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <set>

#include "common/scratch_buffer.h"
#include "core/hle/service/nvdrv/nvdrv.h"
#include "core/hle/service/service.h"

namespace Service::Nvidia {

class NVDRV final : public ServiceFramework<NVDRV> {
public:
    explicit NVDRV(Core::System& system_, std::shared_ptr<Module> nvdrv_, const char* name);
    ~NVDRV() override;

    std::shared_ptr<Module> GetModule() const {
        return nvdrv;
    }

    // Pulse: the session in the nvdrv container, the fds this interface opened with their device
    // state, and the session's nvmap handles. Devices without a serializer reject the checkpoint.
    std::string_view PulseKind() const override {
        return "nvdrv";
    }
    void PulseCapture(Pulse::HandlerCapture& capture) const override;
    Kernel::KEvent* PulseEvent(u32 slot) override;
    SessionRequestHandlerPtr PulseRestore(std::string_view kind,
                                          Pulse::HandlerRestore& restore) override;
    bool PulseInitialized() const {
        return is_initialized;
    }

private:
    void Open(HLERequestContext& ctx);
    void Ioctl1(HLERequestContext& ctx);
    void Ioctl2(HLERequestContext& ctx);
    void Ioctl3(HLERequestContext& ctx);
    void Close(HLERequestContext& ctx);
    void Initialize(HLERequestContext& ctx);
    void QueryEvent(HLERequestContext& ctx);
    void SetAruid(HLERequestContext& ctx);
    void SetGraphicsFirmwareMemoryMarginEnabled(HLERequestContext& ctx);
    void GetStatus(HLERequestContext& ctx);
    void DumpGraphicsMemoryInfo(HLERequestContext& ctx);

    void ServiceError(HLERequestContext& ctx, NvResult result);

    std::shared_ptr<Module> nvdrv;

    u64 pid{};
    bool is_initialized{};
    NvCore::SessionId session_id{};
    std::set<DeviceFD> pulse_fds;
    Common::ScratchBuffer<u8> output_buffer;
    Common::ScratchBuffer<u8> inline_output_buffer;
};

} // namespace Service::Nvidia
