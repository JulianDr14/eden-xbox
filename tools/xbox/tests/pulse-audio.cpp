// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
#include "audio_core/sink/xaudio2_sink.h"
#include "audio_core/sink/sink_stream.h"
#include "core/core.h"
#include "core/core_timing.h"

// Host-backend harness: supply only the System clock/lifecycle calls used by the real sink.
// No guest, DSP or kernel is running; this tests worker/engine quiescence, not audio restoration.
namespace Core {
struct System::Impl { Timing::CoreTiming clock; };
System::System() : impl{std::make_unique<Impl>()} {
    impl->clock.SetMulticore(true);
    impl->clock.Initialize([] {});
}
System::~System() = default;
bool System::IsPaused() const { return false; }
bool System::IsShuttingDown() const { return false; }
Timing::CoreTiming& System::CoreTiming() { return impl->clock; }
const Timing::CoreTiming& System::CoreTiming() const { return impl->clock; }
}

int main() {
    using namespace AudioCore::Sink;
    try {
        Core::System system;
        XAudio2Sink sink("auto");
        auto* stream = sink.AcquireSinkStream(system, 2, "Pulse gate", StreamType::Out);
        // Exercise parking before Start as well as while the worker is producing output.
        sink.BeginCheckpoint();
        sink.EndCheckpoint();
        stream->Start();
        SinkBuffer long_buffer{.frames = 16000, .frames_played = 0,
                               .tag = 1000, .consumed = false};
        std::vector<s16> initial_silence(32000);
        stream->AppendBuffer(long_buffer, initial_silence);
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
        for (int cycle = 0; cycle < 100; ++cycle) {
            SinkBuffer buffer{.frames = 128, .frames_played = 0,
                              .tag = static_cast<u64>(cycle), .consumed = false};
            std::array<s16, 256> silence{};
            stream->AppendBuffer(buffer, silence);
            sink.BeginCheckpoint();
            const auto queued = stream->GetQueueSize();
            std::this_thread::sleep_for(std::chrono::milliseconds{2});
            if (stream->GetQueueSize() != queued) {
                throw std::runtime_error("audio queue mutated behind checkpoint barrier");
            }
            bool rejected = false;
            try { sink.BeginCheckpoint(); }
            catch (const std::logic_error&) { rejected = true; }
            if (!rejected) throw std::runtime_error("nested audio checkpoint accepted");
            sink.EndCheckpoint();
            sink.EndCheckpoint(); // harmless cleanup after an already-released barrier
        }
        std::cout << "Pulse audio: 100 park/cancel cycles passed ("
                  << (sink.IsAvailable() ? "native XAudio2" : "timed fallback") << ")\n";
        // Releasing a stream while parked must join its worker, without waiting for EndCheckpoint.
        sink.BeginCheckpoint();
        sink.CloseStream(stream);
        sink.EndCheckpoint();
        auto* input = sink.AcquireSinkStream(system, 2, "input", StreamType::In);
        bool rejected = false;
        try { sink.BeginCheckpoint(); }
        catch (const std::runtime_error&) { rejected = true; }
        sink.CloseStream(input);
        if (!rejected) throw std::runtime_error("unsupported capture device accepted");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
