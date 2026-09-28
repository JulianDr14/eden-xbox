// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "audio_core/common/common.h"
#include "audio_core/sink/sink.h"
#include "audio_core/sink/sink_stream.h"

namespace Core {
class System;
} // namespace Core

namespace AudioCore::Sink {
class NullSinkStreamImpl final : public SinkStream {
public:
    explicit NullSinkStreamImpl(Core::System& system_, StreamType type_)
        : SinkStream{system_, type_} {}
    ~NullSinkStreamImpl() override {}

    /// The audio renderer only waits for a sink with a full queue (WaitFreeSpace), and this one
    /// never queues anything: without a clock it rendered nonstop, a whole host core, and woke the
    /// guest's audio thread far more often than 200 times a second. Play each rendered buffer in
    /// real time instead, as a device would, allowing a little lead for timer jitter.
    void AppendBuffer(SinkBuffer& buffer, std::span<s16>) override {
        if (type != StreamType::Render) {
            return;
        }
        using namespace std::chrono;
        constexpr auto max_lead = milliseconds{10};
        constexpr auto max_lag = milliseconds{50};
        const auto now = steady_clock::now();
        if (next_deadline + max_lag < now) {
            // Behind by more than a few buffers (loading, a paused guest): restart the clock
            // instead of rendering the backlog at full speed.
            next_deadline = now;
        }
        next_deadline += duration_cast<steady_clock::duration>(
            nanoseconds{buffer.frames * 1'000'000'000ULL / TargetSampleRate});
        if (next_deadline - now > max_lead) {
            std::this_thread::sleep_until(next_deadline - max_lead);
        }
    }
    std::vector<s16> ReleaseBuffer(u64) override {
        return {};
    }

private:
    std::chrono::steady_clock::time_point next_deadline{};
};

/**
 * A no-op sink for when no audio out is wanted.
 */
class NullSink final : public Sink {
public:
    explicit NullSink(std::string_view) {}
    ~NullSink() override = default;

    SinkStream* AcquireSinkStream(Core::System& system, u32, const std::string&,
                                  StreamType type) override {
        if (null_sink == nullptr) {
            null_sink = std::make_unique<NullSinkStreamImpl>(system, type);
            null_sink->SetDeviceVolume(device_volume);
        }
        return null_sink.get();
    }

    void CloseStream(SinkStream*) override {}
    void CloseStreams() override {}
    f32 GetDeviceVolume() const override {
        return device_volume;
    }
    void SetDeviceVolume(f32 volume) override {
        device_volume = volume;
        if (null_sink != nullptr) {
            null_sink->SetDeviceVolume(volume);
        }
    }
    void SetSystemVolume(f32 volume) override {}

private:
    SinkStreamPtr null_sink{};
};

} // namespace AudioCore::Sink
