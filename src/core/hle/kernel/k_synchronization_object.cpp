// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/assert.h"
#include "common/common_types.h"
#include "common/scratch_buffer.h"
#include "core/hle/kernel/k_scheduler.h"
#include "core/hle/kernel/k_scoped_scheduler_lock_and_sleep.h"
#include "core/hle/kernel/k_synchronization_object.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/k_thread_queue.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/svc_results.h"
#include "video_core/frame_trace.h"

namespace Kernel {

namespace {

class ThreadQueueImplForKSynchronizationObjectWait : public KThreadQueueWithoutEndWait {
public:
    ThreadQueueImplForKSynchronizationObjectWait(KernelCore& kernel, KSynchronizationObject** o, KSynchronizationObject::ThreadListNode* n, s32 c)
        : KThreadQueueWithoutEndWait(kernel), m_objects(o), m_nodes(n), m_count(c) {}

    ThreadWaitCheckpoint CaptureCheckpoint(const IdentifyWaitObject& identify, const KThread*) const override {
        if (!identify || m_count < 0 || m_count > Svc::ArgumentHandleCountMax) {
            throw std::invalid_argument("Pulse: invalid synchronization waiter");
        }
        ThreadWaitCheckpoint state{CheckpointWaitKind::Synchronization, 0, {}};
        state.objects.reserve(m_count);
        for (s32 i = 0; i < m_count; ++i) {
            const auto id = identify(m_objects[i]);
            if (!id) throw std::runtime_error("Pulse: synchronization object has no serializer");
            state.objects.push_back({id, m_objects[i]->GetCheckpointWaitOrdinal(&m_nodes[i])});
        }
        return state;
    }

    void NotifyAvailable(KernelCore& kernel, KThread* waiting_thread, KSynchronizationObject* signaled_object, Result wait_result) override {
        if (VideoCore::FrameTrace::TracksGuest(waiting_thread->GetThreadId())) {
            VideoCore::FrameTrace::Mark(VideoCore::FrameTrace::Event::SyncWakeObject,
                reinterpret_cast<uintptr_t>(signaled_object), waiting_thread->GetThreadId());
        }
        // Determine the sync index, and unlink all nodes.
        s32 sync_index = -1;
        for (auto i = 0; i < m_count; ++i) {
            // Check if this is the signaled object.
            if (m_objects[i] == signaled_object && sync_index == -1) {
                sync_index = i;
            }

            // Unlink the current node from the current object.
            m_objects[i]->UnlinkNode(std::addressof(m_nodes[i]));
        }

        // Set the waiting thread's sync index.
        waiting_thread->SetSyncedIndex(sync_index);

        // Set the waiting thread as not cancellable.
        waiting_thread->ClearCancellable();

        // Invoke the base end wait handler.
        KThreadQueue::EndWait(kernel, waiting_thread, wait_result);
    }

    void CancelWait(KernelCore& kernel, KThread* waiting_thread, Result wait_result, bool cancel_timer_task) override {
        // Remove all nodes from our list.
        for (auto i = 0; i < m_count; ++i) {
            m_objects[i]->UnlinkNode(std::addressof(m_nodes[i]));
        }

        // Set the waiting thread as not cancellable.
        waiting_thread->ClearCancellable();

        // Invoke the base cancel wait handler.
        KThreadQueue::CancelWait(kernel, waiting_thread, wait_result, cancel_timer_task);
    }

protected:
    KSynchronizationObject** m_objects;
    KSynchronizationObject::ThreadListNode* m_nodes;
    s32 m_count;
};

class RestoredSynchronizationWait final : public ThreadQueueImplForKSynchronizationObjectWait {
public:
    RestoredSynchronizationWait(KernelCore& kernel, const ThreadWaitCheckpoint& state,
                                const ResolveWaitObject& resolve)
        : ThreadQueueImplForKSynchronizationObjectWait(kernel, nullptr, nullptr, 0),
          m_kernel(kernel) {
        ValidateWaitCheckpoint(state);
        if (state.kind != CheckpointWaitKind::Synchronization || !resolve) {
            throw std::invalid_argument("Pulse: invalid synchronization continuation");
        }
        m_objects = m_owned_objects.data();
        m_nodes = m_owned_nodes.data();
        m_count = static_cast<s32>(state.objects.size());
        // Resolve the full graph before acquiring references or linking nodes.
        for (s32 i = 0; i < m_count; ++i) {
            m_objects[i] = resolve(state.objects[i].id);
            if (!m_objects[i]) throw std::invalid_argument("Pulse: unresolved wait object");
            for (s32 j = 0; j < i; ++j) {
                if ((state.objects[i].id == state.objects[j].id) != (m_objects[i] == m_objects[j])) {
                    throw std::invalid_argument("Pulse: inconsistent wait object identity");
                }
            }
            m_ordinals[i] = state.objects[i].ordinal;
        }
        for (s32 i = 0; i < m_count; ++i) {
            if (!m_objects[i]->Open(kernel)) {
                ReleaseCheckpointReferences(kernel);
                throw std::invalid_argument("Pulse: dead synchronization object");
            }
            ++m_opened;
        }
    }

    ~RestoredSynchronizationWait() override {
        ASSERT(!m_linked);
        ReleaseCheckpointReferences(m_kernel);
    }

    void LinkCheckpointWait(KThread* thread) override {
        ASSERT(!m_linked);
        // The graph coordinator restores waits in their original order. Reject
        // an incorrect order instead of silently changing wakeup FIFO semantics.
        for (s32 i = 0; i < m_count; ++i) {
            auto ordinal = m_objects[i]->GetCheckpointWaitOrdinal(nullptr);
            for (s32 j = 0; j < i; ++j) ordinal += m_objects[j] == m_objects[i];
            if (ordinal != m_ordinals[i]) {
                throw std::invalid_argument("Pulse: synchronization wait restore order");
            }
        }
        for (s32 i = 0; i < m_count; ++i) {
            m_nodes[i] = {nullptr, thread};
            m_objects[i]->LinkNode(&m_nodes[i]);
        }
        m_linked = true;
    }

    void NotifyAvailable(KernelCore& kernel, KThread* thread,
                         KSynchronizationObject* object, Result result) override {
        ThreadQueueImplForKSynchronizationObjectWait::NotifyAvailable(kernel, thread, object, result);
        m_linked = false;
    }

    void CancelWait(KernelCore& kernel, KThread* thread, Result result, bool cancel_timer) override {
        ThreadQueueImplForKSynchronizationObjectWait::CancelWait(kernel, thread, result, cancel_timer);
        m_linked = false;
    }

    void ReleaseCheckpointReferences(KernelCore& kernel) override {
        ASSERT(!m_linked);
        while (m_opened > 0) m_objects[--m_opened]->Close(kernel);
    }

    void AbandonCheckpointWait(KernelCore& kernel, KThread* thread) override {
        if (m_linked) {
            for (s32 i = 0; i < m_count; ++i) m_objects[i]->UnlinkNode(&m_nodes[i]);
            m_linked = false;
        }
        KThreadQueue::AbandonCheckpointWait(kernel, thread);
    }

private:
    KernelCore& m_kernel;
    std::array<KSynchronizationObject*, Svc::ArgumentHandleCountMax> m_owned_objects{};
    std::array<KSynchronizationObject::ThreadListNode, Svc::ArgumentHandleCountMax> m_owned_nodes{};
    std::array<u64, Svc::ArgumentHandleCountMax> m_ordinals{};
    s32 m_opened{};
    bool m_linked{};
};

} // namespace

std::unique_ptr<KThreadQueue> CreateCheckpointSynchronizationWait(
    KernelCore& kernel, const ThreadWaitCheckpoint& state, const ResolveWaitObject& resolve) {
    return std::make_unique<RestoredSynchronizationWait>(kernel, state, resolve);
}

void KSynchronizationObject::Finalize(KernelCore& kernel) {
    this->OnFinalizeSynchronizationObject();
    KAutoObject::Finalize(kernel);
}

Result KSynchronizationObject::Wait(KernelCore& kernel, s32* out_index,
                                    KSynchronizationObject** objects, const s32 num_objects,
                                    s64 timeout) {
    // Allocate space on stack for thread nodes.
    std::array<ThreadListNode, Svc::ArgumentHandleCountMax> thread_nodes;

    // Prepare for wait.
    KThread* thread = GetCurrentThreadPointer(kernel);
    u32 trace_capture{};
    KHardwareTimer* timer{};
    ThreadQueueImplForKSynchronizationObjectWait wait_queue(kernel, objects, thread_nodes.data(),
                                                            num_objects);

    {
        // Setup the scheduling lock and sleep.
        KScopedSchedulerLockAndSleep slp(kernel, std::addressof(timer), thread, timeout);

        // Check if the thread should terminate.
        if (thread->IsTerminationRequested()) {
            slp.CancelSleep();
            R_THROW(ResultTerminationRequested);
        }

        // Check if any of the objects are already signaled.
        for (auto i = 0; i < num_objects; ++i) {
            ASSERT(objects[i] != nullptr);

            if (objects[i]->IsSignaled(kernel)) {
                *out_index = i;
                slp.CancelSleep();
                R_THROW(ResultSuccess);
            }
        }

        // Check if the timeout is zero.
        if (timeout == 0) {
            slp.CancelSleep();
            R_THROW(ResultTimedOut);
        }

        // Check if waiting was canceled.
        if (thread->IsWaitCancelled()) {
            slp.CancelSleep();
            thread->ClearWaitCancelled();
            R_THROW(ResultCancelled);
        }

        if (VideoCore::FrameTrace::TracksGuest(thread->GetThreadId()) &&
            VideoCore::FrameTrace::Active()) {
            trace_capture = VideoCore::FrameTrace::GetCaptureStatus().id;
            VideoCore::FrameTrace::Mark(VideoCore::FrameTrace::Event::SyncWaitBegin,
                                       num_objects, thread->GetThreadId());
            for (s32 i = 0; i < num_objects; ++i) {
                VideoCore::FrameTrace::Mark(VideoCore::FrameTrace::Event::SyncWaitObject,
                    reinterpret_cast<uintptr_t>(objects[i]), (thread->GetThreadId() << 32) | i);
            }
        }
        // Add the waiters.
        for (auto i = 0; i < num_objects; ++i) {
            thread_nodes[i].thread = thread;
            thread_nodes[i].next = nullptr;

            objects[i]->LinkNode(std::addressof(thread_nodes[i]));
        }

        // Mark the thread as cancellable.
        thread->SetCancellable();

        // Clear the thread's synced index.
        thread->SetSyncedIndex(-1);

        // Wait for an object to be signaled.
        wait_queue.SetHardwareTimer(timer);
        thread->BeginWait(kernel, std::addressof(wait_queue));
        thread->SetWaitReasonForDebugging(ThreadWaitReasonForDebugging::Synchronization);
    }

    // Set the output index.
    *out_index = thread->GetSyncedIndex();

    // Get the wait result.
    const auto result = thread->GetWaitResult();
    if (trace_capture != 0 && VideoCore::FrameTrace::Active() &&
        VideoCore::FrameTrace::GetCaptureStatus().id == trace_capture) {
        VideoCore::FrameTrace::Mark(VideoCore::FrameTrace::Event::SyncWaitEnd,
                                   result.raw, thread->GetThreadId());
    }
    R_RETURN(result);
}

KSynchronizationObject::KSynchronizationObject(KernelCore& kernel) : KAutoObjectWithList{kernel} {}

KSynchronizationObject::~KSynchronizationObject() = default;

void KSynchronizationObject::NotifyAvailable(KernelCore& kernel, Result result) {
    KScopedSchedulerLock sl(kernel);

    // If we're not signaled, we've nothing to notify.
    if (!this->IsSignaled(kernel)) {
        return;
    }

    // Iterate over each thread.
    for (auto* cur_node = m_thread_list_head; cur_node != nullptr; cur_node = cur_node->next) {
        cur_node->thread->NotifyAvailable(kernel, this, result);
    }
}

std::vector<KThread*> KSynchronizationObject::GetWaitingThreadsForDebugging(KernelCore& kernel) const {
    std::vector<KThread*> threads;

    // If debugging, dump the list of waiters.
    {
        KScopedSchedulerLock lock(kernel);
        for (auto* cur_node = m_thread_list_head; cur_node != nullptr; cur_node = cur_node->next) {
            threads.emplace_back(cur_node->thread);
        }
    }

    return threads;
}
} // namespace Kernel
