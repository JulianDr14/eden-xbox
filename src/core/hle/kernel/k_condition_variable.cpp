// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <utility>
#include "core/arm/exclusive_monitor.h"
#include "core/core.h"
#include "core/hle/kernel/k_condition_variable.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_scheduler.h"
#include "core/hle/kernel/k_scoped_scheduler_lock_and_sleep.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/k_thread_queue.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/svc_common.h"
#include "core/hle/kernel/svc_results.h"
#include "core/memory.h"

namespace Kernel {

namespace {

bool ReadFromUser(KernelCore& kernel, u32* out, KProcessAddress address) {
    *out = GetCurrentMemory(kernel).Read32(GetInteger(address));
    return true;
}

bool WriteToUser(KernelCore& kernel, KProcessAddress address, u32 val) {
    GetCurrentMemory(kernel).Write32(GetInteger(address), val);
    return true;
}

bool UpdateLockAtomic(KernelCore& kernel, u32* out, KProcessAddress address, u32 if_zero,
                      u32 new_orr_mask) {
    auto& monitor = GetCurrentProcess(kernel).GetExclusiveMonitor();
    const auto current_core = kernel.CurrentPhysicalCoreIndex();

    u32 expected{};

    while (true) {
        // Load the value from the address.
        expected = monitor.ExclusiveRead32(current_core, GetInteger(address));

        // Orr in the new mask.
        u32 value = expected | new_orr_mask;

        // If the value is zero, use the if_zero value, otherwise use the newly orr'd value.
        if (!expected) {
            value = if_zero;
        }

        // Try to store.
        if (monitor.ExclusiveWrite32(current_core, GetInteger(address), value)) {
            break;
        }

        // If we failed to store, try again.
    }

    // We're done.
    *out = expected;
    return true;
}

ThreadWaitCheckpoint CaptureConditionWait(const IdentifyWaitObject& identify, const KThread* thread,
                                         KConditionVariable::ThreadTree* tree, KThread* reference_owner) {
    if (!thread || thread->GetIsKernelAddressKey()) {
        throw std::invalid_argument("Pulse: unsupported condition wait");
    }
    const bool condition = tree != nullptr;
    ThreadWaitCheckpoint state{condition ? CheckpointWaitKind::ConditionVariable : CheckpointWaitKind::Mutex,
                               0, {}, GetInteger(thread->GetAddressKey()), 0};
    state.tag = thread->GetAddressKeyValue();
    if (condition) state.condition_key = thread->GetConditionVariableKey();
    if (!condition) {
        if (!identify || !reference_owner || !(state.reference_owner = identify(reference_owner))) {
            throw std::invalid_argument("Pulse: mutex continuation owner has no serializer");
        }
    }
    if (condition && thread->IsWaitingForConditionVariable()) {
        if (thread->GetConditionVariableTree() != tree) {
            throw std::invalid_argument("Pulse: mismatched condition tree");
        }
        for (const auto& waiter : *tree) {
            if (&waiter == thread) return state;
            if (waiter.GetConditionVariableKey() == state.condition_key &&
                waiter.GetPriority() == thread->GetPriority()) ++state.ordinal;
            if (state.ordinal >= 65536) break;
        }
        throw std::invalid_argument("Pulse: condition waiter missing from tree");
    }
    state.objects.push_back(thread->CaptureCheckpointLockOwner(identify));
    return state;
}

class ThreadQueueImplForKConditionVariableWaitForAddress final : public KThreadQueue {
public:
    explicit ThreadQueueImplForKConditionVariableWaitForAddress(KernelCore& kernel)
        : KThreadQueue(kernel)
    {}

    void SetReferenceOwner(KThread* owner) { m_reference_owner = owner; }

    ThreadWaitCheckpoint CaptureCheckpoint(const IdentifyWaitObject& identify, const KThread* thread) const override {
        return CaptureConditionWait(identify, thread, nullptr, m_reference_owner);
    }

    virtual void CancelWait(KernelCore& kernel, KThread* waiting_thread, Result wait_result, bool cancel_timer_task) override {
        // Remove the thread as a waiter from its owner.
        if (auto* owner = waiting_thread->GetLockOwner(kernel)) {
            owner->RemoveWaiter(kernel, waiting_thread);
        }

        // Invoke the base cancel wait handler.
        KThreadQueue::CancelWait(kernel, waiting_thread, wait_result, cancel_timer_task);
    }
private:
    KThread* m_reference_owner{};
};

class ThreadQueueImplForKConditionVariableWaitConditionVariable final : public KThreadQueue {
private:
    KConditionVariable::ThreadTree* m_tree;

public:
    explicit ThreadQueueImplForKConditionVariableWaitConditionVariable(KernelCore& kernel, KConditionVariable::ThreadTree* t)
        : KThreadQueue(kernel)
        , m_tree(t)
    {}

    ThreadWaitCheckpoint CaptureCheckpoint(const IdentifyWaitObject& identify, const KThread* thread) const override {
        return CaptureConditionWait(identify, thread, m_tree, nullptr);
    }

    void CancelWait(KernelCore& kernel, KThread* waiting_thread, Result wait_result, bool cancel_timer_task) override {
        // Remove the thread as a waiter from its owner.
        if (KThread* owner = waiting_thread->GetLockOwner(kernel); owner != nullptr) {
            owner->RemoveWaiter(kernel, waiting_thread);
        }

        // If the thread is waiting on a condvar, remove it from the tree.
        if (waiting_thread->IsWaitingForConditionVariable()) {
            m_tree->erase(m_tree->iterator_to(*waiting_thread));
            waiting_thread->ClearConditionVariable();
        }

        // Invoke the base cancel wait handler.
        KThreadQueue::CancelWait(kernel, waiting_thread, wait_result, cancel_timer_task);
    }
};

// Own only a reconstructible continuation. The current lock owner may change
// while waiting; the original ArbitrateLock stack reference is a separate ID.
class RestoredConditionWait final : public KThreadQueue {
public:
    RestoredConditionWait(KernelCore& kernel, KConditionVariable::ThreadTree* tree,
                          const ThreadWaitCheckpoint& state, const ResolveWaitObject& resolve)
        : KThreadQueue(kernel), m_kernel(kernel), m_tree(tree), m_state(state) {
        const auto resolve_thread = [&](u64 id) {
            auto* object = resolve ? resolve(id) : nullptr;
            auto* thread = object ? object->DynamicCast<KThread*>() : nullptr;
            if (!thread || !thread->GetOwnerProcess()) {
                throw std::invalid_argument("Pulse: unresolved user mutex owner");
            }
            return thread;
        };
        if (!state.objects.empty()) m_owner = resolve_thread(state.objects[0].id);
        if (state.reference_owner) {
            m_reference_owner = resolve_thread(state.reference_owner);
            if (m_owner && ((state.objects[0].id == state.reference_owner) !=
                            (m_owner == m_reference_owner))) {
                throw std::invalid_argument("Pulse: inconsistent mutex owner identity");
            }
            if (!m_reference_owner->Open(kernel)) {
                m_reference_owner = nullptr;
                throw std::invalid_argument("Pulse: dead mutex continuation owner");
            }
        }
    }

    ~RestoredConditionWait() override {
        ASSERT(!m_thread);
        ReleaseCheckpointReferences(m_kernel);
    }

    ThreadWaitCheckpoint CaptureCheckpoint(const IdentifyWaitObject& identify, const KThread* thread) const override {
        return CaptureConditionWait(identify, thread, m_tree, m_reference_owner);
    }

    void LinkCheckpointWait(KThread* thread) override {
        ASSERT(!m_thread);
        if (!thread || (m_reference_owner &&
            m_reference_owner->GetOwnerProcess() != thread->GetOwnerProcess())) {
            throw std::invalid_argument("Pulse: cross-process mutex continuation owner");
        }
        if (m_owner) {
            thread->SetUserAddressKey(m_state.address, m_state.tag);
            // Keep the condition key even after it has moved to a mutex queue.
            if (m_tree) {
                thread->SetConditionVariable(m_tree, m_state.address, m_state.condition_key, m_state.tag);
                thread->ClearConditionVariable();
            }
            m_owner->LinkCheckpointUserWaiter(m_kernel, thread, m_state.objects[0].ordinal);
        } else {
            u64 ordinal{};
            for (const auto& waiter : *m_tree) {
                if (waiter.GetConditionVariableKey() == m_state.condition_key &&
                    waiter.GetPriority() == thread->GetPriority()) ++ordinal;
                if (ordinal >= 65536) break;
            }
            if (ordinal != m_state.ordinal) {
                throw std::invalid_argument("Pulse: condition waiter restore order");
            }
            thread->SetConditionVariable(m_tree, m_state.address, m_state.condition_key, m_state.tag);
            m_tree->insert(*thread);
        }
        m_thread = thread;
    }

    void EndWait(KernelCore& kernel, KThread* thread, Result result) override {
        // SignalToAddress and SignalImpl already unlinked the waiter.
        ASSERT(thread == m_thread && !thread->GetLockOwner(kernel) && !thread->IsWaitingForConditionVariable());
        m_thread = nullptr;
        KThreadQueue::EndWait(kernel, thread, result);
    }

    void CancelWait(KernelCore& kernel, KThread* thread, Result result, bool cancel_timer) override {
        Unlink(kernel, thread);
        KThreadQueue::CancelWait(kernel, thread, result, cancel_timer);
    }

    void AbandonCheckpointWait(KernelCore& kernel, KThread* thread) override {
        Unlink(kernel, thread);
        KThreadQueue::AbandonCheckpointWait(kernel, thread);
    }

    void ReleaseCheckpointReferences(KernelCore& kernel) override {
        ASSERT(!m_thread);
        if (auto* owner = std::exchange(m_reference_owner, nullptr)) owner->Close(kernel);
    }

private:
    void Unlink(KernelCore& kernel, KThread* thread) {
        if (!m_thread) return;
        ASSERT(thread == m_thread);
        if (auto* owner = thread->GetLockOwner(kernel)) owner->RemoveWaiter(kernel, thread);
        if (m_tree && thread->IsWaitingForConditionVariable()) {
            m_tree->erase(m_tree->iterator_to(*thread));
            thread->ClearConditionVariable();
        }
        m_thread = nullptr;
    }

    KernelCore& m_kernel;
    KConditionVariable::ThreadTree* m_tree;
    ThreadWaitCheckpoint m_state;
    KThread* m_owner{};
    KThread* m_reference_owner{};
    KThread* m_thread{};
};

} // namespace

KConditionVariable::KConditionVariable(Core::System& system)
    : m_system{system}
{}

KConditionVariable::~KConditionVariable() = default;

std::unique_ptr<KThreadQueue> KConditionVariable::CreateCheckpointWait(
    KernelCore& kernel, const ThreadWaitCheckpoint& state, const ResolveWaitObject& resolve) {
    ValidateWaitCheckpoint(state);
    if (state.kind != CheckpointWaitKind::Mutex && state.kind != CheckpointWaitKind::ConditionVariable) {
        throw std::invalid_argument("Pulse: invalid condition wait kind");
    }
    return std::make_unique<RestoredConditionWait>(kernel,
        state.kind == CheckpointWaitKind::ConditionVariable ? &m_tree : nullptr, state, resolve);
}

Result KConditionVariable::SignalToAddress(KernelCore& kernel, KProcessAddress addr) {
    KThread* owner_thread = GetCurrentThreadPointer(kernel);

    // Signal the address.
    {
        KScopedSchedulerLock sl(kernel);

        // Remove waiter thread.
        bool has_waiters{};
        KThread* const next_owner_thread = owner_thread->RemoveUserWaiterByKey(kernel, std::addressof(has_waiters), addr);

        // Determine the next tag.
        u32 next_value{};
        if (next_owner_thread != nullptr) {
            next_value = next_owner_thread->GetAddressKeyValue();
            if (has_waiters) {
                next_value |= Svc::HandleWaitMask;
            }
        }

        // Synchronize memory before proceeding.
        std::atomic_thread_fence(std::memory_order_seq_cst);

        // Write the value to userspace.
        Result result{ResultSuccess};
        if (WriteToUser(kernel, addr, next_value)) {
            result = ResultSuccess;
        } else {
            result = ResultInvalidCurrentMemory;
        }

        // If necessary, signal the next owner thread.
        if (next_owner_thread != nullptr) {
            next_owner_thread->EndWait(kernel, result);
        }

        R_RETURN(result);
    }
}

Result KConditionVariable::WaitForAddress(KernelCore& kernel, Handle handle, KProcessAddress addr, u32 value) {
    KThread* cur_thread = GetCurrentThreadPointer(kernel);
    ThreadQueueImplForKConditionVariableWaitForAddress wait_queue(kernel);

    // Wait for the address.
    KThread* owner_thread{};
    {
        KScopedSchedulerLock sl(kernel);

        // Check if the thread should terminate.
        R_UNLESS(!cur_thread->IsTerminationRequested(), ResultTerminationRequested);

        // Read the tag from userspace.
        u32 test_tag{};
        R_UNLESS(ReadFromUser(kernel, std::addressof(test_tag), addr), ResultInvalidCurrentMemory);

        // If the tag isn't the handle (with wait mask), we're done.
        R_SUCCEED_IF(test_tag != (handle | Svc::HandleWaitMask));

        // Get the lock owner thread.
        owner_thread = GetCurrentProcess(kernel)
            .GetHandleTable()
            .GetObjectWithoutPseudoHandle<KThread>(kernel, handle)
            .ReleasePointerUnsafe();
        R_UNLESS(owner_thread != nullptr, ResultInvalidHandle);

        wait_queue.SetReferenceOwner(owner_thread);

        // Update the lock.
        cur_thread->SetUserAddressKey(addr, value);
        owner_thread->AddWaiter(kernel, cur_thread);

        // Begin waiting.
        cur_thread->BeginWait(kernel, std::addressof(wait_queue));
        cur_thread->SetWaitReasonForDebugging(ThreadWaitReasonForDebugging::ConditionVar);
    }

    // Close our reference to the owner thread, now that the wait is over.
    owner_thread->Close(kernel);

    // Get the wait result.
    R_RETURN(cur_thread->GetWaitResult());
}

void KConditionVariable::SignalImpl(KernelCore& kernel, KThread* thread) {
    // Check pre-conditions.
    ASSERT(KScheduler::IsSchedulerLockedByCurrentThread(kernel));

    // Update the tag.
    KProcessAddress address = thread->GetAddressKey();
    u32 own_tag = thread->GetAddressKeyValue();

    u32 prev_tag{};
    bool can_access{};
    {
        // NOTE: If scheduler lock is not held here, interrupt disable is required.
        // KScopedInterruptDisable di;

        // TODO(bunnei): We should call CanAccessAtomic(..) here.
        can_access = true;
        if (can_access) {
            UpdateLockAtomic(kernel, std::addressof(prev_tag), address, own_tag, Svc::HandleWaitMask);
        }
    }

    if (can_access) {
        if (prev_tag == Svc::InvalidHandle) {
            // If nobody held the lock previously, we're all good.
            thread->EndWait(kernel, ResultSuccess);
        } else {
            // Get the previous owner.
            KThread* owner_thread = GetCurrentProcess(kernel)
                .GetHandleTable()
                .GetObjectWithoutPseudoHandle<KThread>(kernel, Handle(prev_tag & ~Svc::HandleWaitMask))
                .ReleasePointerUnsafe();

            if (owner_thread) {
                // Add the thread as a waiter on the owner.
                owner_thread->AddWaiter(kernel, thread);
                owner_thread->Close(kernel);
            } else {
                // The lock was tagged with a thread that doesn't exist.
                thread->EndWait(kernel, ResultInvalidState);
            }
        }
    } else {
        // If the address wasn't accessible, note so.
        thread->EndWait(kernel, ResultInvalidCurrentMemory);
    }
}

void KConditionVariable::Signal(u64 cv_key, s32 count) {
    // Perform signaling.
    s32 num_waiters{};
    {
        KScopedSchedulerLock sl(m_system.Kernel());

        auto it = m_tree.nfind_key({cv_key, -1});
        while ((it != m_tree.end()) && (count <= 0 || num_waiters < count) && (it->GetConditionVariableKey() == cv_key)) {
            KThread* target_thread = std::addressof(*it);

            it = m_tree.erase(it);
            target_thread->ClearConditionVariable();

            this->SignalImpl(m_system.Kernel(), target_thread);

            ++num_waiters;
        }

        // If we have no waiters, clear the has waiter flag.
        if (it == m_tree.end() || it->GetConditionVariableKey() != cv_key) {
            constexpr u32 HasNoWaiterFlag = 0;
            WriteToUser(m_system.Kernel(), cv_key, HasNoWaiterFlag);
        }
    }
}

Result KConditionVariable::Wait(KProcessAddress addr, u64 key, u32 value, s64 timeout) {
    // Prepare to wait.
    KThread* cur_thread = GetCurrentThreadPointer(m_system.Kernel());
    KHardwareTimer* timer{};
    ThreadQueueImplForKConditionVariableWaitConditionVariable wait_queue(m_system.Kernel(),
                                                                         std::addressof(m_tree));

    {
        KScopedSchedulerLockAndSleep slp(m_system.Kernel(), std::addressof(timer), cur_thread, timeout);

        // Check that the thread isn't terminating.
        if (cur_thread->IsTerminationRequested()) {
            slp.CancelSleep();
            R_THROW(ResultTerminationRequested);
        }

        // Update the value and process for the next owner.
        {
            // Remove waiter thread.
            bool has_waiters{};
            KThread* next_owner_thread =
                cur_thread->RemoveUserWaiterByKey(m_system.Kernel(), std::addressof(has_waiters), addr);

            // Update for the next owner thread.
            u32 next_value{};
            if (next_owner_thread != nullptr) {
                // Get the next tag value.
                next_value = next_owner_thread->GetAddressKeyValue();
                if (has_waiters) {
                    next_value |= Svc::HandleWaitMask;
                }

                // Wake up the next owner.
                next_owner_thread->EndWait(m_system.Kernel(), ResultSuccess);
            }

            // Write to the cv key.
            {
                constexpr u32 HasWaiterFlag = 1;
                WriteToUser(m_system.Kernel(), key, HasWaiterFlag);
                std::atomic_thread_fence(std::memory_order_seq_cst);
            }

            // Write the value to userspace.
            if (!WriteToUser(m_system.Kernel(), addr, next_value)) {
                slp.CancelSleep();
                R_THROW(ResultInvalidCurrentMemory);
            }
        }

        // If timeout is zero, time out.
        R_UNLESS(timeout != 0, ResultTimedOut);

        // Update condition variable tracking.
        cur_thread->SetConditionVariable(std::addressof(m_tree), addr, key, value);
        m_tree.insert(*cur_thread);

        // Begin waiting.
        wait_queue.SetHardwareTimer(timer);
        cur_thread->BeginWait(m_system.Kernel(), std::addressof(wait_queue));
        cur_thread->SetWaitReasonForDebugging(ThreadWaitReasonForDebugging::ConditionVar);
    }

    // Get the wait result.
    R_RETURN(cur_thread->GetWaitResult());
}

} // namespace Kernel
