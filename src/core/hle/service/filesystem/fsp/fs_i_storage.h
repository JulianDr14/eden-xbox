// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "core/file_sys/vfs/vfs.h"
#include "core/hle/service/cmif_types.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/hle/service/service.h"

namespace Service::FileSystem {

class IStorage final : public ServiceFramework<IStorage> {
public:
    explicit IStorage(Core::System& system_, FileSys::VirtualFile backend_);

    // Pulse: read-only content, reopened on restore by the FSP_SRV call that opened it.
    enum class PulseOrigin : u32 { None, CurrentProcess, DataId, ProgramIndex };
    void SetPulseOrigin(SessionRequestHandlerPtr opener, PulseOrigin origin, std::vector<u8> args) {
        pulse_opener = std::move(opener);
        pulse_origin = origin;
        pulse_args = std::move(args);
    }
    std::string_view PulseKind() const override {
        return "IStorage";
    }
    void PulseCapture(Pulse::HandlerCapture& capture) const override;

private:
    FileSys::VirtualFile backend;
    SessionRequestHandlerPtr pulse_opener;
    PulseOrigin pulse_origin{};
    std::vector<u8> pulse_args;

    Result Read(
        OutBuffer<BufferAttr_HipcMapAlias | BufferAttr_HipcMapTransferAllowsNonSecure> out_bytes,
        s64 offset, s64 length);
    Result GetSize(Out<u64> out_size);
};

} // namespace Service::FileSystem
