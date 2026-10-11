// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <vector>
#include "common/common_types.h"
#include "core/memory/cheat_text.h"
#include "core/memory/dmnt_cheat_types.h"
#include "core/memory/dmnt_cheat_vm.h"

namespace Core {
class System;
}

namespace Kernel {
class KProcess;
}

namespace Core::Timing {
class CoreTiming;
struct EventType;
} // namespace Core::Timing

namespace Core::Memory {

class StandardVmCallbacks : public DmntCheatVm::Callbacks {
public:
    StandardVmCallbacks(System& system_, const CheatProcessMetadata& metadata_);
    ~StandardVmCallbacks() override;

    void MemoryReadUnsafe(VAddr address, void* data, u64 size) override;
    void MemoryWriteUnsafe(VAddr address, const void* data, u64 size) override;
    u64 HidKeysDown() override;
    void PauseProcess() override;
    void ResumeProcess() override;
    void DebugLog(u8 id, u64 value) override;
    void CommandLog(std::string_view data) override;

private:
    bool IsAddressInRange(VAddr address) const;

    Kernel::KProcess* GetProcess() const;

    const CheatProcessMetadata& metadata;
    Core::System& system;
    mutable Kernel::KProcess* cached_process{};
    mutable u64 cached_process_id{};
};

// Class that encapsulates a CheatList and manages its interaction with memory and CoreTiming
class CheatEngine final {
public:
    CheatEngine(System& system_, std::vector<CheatEntry> cheats_,
                const std::array<u8, 0x20>& build_id_);
    ~CheatEngine();

    void Initialize();
    void SetMainMemoryParameters(VAddr main_region_begin, u64 main_region_size);

    /// Replaces the cheat list; the VM loads it on its next tick. Safe from any thread.
    void Reload(std::vector<CheatEntry> reload_cheats);

private:
    void FrameCallback(std::chrono::nanoseconds ns_late);

    DmntCheatVm vm;
    CheatProcessMetadata metadata;

    std::mutex cheats_mutex; ///< Guards `cheats` between Reload and the timing thread.
    std::vector<CheatEntry> cheats;
    std::atomic_bool is_pending_reload{false};

    std::shared_ptr<Core::Timing::EventType> event;
    Core::Timing::CoreTiming& core_timing;
    Core::System& system;
};

} // namespace Core::Memory
