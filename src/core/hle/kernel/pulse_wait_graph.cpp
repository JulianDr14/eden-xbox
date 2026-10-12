// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#include <unordered_map>
#include "core/hle/kernel/pulse_wait_graph.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_scheduler.h"
#include "core/hle/kernel/k_thread.h"

namespace Kernel {
std::vector<CheckpointWaitingThread> CaptureCheckpointWaitGraph(
    KernelCore& kernel, std::span<KThread* const> threads, const IdentifyWaitObject& identify) {
    if (!KScheduler::IsSchedulerLockedByCurrentThread(kernel) || !identify || threads.size() > 4096) {
        throw std::invalid_argument("Pulse: wait graph capture requires stopped scheduler and inventory");
    }
    std::vector<CheckpointWaitingThread> state;
    state.reserve(threads.size());
    for (auto* thread : threads) {
        if (!thread || !thread->GetOwnerProcess()) {
            throw std::invalid_argument("Pulse: invalid waiting guest thread");
        }
        state.push_back({identify(thread), identify(thread->GetOwnerProcess()), thread->GetPriority(),
                         thread->CaptureWaitCheckpoint(kernel, identify)});
    }
    // Detect missing predecessors and incompatible ordering before publishing.
    (void)PlanCheckpointWaitRestore(state);
    return state;
}

void RestoreCheckpointWaitGraph(KernelCore& kernel,
    std::span<const CheckpointWaitingThread> threads, const ResolveWaitObject& resolve) {
    if (!KScheduler::IsSchedulerLockedByCurrentThread(kernel) || !resolve) {
        throw std::invalid_argument("Pulse: wait graph restore requires stopped scheduler");
    }
    const auto order = PlanCheckpointWaitRestore(threads);
    std::unordered_map<u64, KSynchronizationObject*> resolved;
    std::unordered_map<const KSynchronizationObject*, u64> identities;
    const ResolveWaitObject reference = [&](u64 id) -> KSynchronizationObject* {
        if (!id) throw std::invalid_argument("Pulse: null wait graph identity");
        if (auto found = resolved.find(id); found != resolved.end()) return found->second;
        if (resolved.size() >= 65536) throw std::invalid_argument("Pulse: wait object graph limit");
        auto* object = resolve(id);
        if (!object || !identities.emplace(object, id).second) {
            throw std::invalid_argument("Pulse: unresolved or aliased wait graph identity");
        }
        resolved.emplace(id, object);
        return object;
    };
    std::vector<KThread*> bindings;
    bindings.reserve(threads.size());
    std::unordered_set<KThread*> owners;
    for (const auto& state : threads) {
        auto* object = reference(state.id);
        auto* thread = object ? object->DynamicCast<KThread*>() : nullptr;
        auto* process_object = reference(state.process_id);
        auto* process = process_object ? process_object->DynamicCast<KProcess*>() : nullptr;
        if (!thread || !process || thread->GetOwnerProcess() != process ||
            !thread->IsFreshCheckpointThread() || state.effective_priority > thread->GetBasePriority() ||
            !owners.insert(thread).second) {
            throw std::invalid_argument("Pulse: invalid fresh wait graph binding");
        }
        bindings.push_back(thread);
    }
    // Resolve the complete reference set before changing any thread. One stable
    // binding per ID is shared by every queue, including repeated handles.
    for (const auto& state : threads) {
        for (auto object : state.wait.objects) (void)reference(object.id);
        if (state.wait.reference_owner) (void)reference(state.wait.reference_owner);
    }
    // Restore effective priorities together before linking inheritance chains.
    // Initialized threads are not in the runnable scheduler trees yet.
    for (size_t i = 0; i < threads.size(); ++i) bindings[i]->SetPriority(threads[i].effective_priority);
    for (auto index : order) bindings[index]->RestoreWaitCheckpoint(kernel, threads[index].wait, reference);
}
} // namespace Kernel
