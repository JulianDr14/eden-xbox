// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "core/hle/service/cmif_types.h"
#include "core/hle/service/kernel_helpers.h"
#include "core/hle/service/service.h"

namespace Core {
class System;
}

namespace Kernel {
class KEvent;
}

namespace Service::AOC {

class IPurchaseEventManager;

class IAddOnContentManager final : public ServiceFramework<IAddOnContentManager> {
public:
    explicit IAddOnContentManager(Core::System& system);
    ~IAddOnContentManager() override;

    // Pulse: the add-on list is the System's; only the change event is the guest's.
    std::string_view PulseKind() const override {
        return "aoc:u";
    }
    void PulseCapture(Pulse::HandlerCapture& capture) const override {
        capture.ExportEvent(0, aoc_change_event);
    }
    Kernel::KEvent* PulseEvent(u32 slot) override {
        return slot == 0 ? aoc_change_event : nullptr;
    }
    SessionRequestHandlerPtr PulseRestore(std::string_view kind, Pulse::HandlerRestore& restore) override;

    Result CountAddOnContent(Out<u32> out_count, ClientProcessId process_id);
    Result ListAddOnContent(Out<u32> out_count, OutBuffer<BufferAttr_HipcMapAlias> out_addons,
                            u32 offset, u32 count, ClientProcessId process_id);
    Result GetAddOnContentBaseId(Out<u64> out_title_id, ClientProcessId process_id);
    Result PrepareAddOnContent(s32 addon_index, ClientProcessId process_id);
    Result GetAddOnContentListChangedEvent(OutCopyHandle<Kernel::KReadableEvent> out_event);
    Result GetAddOnContentListChangedEventWithProcessId(
        OutCopyHandle<Kernel::KReadableEvent> out_event, ClientProcessId process_id);
    Result NotifyMountAddOnContent();
    Result NotifyUnmountAddOnContent();
    Result CheckAddOnContentMountStatus();
    Result CreateEcPurchasedEventManager(OutInterface<IPurchaseEventManager> out_interface);
    Result CreatePermanentEcPurchasedEventManager(
        OutInterface<IPurchaseEventManager> out_interface);

private:
    std::vector<u64> add_on_content;
    KernelHelpers::ServiceContext service_context;

    Kernel::KEvent* aoc_change_event;
};

void LoopProcess(Core::System& system);

} // namespace Service::AOC
