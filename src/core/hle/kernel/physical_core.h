// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <memory>
#include <mutex>

#include "common/wake_flag.h"
#include "core/arm/arm_interface.h"

namespace Kernel {
class KernelCore;
} // namespace Kernel

namespace Core {
class ExclusiveMonitor;
class System;
} // namespace Core

namespace Kernel {

class PhysicalCore {
public:
    PhysicalCore(KernelCore& kernel, std::size_t core_index);
    ~PhysicalCore();

    YUZU_NON_COPYABLE(PhysicalCore);
    YUZU_NON_MOVEABLE(PhysicalCore);

    // Execute guest code running on the given thread.
    void RunThread(KernelCore& kernel, KThread* thread);

    // Copy context from thread to current core.
    void LoadContext(const KThread* thread);
    void LoadSvcArguments(const KProcess& process, std::span<const uint64_t, 8> args);

    // Copy context from current core to thread.
    void SaveContext(KThread* thread) const;
    void SaveSvcArguments(KProcess& process, std::span<uint64_t, 8> args) const;

    // Copy floating point status registers to the target thread.
    void CloneFpuStatus(KThread* dst) const;

    // Log backtrace of current processor state.
    void LogBacktrace(KernelCore& kernel);

    // Wait for an interrupt.
    void Idle();

    /// How idle cores wait for an interrupt; read when the cores are created. Guest threads hand
    /// work between cores thousands of times a second (a mutex unlocked on one core wakes its
    /// waiter on another), so a core that spins briefly before blocking takes the handoff without
    /// the OS wake latency.
    static void SetIdleSpinPolicy(Common::SpinPolicy policy);

    // Interrupt this core.
    void Interrupt();

    // Clear this core's interrupt.
    void ClearInterrupt();

    // Check if this core is interrupted.
    bool IsInterrupted() const;

    std::size_t CoreIndex() const {
        return m_core_index;
    }

private:
    const std::size_t m_core_index;
    /// Raised by Interrupt, cleared by this core; Idle waits on it.
    Common::WakeFlag m_interrupt;
    std::mutex m_guard;
    Core::ArmInterface* m_arm_interface{};
    KThread* m_current_thread{};
    bool m_is_single_core{};
};

} // namespace Kernel
