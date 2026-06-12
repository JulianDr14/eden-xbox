// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <condition_variable>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>

#include "core/core.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/svc.h"
#include "core/hle/kernel/svc/svc_debug_string.h"
#include "core/memory.h"

namespace Kernel::Svc {

constexpr auto MAX_MSG_TIME = std::chrono::milliseconds(250);
const auto MAX_MSG_SIZE = 0x1000;

namespace {
std::mutex g_debug_observer_mutex;
DebugStringObserver g_debug_observer;
} // namespace

void SetDebugStringObserver(DebugStringObserver observer) {
    std::lock_guard lock(g_debug_observer_mutex);
    g_debug_observer = std::move(observer);
}

/// Used to output a message on a debug hardware unit - does nothing on a retail unit
Result OutputDebugString(Core::System& system, u64 address, u64 len) {
    static struct DebugFlusher {
        std::string msg_buffer;
        std::mutex msg_mutex;
        std::condition_variable msg_cv;
        std::chrono::steady_clock::time_point last_msg_time;
        std::optional<std::jthread> thread;
    } flusher_data;
    R_SUCCEED_IF(len == 0);
    // Only start the thread the very first time this function is called
    if (!flusher_data.thread) {
        flusher_data.thread.emplace([](std::stop_token stop_token) {
            while (!stop_token.stop_requested()) {
                std::unique_lock lock(flusher_data.msg_mutex);
                flusher_data.msg_cv.wait(lock, [&stop_token] {
                    return !flusher_data.msg_buffer.empty() || stop_token.stop_requested();
                });
                if (stop_token.stop_requested() && flusher_data.msg_buffer.empty())
                    break;
                auto timeout = flusher_data.last_msg_time + MAX_MSG_TIME;
                bool woke_early = flusher_data.msg_cv.wait_until(lock, timeout, [&stop_token] {
                    return flusher_data.msg_buffer.size() >= MAX_MSG_SIZE || stop_token.stop_requested();
                });
                if (!woke_early || flusher_data.msg_buffer.size() >= MAX_MSG_SIZE || stop_token.stop_requested()) {
                    if (!flusher_data.msg_buffer.empty()) {
                        // Remove trailing newline as LOG_INFO adds that anyways
                        if (flusher_data.msg_buffer.back() == '\n')
                            flusher_data.msg_buffer.pop_back();

                        LOG_INFO(Debug_Emulated, "\n{}", flusher_data.msg_buffer);
                        flusher_data.msg_buffer.clear();
                    }
                    if (stop_token.stop_requested()) break;
                }
            }
            flusher_data.msg_cv.notify_all();
        });
    }
    // Only pay the per-call copy when a debug-string observer is actually installed (the headless
    // UWP boot frontend installs one to watch for the JIT-liveness sentinel; normal builds do not).
    bool has_observer;
    {
        std::lock_guard observer_lock(g_debug_observer_mutex);
        has_observer = static_cast<bool>(g_debug_observer);
    }
    std::string observed_chunk;
    {
        std::lock_guard lock(flusher_data.msg_mutex);
        const auto old_size = flusher_data.msg_buffer.size();
        flusher_data.msg_buffer.resize(old_size + len);
        GetCurrentMemory(system.Kernel()).ReadBlock(address, flusher_data.msg_buffer.data() + old_size, len);
        flusher_data.last_msg_time = std::chrono::steady_clock::now();
        if (has_observer) {
            // Capture the chunk while we hold the buffer; the flusher thread may clear it after we
            // release this lock.
            observed_chunk.assign(flusher_data.msg_buffer.data() + old_size, len);
        }
    }
    flusher_data.msg_cv.notify_one();
    if (has_observer) {
        // Notify outside the message lock to avoid holding two locks across the callback.
        std::lock_guard observer_lock(g_debug_observer_mutex);
        if (g_debug_observer) {
            g_debug_observer(observed_chunk);
        }
    }
    R_SUCCEED();
}

Result OutputDebugString64(Core::System& system, uint64_t debug_str, uint64_t len) {
    R_RETURN(OutputDebugString(system, debug_str, len));
}

Result OutputDebugString64From32(Core::System& system, uint32_t debug_str, uint32_t len) {
    R_RETURN(OutputDebugString(system, debug_str, len));
}

} // namespace Kernel::Svc
