// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <atomic>
#include <cassert>
#include <iostream>
#include <stdexcept>
#include <thread>

#include "video_core/renderer_d3d12/d3d12_linked_shader_cache.h"

using Value = std::vector<std::uint8_t>;
using Cache = D3D12::LinkedShaderCache<Value>;
static size_t Measure(const Value& value) { return sizeof(value) + value.capacity(); }

struct CollisionHash {
    std::uint64_t operator()(std::span<const std::uint32_t>) const { return 1; }
};

int main() {
    const std::array<std::uint32_t, 4> words{1, 2, 3, 4};
    const std::array<std::uint32_t, 2> options{7, 8};
    const std::array<D3D12::LinkedShaderStage, 2> stages{{
        {std::span(words).first(2), 0}, {std::span(words).last(2), 4}}};
    const auto key = D3D12::MakeLinkedShaderKey(stages, options);
    auto changed_options = options;
    changed_options[1] ^= 1;
    assert(key != D3D12::MakeLinkedShaderKey(stages, changed_options));
    auto changed_stages = stages;
    changed_stages[0].stage = 1;
    assert(key != D3D12::MakeLinkedShaderKey(changed_stages, options));
    std::swap(changed_stages[0], changed_stages[1]);
    assert(key != D3D12::MakeLinkedShaderKey(changed_stages, options));
    // Identical concatenated words, different linked-stage partition.
    changed_stages = {{{std::span(words).first(1), 0}, {std::span(words).subspan(1), 4}}};
    assert(key != D3D12::MakeLinkedShaderKey(changed_stages, options));
    auto changed_words = words;
    changed_words[3] ^= 1;
    changed_stages = {{{std::span(changed_words).first(2), 0},
                       {std::span(changed_words).last(2), 4}}};
    assert(key != D3D12::MakeLinkedShaderKey(changed_stages, options));

    D3D12::LinkedShaderCache<Value, CollisionHash> collisions;
    assert(collisions.Get({1}, [] { return Value{11}; }, Measure).value->front() == 11);
    assert(collisions.Get({2}, [] { return Value{22}; }, Measure).value->front() == 22);
    assert(collisions.Get({1}, [] { throw std::runtime_error("unexpected build"); return Value{}; },
                          Measure).value->front() == 11);

    Cache cache;
    std::atomic<int> builds{}, entrants{};
    std::promise<void> owner_started, release;
    auto gate = release.get_future().share();
    std::array<Cache::Result, 16> results;
    std::array<std::thread, 16> threads;
    auto build = [&] {
        ++builds;
        owner_started.set_value();
        gate.wait();
        return Value{1, 2, 3, 4};
    };
    threads[0] = std::thread([&] { results[0] = cache.Get(key, build, Measure); });
    owner_started.get_future().wait();
    for (size_t i = 1; i < threads.size(); ++i) {
        threads[i] = std::thread([&, i] {
            ++entrants;
            results[i] = cache.Get(key, build, Measure);
        });
    }
    while (entrants != 15) {
        std::this_thread::yield();
    }
    release.set_value();
    for (auto& thread : threads) {
        thread.join();
    }
    assert(builds == 1);
    for (const auto& result : results) {
        assert(result.value == results[0].value);
        assert(*result.value == Value({1, 2, 3, 4}));
    }
    assert(results[0].outcome == Cache::Outcome::Compiled);
    assert(cache.Get(key, build, Measure).outcome == Cache::Outcome::Hit);

    Cache retries;
    for (int i = 0; i < 2; ++i) {
        bool threw = false;
        try {
            retries.Get(key, []() -> Value { throw std::runtime_error("translator failed"); },
                        Measure);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        assert(threw && retries.RetainedBytes() == 0);
    }
    assert(retries.Get(key, [] { return Value{42}; }, Measure).value->front() == 42);

    Cache bounded{1024, 2};
    for (std::uint32_t i = 0; i < 1000; ++i) {
        const auto value = bounded.Get({i}, [i] { return Value(400, i % 256); }, Measure);
        assert(value.value->front() == i % 256 && bounded.RetainedBytes() <= 1024);
    }
    // Live consumer results survive eviction.
    auto retained = bounded.Get({1001}, [] { return Value{17}; }, Measure).value;
    bounded.Get({1002}, [] { return Value(2000, 99); }, Measure);
    assert(retained->front() == 17 && bounded.RetainedBytes() <= 1024);
    Cache disabled{0};
    assert(disabled.Get(key, [] { return Value{5}; }, Measure).outcome == Cache::Outcome::Bypassed);
    assert(disabled.RetainedBytes() == 0);
    Cache lifetimes{4096, 1};
    const auto kept = lifetimes.Get({1}, [] { return Value{17}; }, Measure).value;
    lifetimes.Get({2}, [] { return Value{22}; }, Measure); // Evicts key 1.
    const auto rebuilt = lifetimes.Get({1}, [] { return Value{17}; }, Measure);
    assert(rebuilt.outcome == Cache::Outcome::Compiled && rebuilt.value != kept);
    assert(kept->front() == 17);
    Cache pending_limit{4096, 1};
    std::promise<void> pending_started, pending_release;
    auto pending_gate = pending_release.get_future();
    std::thread pending_owner([&] {
        pending_limit.Get({1}, [&] {
            pending_started.set_value();
            pending_gate.wait();
            return Value{1};
        }, Measure);
    });
    pending_started.get_future().wait();
    assert(pending_limit.Get({2}, [] { return Value{2}; }, Measure).outcome ==
           Cache::Outcome::Bypassed); // Cannot evict an in-flight compilation.
    pending_release.set_value();
    pending_owner.join();
    assert(pending_limit.Get({1}, [] { return Value{9}; }, Measure).outcome == Cache::Outcome::Hit);
    Cache lru{4096, 2};
    auto one = lru.Get({1}, [] { return Value{1}; }, Measure).value;
    lru.Get({2}, [] { return Value{2}; }, Measure);
    lru.Get({1}, [] { return Value{9}; }, Measure);
    lru.Get({3}, [] { return Value{3}; }, Measure);
    assert(lru.Get({1}, [] { return Value{9}; }, Measure).value == one);
    assert(lru.Get({2}, [] { return Value{2}; }, Measure).outcome == Cache::Outcome::Compiled);
    std::cout << "Linked shader cache: exact input/options/stages, collisions, 16 concurrent requests, "
                 "retry, bounded LRU and retained consumers passed\n";
}
