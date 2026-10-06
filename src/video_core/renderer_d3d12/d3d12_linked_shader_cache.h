// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cstdint>
#include <exception>
#include <future>
#include <memory>
#include <mutex>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace D3D12 {

struct LinkedShaderStage {
    std::span<const std::uint32_t> words;
    std::uint32_t stage;
};

/// Full linked input, including options, stage order and lengths. Guest shader hashes alone
/// are insufficient: runtime specialization and inter-stage linking can change the bytecode.
inline std::vector<std::uint32_t> MakeLinkedShaderKey(
    std::span<const LinkedShaderStage> stages, std::span<const std::uint32_t> options) {
    size_t count = 2 + options.size() + 3 * stages.size();
    for (const auto& stage : stages) {
        count += stage.words.size();
    }
    std::vector<std::uint32_t> key;
    key.reserve(count);
    key.push_back(static_cast<std::uint32_t>(options.size()));
    key.insert(key.end(), options.begin(), options.end());
    key.push_back(static_cast<std::uint32_t>(stages.size()));
    for (const auto& stage : stages) {
        key.push_back(stage.stage);
        const auto size = static_cast<std::uint64_t>(stage.words.size());
        key.push_back(static_cast<std::uint32_t>(size));
        key.push_back(static_cast<std::uint32_t>(size >> 32));
        key.insert(key.end(), stage.words.begin(), stage.words.end());
    }
    return key;
}

struct LinkedShaderKeyHash {
    std::uint64_t operator()(std::span<const std::uint32_t> key) const noexcept {
        std::uint64_t hash = 14695981039346656037ULL;
        for (const auto word : key) {
            hash = (hash ^ word) * 1099511628211ULL;
        }
        return hash;
    }
};

/// Per-game, bounded memoization of signed linked shaders, independent of fixed PSO state.
/// Only bookkeeping holds the mutex. Identical concurrent requests share one compilation;
/// an owner always builds inline, so a waiter cannot wait on a task queued behind itself.
/// Failed builds are removed and can be retried; existing waiters receive the same exception.
template <class Value, class Hash = LinkedShaderKeyHash>
class LinkedShaderCache {
public:
    enum class Outcome : std::uint32_t { Compiled, Hit, SharedInFlight, Bypassed };
    struct Result {
        std::shared_ptr<const Value> value;
        Outcome outcome;
    };

    static constexpr size_t DEFAULT_BUDGET = 4 * 1024 * 1024;

    explicit LinkedShaderCache(size_t budget = DEFAULT_BUDGET, size_t entry_limit = 128)
        : budget_bytes{budget}, max_entries{entry_limit} {}

    template <class Build, class Measure>
    Result Get(std::vector<std::uint32_t> key, Build&& build, Measure&& measure) {
        const auto hash = Hash{}(key);
        std::shared_ptr<Entry> entry;
        bool owner = false;
        Outcome outcome = Outcome::Bypassed;
        {
            std::scoped_lock lock{mutex};
            const auto [first, last] = entries.equal_range(hash);
            for (auto it = first; it != last; ++it) {
                if (it->second->key == key) {
                    entry = it->second;
                    entry->used = ++sequence;
                    outcome = entry->ready ? Outcome::Hit : Outcome::SharedInFlight;
                    break;
                }
            }
            // Include bookkeeping and allocator slack; the key is stored only once.
            const size_t bytes = sizeof(Entry) + 128 + key.capacity() * sizeof(key[0]);
            if (!entry && max_entries != 0 && bytes <= budget_bytes &&
                MakeRoom(bytes, 1)) {
                entry = std::make_shared<Entry>(std::move(key), bytes, ++sequence);
                entries.emplace(hash, entry);
                retained_bytes += bytes;
                owner = true;
            }
        }
        if (!entry) {
            return {std::make_shared<const Value>(build()), Outcome::Bypassed};
        }
        if (!owner) {
            return {entry->future.get(), outcome};
        }
        try {
            auto value = std::make_shared<const Value>(build());
            const size_t result_bytes = measure(*value);
            entry->promise.set_value(value);
            {
                std::scoped_lock lock{mutex};
                // An in-flight entry is never evicted, including this one. Oversized results
                // are returned to consumers but not retained beyond the compilation.
                if (result_bytes <= budget_bytes && MakeRoom(result_bytes, 0)) {
                    entry->bytes += result_bytes;
                    retained_bytes += result_bytes;
                    entry->ready = true;
                } else {
                    Remove(hash, entry);
                }
            }
            return {std::move(value), Outcome::Compiled};
        } catch (...) {
            entry->promise.set_exception(std::current_exception());
            std::scoped_lock lock{mutex};
            Remove(hash, entry);
            throw;
        }
    }

    size_t RetainedBytes() const {
        std::scoped_lock lock{mutex};
        return retained_bytes;
    }

    /// Compiling entries remain alive. Live PSOs retain their own immutable DXIL references.
    void SetBudget(size_t budget) {
        std::scoped_lock lock{mutex};
        budget_bytes = budget;
        MakeRoom(0, 0);
    }

private:
    struct Entry {
        Entry(std::vector<std::uint32_t> key_, size_t bytes_, std::uint64_t used_)
            : key{std::move(key_)}, future{promise.get_future().share()}, bytes{bytes_}, used{used_} {}
        std::vector<std::uint32_t> key;
        std::promise<std::shared_ptr<const Value>> promise;
        std::shared_future<std::shared_ptr<const Value>> future;
        size_t bytes;
        std::uint64_t used;
        bool ready{};
    };

    bool MakeRoom(size_t bytes, size_t count) {
        while (retained_bytes > budget_bytes || bytes > budget_bytes - retained_bytes ||
               entries.size() + count > max_entries) {
            auto oldest = entries.end();
            for (auto it = entries.begin(); it != entries.end(); ++it) {
                if (it->second->ready &&
                    (oldest == entries.end() || it->second->used < oldest->second->used)) {
                    oldest = it;
                }
            }
            if (oldest == entries.end()) {
                return false;
            }
            retained_bytes -= oldest->second->bytes;
            entries.erase(oldest);
        }
        return true;
    }

    void Remove(std::uint64_t hash, const std::shared_ptr<Entry>& entry) {
        const auto [first, last] = entries.equal_range(hash);
        for (auto it = first; it != last; ++it) {
            if (it->second == entry) {
                retained_bytes -= entry->bytes;
                entries.erase(it);
                return;
            }
        }
    }

    mutable std::mutex mutex;
    std::unordered_multimap<std::uint64_t, std::shared_ptr<Entry>> entries;
    size_t retained_bytes{};
    std::uint64_t sequence{};
    size_t budget_bytes;
    const size_t max_entries;
};

} // namespace D3D12
