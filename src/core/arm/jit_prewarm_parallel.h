// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <exception>
#include <functional>
#include <thread>

namespace Core::JitPrewarm {

using Progress = std::function<void(std::size_t, std::size_t)>;

// One stopped JIT per owner, never concurrent access to one instance. UI callbacks
// run only on the coordinator. RAII joins also protect callback/state lifetimes
// when progress throws. Zero-sized owners do not need a worker.
inline void RunOwners(const std::array<std::size_t, 4>& sizes,
                      const std::function<void(std::size_t, const Progress&)>& work,
                      const Progress& progress) {
    std::array<std::atomic<std::size_t>, 4> completed{};
    std::array<std::atomic_bool, 4> finished{};
    std::array<std::exception_ptr, 4> errors{};
    std::array<std::jthread, 4> workers;
    std::size_t total{};
    for (std::size_t core = 0; core < sizes.size(); ++core) {
        total += sizes[core];
        if (sizes[core] == 0) {
            finished[core].store(true);
            continue;
        }
        const auto task = [&, core] {
            try {
                work(core, [&, core](auto done, auto) {
                    completed[core].store(done, std::memory_order_relaxed);
                });
            } catch (...) {
                errors[core] = std::current_exception();
            }
            completed[core].store(sizes[core], std::memory_order_relaxed);
            finished[core].store(true, std::memory_order_release);
        };
        try {
            workers[core] = std::jthread{task};
        } catch (const std::system_error&) {
            task(); // Thread creation failure: keep this owner's sequential path.
        }
    }
    for (;;) {
        bool all_finished = true;
        std::size_t done{};
        for (std::size_t core = 0; core < sizes.size(); ++core) {
            all_finished &= finished[core].load(std::memory_order_acquire);
            done += completed[core].load(std::memory_order_relaxed);
        }
        if (progress && total != 0) progress(done, total);
        if (all_finished) break;
        std::this_thread::sleep_for(std::chrono::milliseconds{33});
    }
    for (auto& worker : workers) {
        if (worker.joinable()) worker.join();
    }
    for (const auto& error : errors) {
        if (error) std::rethrow_exception(error);
    }
}

} // namespace Core::JitPrewarm
