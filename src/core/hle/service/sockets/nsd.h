// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "core/hle/service/service.h"

namespace Core {
class System;
}

namespace Service::Sockets {

class NSD final : public ServiceFramework<NSD> {
public:
    explicit NSD(Core::System& system_, const char* name);
    ~NSD() override;

    // Pulse: answers come from the settings; no state.
    std::string_view PulseKind() const override {
        return "nsd";
    }

private:
    void SetChangeEnvironmentIdentifierDisabled(HLERequestContext& ctx);
    void Resolve(HLERequestContext& ctx);
    void ResolveEx(HLERequestContext& ctx);
    void GetEnvironmentIdentifier(HLERequestContext& ctx);
    void GetApplicationServerEnvironmentType(HLERequestContext& ctx);
};

} // namespace Service::Sockets
