// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/hle/service/cmif_serialization.h"
#include "core/hle/service/nvnflinger/hos_binder_driver.h"
#include "core/hle/service/vi/application_display_service.h"
#include "core/hle/service/vi/application_root_service.h"
#include "core/hle/service/vi/container.h"
#include "core/hle/service/vi/manager_display_service.h"
#include "core/hle/service/vi/service_creator.h"
#include "core/hle/service/vi/system_display_service.h"
#include "core/hle/service/vi/vi.h"
#include "core/hle/service/vi/vi_types.h"

namespace Service::VI {

IApplicationRootService::IApplicationRootService(Core::System& system_,
                                                 std::shared_ptr<Container> container)
    : ServiceFramework{system_, "vi:u"}, m_container{std::move(container)} {
    static const FunctionInfo functions[] = {
        {0, C<&IApplicationRootService::GetDisplayService>, "GetDisplayService"},
        {1, nullptr, "GetDisplayServiceWithProxyNameExchange"},
    };
    RegisterHandlers(functions);
}

IApplicationRootService::~IApplicationRootService() = default;

SessionRequestHandlerPtr IApplicationRootService::PulseRestore(std::string_view kind,
                                                Pulse::HandlerRestore& restore) {
    if (kind == "IApplicationDisplayService") {
        return IApplicationDisplayService::PulseRestoreFrom(system, m_container, restore);
    }
    if (kind == "ISystemDisplayService") {
        return ISystemDisplayService::PulseRestoreFrom(system, m_container, restore);
    }
    if (kind == "IManagerDisplayService") {
        return IManagerDisplayService::PulseRestoreFrom(system, m_container, restore);
    }
    if (kind == "IHOSBinderDriver") {
        std::shared_ptr<Nvnflinger::IHOSBinderDriver> driver;
        m_container->GetBinderDriver(&driver);
        return driver->PulseRestore(kind, restore);
    }
    return SessionRequestHandler::PulseRestore(kind, restore);
}

Result IApplicationRootService::GetDisplayService(
    Out<SharedPointer<IApplicationDisplayService>> out_application_display_service, Policy policy) {
    LOG_DEBUG(Service_VI, "called");
    R_RETURN(GetApplicationDisplayService(out_application_display_service, system, m_container,
                                          Permission::User, policy));
}

} // namespace Service::VI
