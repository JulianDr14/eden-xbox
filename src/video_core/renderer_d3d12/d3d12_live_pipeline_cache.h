// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <condition_variable>
#include <cstddef>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>

namespace D3D12 {

/// An index of objects owned by pipelines, not another residency cache. A compiler owns its
/// entry before starting work inline; equivalent requests wait on that entry, never on a task
/// queued behind themselves. The index retains neither results nor keys after consumers retire.
template <class Key, class Value, class Hash>
class LivePipelineCache {
public:
    enum class Outcome { Created, Hit, SharedInFlight, Bypassed };
    struct Entry {
        explicit Entry(Key key_) : key{std::move(key_)} {}
        const Key key;
        std::optional<Value> value;

    private:
        friend class LivePipelineCache;
        std::mutex mutex;
        std::condition_variable ready_condition;
        bool ready{};
        std::exception_ptr failure;
    };
    struct Result {
        std::shared_ptr<const Entry> entry;
        Outcome outcome;
    };

    explicit LivePipelineCache(size_t limit = 8192) : max_entries{limit} {}

    template <class Build>
    Result Get(Key key, Build&& build) {
        const auto hash = Hash{}(key);
        std::shared_ptr<Entry> entry;
        bool owner = false;
        bool indexed = false;
        {
            std::scoped_lock lock{mutex};
            // Bound dead weak control blocks too. Full sweep is amortized over requests and
            // only occurs on compilation, never on the per-draw ready fast path.
            if (++requests % 128 == 0 || entries.size() >= max_entries) {
                std::erase_if(entries, [](const auto& item) { return item.second.expired(); });
            }
            const auto [first, last] = entries.equal_range(hash);
            for (auto it = first; it != last; ++it) {
                if (auto existing = it->second.lock(); existing && existing->key == key) {
                    entry = std::move(existing);
                    break;
                }
            }
            if (!entry) {
                entry = std::make_shared<Entry>(std::move(key));
                owner = true;
                indexed = entries.size() < max_entries;
                if (indexed) {
                    entries.emplace(hash, entry);
                }
            }
        }
        if (!owner) {
            std::unique_lock lock{entry->mutex};
            const auto outcome = entry->ready ? Outcome::Hit : Outcome::SharedInFlight;
            entry->ready_condition.wait(lock, [&] { return entry->ready; });
            if (entry->failure) {
                std::rethrow_exception(entry->failure);
            }
            return {std::move(entry), outcome};
        }
        try {
            entry->value.emplace(build());
        } catch (...) {
            const auto failure = std::current_exception();
            // A failed build must be retriable even while existing waiters hold its entry.
            {
                std::scoped_lock lock{mutex};
                const auto [first, last] = entries.equal_range(hash);
                for (auto it = first; it != last; ++it) {
                    if (it->second.lock() == entry) {
                        entries.erase(it);
                        break;
                    }
                }
            }
            {
                std::scoped_lock lock{entry->mutex};
                entry->failure = failure;
                entry->ready = true;
            }
            entry->ready_condition.notify_all();
            std::rethrow_exception(failure);
        }
        {
            std::scoped_lock lock{entry->mutex};
            entry->ready = true;
        }
        entry->ready_condition.notify_all();
        return {std::move(entry), indexed ? Outcome::Created : Outcome::Bypassed};
    }

private:
    std::mutex mutex;
    std::unordered_multimap<size_t, std::weak_ptr<Entry>> entries;
    size_t requests{};
    const size_t max_entries;
};

} // namespace D3D12
