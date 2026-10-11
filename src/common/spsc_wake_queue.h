// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <memory>
#include <utility>

#include "common/polyfill_thread.h"
#include "common/wake_flag.h"

namespace Common {

/// A bounded single-producer, single-consumer queue whose consumer waits with a WakeFlag: it
/// spins briefly before blocking, and a push only enters the OS when the consumer is blocked.
/// SPSCQueue takes two mutexes and notifies a condition variable on every push and pop, which
/// costs a system call per item while its consumer sleeps.
template <typename T, size_t Capacity = 0x1000>
class SPSCWakeQueue {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two.");

public:
    explicit SPSCWakeQueue(SpinPolicy policy = {}) : ready{policy} {}

    /// Waits while the queue is full, which only a consumer far behind makes happen.
    template <typename... Args>
    void EmplaceWait(Args&&... args) {
        const size_t write = write_index.load(std::memory_order_relaxed);
        size_t read = read_index.load(std::memory_order_acquire);
        while (write - read == Capacity) {
            producer_waiting.store(true, std::memory_order_seq_cst);
            read = read_index.load(std::memory_order_seq_cst);
            if (write - read == Capacity) {
                read_index.wait(read, std::memory_order_acquire);
                read = read_index.load(std::memory_order_acquire);
            }
            producer_waiting.store(false, std::memory_order_relaxed);
        }
        slots[write % Capacity] = T{std::forward<Args>(args)...};
        write_index.store(write + 1, std::memory_order_release);
        ready.Raise();
    }

    bool TryPop(T& item) {
        const size_t read = read_index.load(std::memory_order_relaxed);
        if (read == write_index.load(std::memory_order_acquire)) {
            return false;
        }
        item = std::move(slots[read % Capacity]);
        read_index.store(read + 1, std::memory_order_seq_cst);
        if (producer_waiting.load(std::memory_order_seq_cst)) {
            read_index.notify_one();
        }
        return true;
    }

    /// False only when stop was requested with the queue empty.
    bool PopWait(T& item, std::stop_token stop_token) {
        std::stop_callback wake_on_stop{stop_token, [this] { ready.Raise(); }};
        for (;;) {
            if (TryPop(item)) {
                return true;
            }
            if (stop_token.stop_requested()) {
                return false;
            }
            // Clear before the second look: a push after it raises the flag again.
            ready.Clear();
            if (TryPop(item)) {
                return true;
            }
            if (stop_token.stop_requested()) {
                return false;
            }
            ready.Wait();
        }
    }

private:
    std::array<T, Capacity> slots{};
    alignas(64) std::atomic<size_t> write_index{0};
    alignas(64) std::atomic<size_t> read_index{0};
    std::atomic<bool> producer_waiting{false};
    WakeFlag ready;
};

} // namespace Common
