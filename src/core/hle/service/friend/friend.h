// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "core/hle/service/service.h"

namespace Core {
class System;
}

namespace Service::Friend {

class Module final {
public:
    class Interface : public ServiceFramework<Interface> {
    public:
        explicit Interface(std::shared_ptr<Module> module_, Core::System& system_,
                           const char* name);
        ~Interface() override;

        // Pulse: a stateless root shared by the friend ports; the module holds nothing.
        std::string_view PulseKind() const override {
            return "friend";
        }
        SessionRequestHandlerPtr PulseRestore(std::string_view kind,
                                              Pulse::HandlerRestore& restore) override;

        void CreateFriendService(HLERequestContext& ctx);
        void CreateNotificationService(HLERequestContext& ctx);

    protected:
        std::shared_ptr<Module> module;
    };
};

void LoopProcess(Core::System& system);

} // namespace Service::Friend
