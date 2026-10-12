// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <queue>
#include <unordered_set>
#include "core/hle/kernel/pulse_wait_state.h"

namespace Kernel {

struct CheckpointWaitingThread {
    u64 id{};
    u64 process_id{};
    s32 effective_priority{};
    ThreadWaitCheckpoint wait;
};

// Both APIs require a stopped kernel and the scheduler lock. The inventory must
// include every waiting guest thread; host/service waits are rejected separately
// by the system barrier. A restore failure requires destroying the fresh system.
std::vector<CheckpointWaitingThread> CaptureCheckpointWaitGraph(
    KernelCore& kernel, std::span<KThread* const> threads, const IdentifyWaitObject& identify);
void RestoreCheckpointWaitGraph(KernelCore& kernel,
    std::span<const CheckpointWaitingThread> threads, const ResolveWaitObject& resolve);

// Construct the ordering constraints before touching a fresh kernel. The caller
// must include every waiting thread and restore all effective priorities first.
// This operation owns only metadata, has hard limits, and allocates nothing in
// normal gameplay. The stopped kernel revalidates each ordinal when linking.
inline std::vector<size_t> PlanCheckpointWaitRestore(std::span<const CheckpointWaitingThread> threads) {
    constexpr size_t MaxThreads = 4096;
    constexpr size_t MaxEntries = 65536;
    if (threads.size() > MaxThreads) throw std::invalid_argument("Pulse: waiting thread graph limit");
    struct Entry {
        std::array<u64, 4> group;
        u64 ordinal;
        size_t node;
    };
    size_t entry_count{};
    std::unordered_set<u64> identities;
    for (const auto& thread : threads) {
        ValidateWaitCheckpoint(thread.wait);
        if (!thread.id || !thread.process_id || thread.effective_priority < 0 ||
            thread.effective_priority > 63 || !identities.insert(thread.id).second) {
            throw std::invalid_argument("Pulse: invalid waiting thread identity or priority");
        }
        const auto& wait = thread.wait;
        const size_t entries = wait.kind == CheckpointWaitKind::Synchronization ? wait.objects.size() :
                               wait.kind == CheckpointWaitKind::Sleep ? 0 : 1;
        entry_count += entries + (wait.deadline > 0);
        if (entry_count > MaxEntries) throw std::invalid_argument("Pulse: wait graph reference limit");
    }
    std::vector<Entry> entries;
    entries.reserve(entry_count);
    for (size_t i = 0; i < threads.size(); ++i) {
        const auto& thread = threads[i];
        const auto& wait = thread.wait;
        const auto priority = static_cast<u64>(thread.effective_priority);
        switch (wait.kind) {
        case CheckpointWaitKind::Synchronization:
            for (auto reference : wait.objects) entries.push_back({{0, reference.id, 0, 0}, reference.ordinal, i});
            break;
        case CheckpointWaitKind::AddressArbiter:
            entries.push_back({{1, thread.process_id, wait.address, priority}, wait.ordinal, i});
            break;
        case CheckpointWaitKind::Mutex:
        case CheckpointWaitKind::ConditionVariable:
            if (!wait.objects.empty()) {
                entries.push_back({{2, wait.objects[0].id, wait.address, priority}, wait.objects[0].ordinal, i});
            } else {
                entries.push_back({{3, thread.process_id, wait.condition_key, priority}, wait.ordinal, i});
            }
            break;
        case CheckpointWaitKind::Sleep:
            break;
        default:
            throw std::invalid_argument("Pulse: unsupported wait graph node");
        }
        if (wait.deadline > 0) entries.push_back({{4, static_cast<u64>(wait.deadline), 0, 0}, wait.timer_ordinal, i});
    }
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        return a.group < b.group || (a.group == b.group && a.ordinal < b.ordinal);
    });
    std::vector<std::pair<size_t, size_t>> edges;
    edges.reserve(entries.size());
    for (size_t i = 0; i < entries.size(); ++i) {
        const auto& entry = entries[i];
        if (!i || entries[i - 1].group != entry.group) {
            if (entry.ordinal) throw std::invalid_argument("Pulse: incomplete wait graph bucket");
        } else {
            const auto& previous = entries[i - 1];
            if (entry.ordinal != previous.ordinal + 1) {
                throw std::invalid_argument("Pulse: duplicated or missing wait ordinal");
            }
            if (previous.node != entry.node) edges.emplace_back(previous.node, entry.node);
        }
    }
    std::sort(edges.begin(), edges.end());
    edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
    std::vector<size_t> incoming(threads.size());
    std::vector<size_t> offsets(threads.size() + 1);
    for (auto [source, target] : edges) { ++incoming[target]; ++offsets[source + 1]; }
    for (size_t i = 1; i < offsets.size(); ++i) offsets[i] += offsets[i - 1];
    using Ready = std::pair<u64, size_t>;
    std::priority_queue<Ready, std::vector<Ready>, std::greater<Ready>> ready;
    for (size_t i = 0; i < threads.size(); ++i) if (!incoming[i]) ready.emplace(threads[i].id, i);
    std::vector<size_t> order;
    order.reserve(threads.size());
    while (!ready.empty()) {
        const auto node = ready.top().second;
        ready.pop();
        order.push_back(node);
        for (size_t i = offsets[node]; i < offsets[node + 1]; ++i) {
            const auto target = edges[i].second;
            if (!--incoming[target]) ready.emplace(threads[target].id, target);
        }
    }
    if (order.size() != threads.size()) throw std::invalid_argument("Pulse: cyclic wait ordering constraints");
    return order;
}
} // namespace Kernel
