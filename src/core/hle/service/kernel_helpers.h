// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

namespace Core {
class System;
}

namespace Kernel {
class KernelCore;
class KEvent;
class KProcess;
} // namespace Kernel

namespace Service::KernelHelpers {

class ServiceContext {
public:
    ServiceContext(Core::System& system_, std::string name_);
    ~ServiceContext();

    Kernel::KEvent* CreateEvent(std::string&& name);

    void CloseEvent(Kernel::KEvent* event);

    Kernel::KernelCore& kernel;
    Kernel::KProcess* process{};
    bool process_created{false};
};

// The name a service gave an event it created (empty: unknown). Diagnostics only: Pulse names
// the events it cannot store with it.
std::string EventName(const Kernel::KEvent* event);

// Names an event created outside ServiceContext::CreateEvent; it is forgotten with ForgetEvent.
void NameEvent(const Kernel::KEvent* event, std::string name);
void ForgetEvent(const Kernel::KEvent* event);

// Whether the service that created this event already closed it with CloseEvent: an event the
// guest still holds then has no signaller left, and Pulse stores it as an application event.
bool IsClosedServiceEvent(const Kernel::KEvent* event);

} // namespace Service::KernelHelpers
