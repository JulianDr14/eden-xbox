// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <chrono>
#include <thread>

#include "audio_core/common/common.h"

namespace AudioCore::Sink {

/// Keeps a sink without a hardware clock from letting the guest audio renderer run unbounded.
class RealtimePacer {
public:
    void Wait(const u64 frames) {
        using namespace std::chrono;
        constexpr auto max_lead = milliseconds{10};
        constexpr auto max_lag = milliseconds{50};
        const auto now = steady_clock::now();
        if (next_deadline.time_since_epoch().count() == 0 || next_deadline + max_lag < now) {
            next_deadline = now;
        }
        next_deadline += duration_cast<steady_clock::duration>(
            nanoseconds{frames * 1'000'000'000ULL / TargetSampleRate});
        if (next_deadline - now > max_lead) {
            std::this_thread::sleep_until(next_deadline - max_lead);
        }
    }

    void Reset() {
        next_deadline = {};
    }

private:
    std::chrono::steady_clock::time_point next_deadline{};
};

} // namespace AudioCore::Sink
