// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <locale>
#include "common/hex_util.h"
#include "common/swap.h"
#include "core/arm/debug.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/hle/kernel/k_page_table.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_process_page_table.h"
#include "core/hle/kernel/svc_types.h"
#include "core/hle/service/hid/hid_server.h"
#include "core/hle/service/sm/sm.h"
#include "core/memory.h"
#include "core/memory/cheat_engine.h"
#include "hid_core/resource_manager.h"
#include "hid_core/resources/npad/npad.h"

namespace Core::Memory {
namespace {
constexpr auto CHEAT_ENGINE_NS = std::chrono::nanoseconds{1000000000 / 12};
} // Anonymous namespace

StandardVmCallbacks::StandardVmCallbacks(System& system_, const CheatProcessMetadata& metadata_)
    : metadata{metadata_}, system{system_} {}

StandardVmCallbacks::~StandardVmCallbacks() = default;

Kernel::KProcess* StandardVmCallbacks::GetProcess() const {
    if (cached_process != nullptr && cached_process_id == metadata.process_id) {
        return cached_process;
    }

    auto process = system.Kernel().GetProcessByProcessId(metadata.process_id);
    cached_process = process.IsNull() ? nullptr : process.GetPointerUnsafe();
    cached_process_id = metadata.process_id;
    return cached_process;
}

void StandardVmCallbacks::MemoryReadUnsafe(VAddr address, void* data, u64 size) {
    auto* const process = this->GetProcess();

    // Return zero on invalid address
    if (process == nullptr || !IsAddressInRange(address) ||
        !process->GetMemory().IsValidVirtualAddress(address)) {
        std::memset(data, 0, size);
        return;
    }

    process->GetMemory().ReadBlock(address, data, size);
}

void StandardVmCallbacks::MemoryWriteUnsafe(VAddr address, const void* data, u64 size) {
    auto* const process = this->GetProcess();

    // Skip invalid memory write address
    if (process == nullptr || !IsAddressInRange(address) ||
        !process->GetMemory().IsValidVirtualAddress(address)) {
        return;
    }

    if (process->GetMemory().WriteBlock(address, data, size)) {
        Core::InvalidateInstructionCacheRange(process, address, size);
    }
}

u64 StandardVmCallbacks::HidKeysDown() {
    const auto hid = system.ServiceManager().GetService<Service::HID::IHidServer>("hid");
    if (hid == nullptr) {
        LOG_WARNING(CheatEngine, "Attempted to read input state, but hid is not initialized!");
        return 0;
    }

    const auto applet_resource = hid->GetResourceManager();
    if (applet_resource == nullptr || applet_resource->GetNpad() == nullptr) {
        LOG_WARNING(CheatEngine,
                    "Attempted to read input state, but applet resource is not initialized!");
        return 0;
    }

    const auto press_state = applet_resource->GetNpad()->GetAndResetPressState();
    return static_cast<u64>(press_state & HID::NpadButton::All);
}

void StandardVmCallbacks::PauseProcess() {
    auto* const process = this->GetProcess();
    if (process != nullptr && !process->IsSuspended()) {
        process->SetActivity(system.Kernel(), Kernel::Svc::ProcessActivity::Paused);
    }
}

void StandardVmCallbacks::ResumeProcess() {
    auto* const process = this->GetProcess();
    if (process != nullptr && process->IsSuspended()) {
        process->SetActivity(system.Kernel(), Kernel::Svc::ProcessActivity::Runnable);
    }
}

void StandardVmCallbacks::DebugLog(u8 id, u64 value) {
    LOG_INFO(CheatEngine, "Cheat triggered DebugLog: ID '{:01X}' Value '{:016X}'", id, value);
}

void StandardVmCallbacks::CommandLog(std::string_view data) {
    LOG_DEBUG(CheatEngine, "[DmntCheatVm]: {}",
              data.back() == '\n' ? data.substr(0, data.size() - 1) : data);
}

bool StandardVmCallbacks::IsAddressInRange(VAddr in) const {
    if ((in < metadata.main_nso_extents.base ||
         in >= metadata.main_nso_extents.base + metadata.main_nso_extents.size) &&
        (in < metadata.heap_extents.base ||
         in >= metadata.heap_extents.base + metadata.heap_extents.size) &&
        (in < metadata.alias_extents.base ||
         in >= metadata.alias_extents.base + metadata.alias_extents.size) &&
        (in < metadata.aslr_extents.base ||
         in >= metadata.aslr_extents.base + metadata.aslr_extents.size)) {
        LOG_DEBUG(CheatEngine,
                  "Cheat attempting to access memory at invalid address={:016X}, if this "
                  "persists, "
                  "the cheat may be incorrect. However, this may be normal early in execution if "
                  "the game has not properly set up yet.",
                  in);
        return false; ///< Invalid addresses will hard crash
    }

    return true;
}

CheatEngine::CheatEngine(System& system_, std::vector<CheatEntry> cheats_,
                         const std::array<u8, 0x20>& build_id_)
    : vm{std::make_unique<StandardVmCallbacks>(system_, metadata)},
      cheats(std::move(cheats_)), core_timing{system_.CoreTiming()}, system{system_} {
    metadata.main_nso_build_id = build_id_;
}

CheatEngine::~CheatEngine() {
    if (event)
        core_timing.UnscheduleEvent(event);
    else
        LOG_ERROR(CheatEngine, "~CheatEngine before event was registered");
}

void CheatEngine::Initialize() {
    event = Core::Timing::CreateEvent(
        "CheatEngine::FrameCallback::" + Common::HexToString(metadata.main_nso_build_id),
        [this](s64 time, std::chrono::nanoseconds ns_late) -> std::optional<std::chrono::nanoseconds> {
            FrameCallback(ns_late);
            return std::nullopt;
        });
    core_timing.ScheduleLoopingEvent(CHEAT_ENGINE_NS, CHEAT_ENGINE_NS, event);

    metadata.process_id = system.ApplicationProcess()->GetProcessId();
    metadata.title_id = system.GetApplicationProcessProgramID();

    const auto& page_table = system.ApplicationProcess()->GetPageTable();
    metadata.heap_extents = {
        .base = GetInteger(page_table.GetHeapRegionStart()),
        .size = page_table.GetHeapRegionSize(),
    };
    metadata.aslr_extents = {
        .base = GetInteger(page_table.GetAliasCodeRegionStart()),
        .size = page_table.GetAliasCodeRegionSize(),
    };
    metadata.alias_extents = {
        .base = GetInteger(page_table.GetAliasRegionStart()),
        .size = page_table.GetAliasRegionSize(),
    };

    is_pending_reload.exchange(true);
}

void CheatEngine::SetMainMemoryParameters(VAddr main_region_begin, u64 main_region_size) {
    metadata.main_nso_extents = {
        .base = main_region_begin,
        .size = main_region_size,
    };
}

void CheatEngine::Reload(std::vector<CheatEntry> reload_cheats) {
    {
        std::scoped_lock lock{cheats_mutex};
        cheats = std::move(reload_cheats);
    }
    is_pending_reload.store(true);
}

void CheatEngine::FrameCallback(std::chrono::nanoseconds ns_late) {
    if (is_pending_reload.exchange(false)) {
        std::scoped_lock lock{cheats_mutex};
        if (!vm.LoadProgram(cheats)) {
            LOG_ERROR(CheatEngine, "Enabled cheats need {} opcodes, more than the VM holds ({})",
                      EnabledOpcodeCount(cheats), DmntCheatVm::MaximumProgramOpcodeCount);
        }
    }

    if (vm.GetProgramSize() == 0) {
        return;
    }

    vm.Execute(metadata);
}

} // namespace Core::Memory
