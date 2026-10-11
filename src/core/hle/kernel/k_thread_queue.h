// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "core/hle/kernel/k_scheduler.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/pulse_wait_state.h"

namespace Kernel {

class KHardwareTimer;

class KThreadQueue {
public:
    explicit KThreadQueue(KernelCore& kernel) : m_hardware_timer{} {}
    virtual ~KThreadQueue() = default;

    void SetHardwareTimer(KHardwareTimer* timer) {
        m_hardware_timer = timer;
    }

    virtual void NotifyAvailable(KernelCore& kernel, KThread* waiting_thread, KSynchronizationObject* signaled_object, Result wait_result);
    virtual void EndWait(KernelCore& kernel, KThread* waiting_thread, Result wait_result);
    virtual void CancelWait(KernelCore& kernel, KThread* waiting_thread, Result wait_result, bool cancel_timer_task);

    // No host stack is captured. Unsupported queue implementations fail closed.
    virtual ThreadWaitCheckpoint CaptureCheckpoint(const IdentifyWaitObject&, const KThread*) const {
        throw std::runtime_error("Pulse: kernel wait continuation is not reconstructible");
    }
    virtual void LinkCheckpointWait(KThread*) {
        throw std::runtime_error("Pulse: kernel wait queue cannot be reconstructed");
    }
    virtual void ReleaseCheckpointReferences(KernelCore&) {}
    // Discard of a fresh reconstruction must not make its guest thread runnable.
    virtual void AbandonCheckpointWait(KernelCore& kernel, KThread* thread);

private:
    KHardwareTimer* m_hardware_timer{};
};

class KThreadQueueWithoutEndWait : public KThreadQueue {
public:
    explicit KThreadQueueWithoutEndWait(KernelCore& kernel) : KThreadQueue(kernel) {}
    void EndWait(KernelCore& kernel, KThread* waiting_thread, Result wait_result) override final;
};

} // namespace Kernel
