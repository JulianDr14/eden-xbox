// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "core/hle/service/cmif_types.h"
#include "core/hle/service/service.h"

namespace Core {
class System;
}

namespace Service::VI {

class Container;
class IApplicationDisplayService;
enum class Policy : u32;

class ISystemRootService final : public ServiceFramework<ISystemRootService> {
public:
    explicit ISystemRootService(Core::System& system_, std::shared_ptr<Container> container);
    ~ISystemRootService() override;

    std::string_view PulseKind() const override {
        return "vi:s";
    }
    SessionRequestHandlerPtr PulseRestore(std::string_view kind,
                                          Pulse::HandlerRestore& restore) override;

private:
    Result GetDisplayService(
        Out<SharedPointer<IApplicationDisplayService>> out_application_display_service,
        Policy policy);

    const std::shared_ptr<Container> m_container;
};

} // namespace Service::VI
