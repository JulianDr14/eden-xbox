// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "audio_core/sink/xaudio2_sink.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <limits>
#include <span>
#include <thread>

#include <objbase.h>

#include "audio_core/common/common.h"
#include "audio_core/sink/null_sink.h"
#include "audio_core/sink/realtime_pacer.h"
#include "audio_core/sink/sink_stream.h"
#include "common/logging.h"
#include "common/thread.h"

namespace AudioCore::Sink {
namespace {

constexpr u32 SlotCount = 3;
constexpr u32 SlotFrames = TargetSampleCount * 4;
constexpr u32 OutputChannels = 2;
constexpr u32 AllSlotsMask = (1U << SlotCount) - 1;
constexpr auto ProfilePeriod = std::chrono::seconds{5};

std::atomic<bool> profile_enabled{};

enum class StreamState : u8 {
    Stopped,
    Running,
    Failed,
    Finalizing,
};

class XAudio2SinkStream final : public SinkStream {
public:
    XAudio2SinkStream(XAudio2Sink& sink_, Core::System& system_, u32 system_channels_,
                      std::string name_, StreamType type_)
        : SinkStream{system_, type_}, sink{sink_}, callback{*this} {
        name = std::move(name_);
        system_channels = system_channels_;
        device_channels = OutputChannels;

        if (sink.IsAvailable()) {
            WAVEFORMATEX format{
                .wFormatTag = WAVE_FORMAT_PCM,
                .nChannels = static_cast<WORD>(OutputChannels),
                .nSamplesPerSec = TargetSampleRate,
                .nAvgBytesPerSec = TargetSampleRate * OutputChannels * sizeof(s16),
                .nBlockAlign = static_cast<WORD>(OutputChannels * sizeof(s16)),
                .wBitsPerSample = 16,
                .cbSize = 0,
            };
            const HRESULT result = sink.Engine()->CreateSourceVoice(
                &source_voice, &format, XAUDIO2_VOICE_NOSRC | XAUDIO2_VOICE_NOPITCH, 1.0F,
                &callback);
            if (FAILED(result)) {
                failure_result = result;
                failure_reported = true;
                state = StreamState::Failed;
                LOG_ERROR(Audio_Sink,
                          "XAudio2: CreateSourceVoice failed for '{}' (HRESULT {:#010x}); using "
                          "timed silent output",
                          name, static_cast<u32>(result));
            }
        } else {
            state = StreamState::Failed;
        }

        worker = std::jthread{[this](std::stop_token stop_token) { WorkerLoop(stop_token); }};
    }

    ~XAudio2SinkStream() override {
        Finalize();
    }

    void Finalize() override {
        const StreamState previous = state.exchange(StreamState::Finalizing);
        if (previous == StreamState::Finalizing) {
            return;
        }
        paused = true;
        {
            std::scoped_lock lock{voice_mutex};
            if (source_voice != nullptr) {
                static_cast<void>(source_voice->Stop(0));
                static_cast<void>(source_voice->FlushSourceBuffers());
            }
        }
        worker.request_stop();
        wake_cv.notify_all();
        if (worker.joinable()) {
            worker.join();
        }
        RetireVoice();
        LOG_INFO(Service_Audio,
                 "XAudio2 stream '{}' closed ({} submitted, {} completed, {} submit failures, {} "
                 "starvations, queue {}..{})",
                 name, submitted.load(), completed.load(), submit_failures.load(),
                 starvations.load(), ReportedMinDepth(), max_depth.load());
    }

    void Start(bool resume = false) override {
        if (state == StreamState::Finalizing || !paused) {
            return;
        }
        paused = false;
        if (state != StreamState::Failed) {
            state = StreamState::Running;
        }
        if (!resume) {
            pacer.Reset();
        }
        // Stop deliberately leaves queued voice buffers intact. Resuming them avoids reusing a
        // slot while XAudio2 may still own it, and preserves sample-continuous playback.
        std::scoped_lock lock{voice_mutex};
        if (source_voice != nullptr && voice_started) {
            const HRESULT result = source_voice->Start(0);
            if (FAILED(result)) {
                Fail(result, "source voice resume");
            }
        }
        wake_cv.notify_all();
    }

    void Stop() override {
        if (state == StreamState::Finalizing || paused) {
            return;
        }
        SignalPause();
        std::scoped_lock lock{voice_mutex};
        if (source_voice != nullptr) {
            static_cast<void>(source_voice->Stop(0));
        }
        if (state != StreamState::Failed) {
            state = StreamState::Stopped;
        }
        pacer.Reset();
        wake_cv.notify_all();
    }

private:
    class VoiceCallback final : public IXAudio2VoiceCallback {
    public:
        explicit VoiceCallback(XAudio2SinkStream& owner_) : owner{owner_} {}
        void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) override {}
        void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() override {}
        void STDMETHODCALLTYPE OnStreamEnd() override {}
        void STDMETHODCALLTYPE OnBufferStart(void*) override {}
        void STDMETHODCALLTYPE OnLoopEnd(void*) override {}
        void STDMETHODCALLTYPE OnBufferEnd(void* context) override {
            const auto slot = static_cast<u32>(reinterpret_cast<std::uintptr_t>(context) - 1);
            if (slot < SlotCount) {
                owner.completed.fetch_add(1, std::memory_order_relaxed);
                owner.free_slots.fetch_or(1U << slot, std::memory_order_release);
                owner.wake_cv.notify_one();
            }
        }
        void STDMETHODCALLTYPE OnVoiceError(void*, HRESULT error) override {
            owner.failure_result.store(error, std::memory_order_relaxed);
            owner.state.store(StreamState::Failed, std::memory_order_release);
            owner.wake_cv.notify_one();
        }

    private:
        XAudio2SinkStream& owner;
    };

    bool TakeFreeSlot(u32& slot) {
        u32 available = free_slots.load(std::memory_order_acquire);
        while (available != 0) {
            const u32 bit = available & (~available + 1U);
            if (free_slots.compare_exchange_weak(available, available & ~bit,
                                                 std::memory_order_acq_rel,
                                                 std::memory_order_acquire)) {
                slot = static_cast<u32>(std::countr_zero(bit));
                return true;
            }
        }
        return false;
    }

    void FillSlot(const u32 slot) {
        ProcessAudioOutAndRender(std::span<s16>{slots[slot].data(), slots[slot].size()},
                                 SlotFrames);
    }

    void Fail(const HRESULT result, std::string_view operation) {
        failure_result = result;
        const StreamState previous = state.exchange(StreamState::Failed);
        if (previous != StreamState::Failed && previous != StreamState::Finalizing) {
            LOG_ERROR(Audio_Sink,
                      "XAudio2: {} failed for '{}' (HRESULT {:#010x}); using timed silent output",
                      operation, name, static_cast<u32>(result));
            failure_reported = true;
        }
        wake_cv.notify_one();
    }

    void RetireVoice() {
        std::scoped_lock lock{voice_mutex};
        if (source_voice == nullptr) {
            return;
        }
        static_cast<void>(source_voice->Stop(0));
        static_cast<void>(source_voice->FlushSourceBuffers());
        source_voice->DestroyVoice();
        source_voice = nullptr;
        voice_started = false;
        // DestroyVoice does not return until callbacks for this voice have finished. Only now may
        // the fallback path reuse storage that was referenced by queued XAUDIO2_BUFFER objects.
        free_slots.store(AllSlotsMask, std::memory_order_release);
    }

    void RunFallback(std::stop_token stop_token) {
        FillSlot(0);
        pacer.Wait(SlotFrames);
        if (stop_token.stop_requested()) {
            return;
        }
    }

    void MaybeLogProfile(std::chrono::steady_clock::time_point& next_profile) {
        if (!profile_enabled.load(std::memory_order_relaxed)) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now < next_profile) {
            return;
        }
        next_profile = now + ProfilePeriod;
        sink.LogPerformance(name, submitted.load(std::memory_order_relaxed),
                            completed.load(std::memory_order_relaxed),
                            submit_failures.load(std::memory_order_relaxed),
                            starvations.load(std::memory_order_relaxed),
                            ReportedMinDepth(),
                            max_depth.load(std::memory_order_relaxed));
    }

    u32 ReportedMinDepth() const {
        return submitted.load(std::memory_order_relaxed) == 0
                   ? 0
                   : min_depth.load(std::memory_order_relaxed);
    }

    void RecordQueueDepth() {
        const u64 sent = submitted.load(std::memory_order_relaxed);
        const u64 done = completed.load(std::memory_order_relaxed);
        if (sent == 0) {
            return;
        }
        const u32 depth = static_cast<u32>(std::min<u64>(sent > done ? sent - done : 0, SlotCount));
        u32 minimum = min_depth.load(std::memory_order_relaxed);
        while (depth < minimum &&
               !min_depth.compare_exchange_weak(minimum, depth, std::memory_order_relaxed)) {}
        u32 maximum = max_depth.load(std::memory_order_relaxed);
        while (depth > maximum &&
               !max_depth.compare_exchange_weak(maximum, depth, std::memory_order_relaxed)) {}
        if (depth == 0) {
            if (!queue_empty_latched) {
                starvations.fetch_add(1, std::memory_order_relaxed);
                queue_empty_latched = true;
            }
        } else {
            queue_empty_latched = false;
        }
    }

    void WorkerLoop(std::stop_token stop_token) {
        Common::SetCurrentThreadName("XAudio2Output");
        const HRESULT com_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool uninitialize_com = SUCCEEDED(com_result);
        auto next_profile = std::chrono::steady_clock::now() + ProfilePeriod;

        while (!stop_token.stop_requested()) {
            if (paused || state == StreamState::Stopped) {
                std::unique_lock lock{wake_mutex};
                wake_cv.wait(lock, stop_token,
                             [this] { return !paused.load() || state == StreamState::Finalizing; });
                continue;
            }
            if (state == StreamState::Finalizing) {
                break;
            }
            if (state == StreamState::Failed || !sink.IsAvailable()) {
                if (state != StreamState::Failed) {
                    Fail(sink.FailureResult(), "audio engine");
                } else if (!failure_reported.exchange(true, std::memory_order_acq_rel)) {
                    const HRESULT result = failure_result.load(std::memory_order_relaxed);
                    LOG_ERROR(Audio_Sink,
                              "XAudio2: source voice callback failed for '{}' (HRESULT {:#010x}); "
                              "using timed silent output",
                              name, static_cast<u32>(result));
                }
                RetireVoice();
                RunFallback(stop_token);
                MaybeLogProfile(next_profile);
                continue;
            }

            RecordQueueDepth();

            bool submitted_any = false;
            for (u32 slot{}; TakeFreeSlot(slot);) {
                FillSlot(slot);
                XAUDIO2_BUFFER buffer{};
                buffer.AudioBytes = static_cast<UINT32>(slots[slot].size() * sizeof(s16));
                buffer.pAudioData = reinterpret_cast<const BYTE*>(slots[slot].data());
                buffer.pContext = reinterpret_cast<void*>(static_cast<std::uintptr_t>(slot + 1));
                HRESULT result{};
                {
                    std::scoped_lock lock{voice_mutex};
                    if (source_voice == nullptr || paused || state != StreamState::Running) {
                        free_slots.fetch_or(1U << slot, std::memory_order_release);
                        break;
                    }
                    result = source_voice->SubmitSourceBuffer(&buffer);
                }
                if (FAILED(result)) {
                    free_slots.fetch_or(1U << slot, std::memory_order_release);
                    submit_failures.fetch_add(1, std::memory_order_relaxed);
                    Fail(result, "SubmitSourceBuffer");
                    break;
                }
                submitted.fetch_add(1, std::memory_order_relaxed);
                submitted_any = true;
            }
            RecordQueueDepth();

            if (submitted_any && state == StreamState::Running) {
                HRESULT result = S_OK;
                {
                    std::scoped_lock lock{voice_mutex};
                    if (source_voice != nullptr && !voice_started) {
                        result = source_voice->Start(0);
                        voice_started = SUCCEEDED(result);
                    }
                }
                if (FAILED(result)) {
                    Fail(result, "source voice Start");
                }
            }
            MaybeLogProfile(next_profile);
            std::unique_lock lock{wake_mutex};
            wake_cv.wait_for(lock, stop_token, std::chrono::milliseconds{10}, [this] {
                return free_slots.load(std::memory_order_acquire) != 0 || paused.load() ||
                       state.load() == StreamState::Failed ||
                       state.load() == StreamState::Finalizing;
            });
        }

        if (uninitialize_com) {
            CoUninitialize();
        }
    }

    XAudio2Sink& sink;
    VoiceCallback callback;
    IXAudio2SourceVoice* source_voice{};
    std::array<std::array<s16, SlotFrames * OutputChannels>, SlotCount> slots{};
    std::atomic<u32> free_slots{AllSlotsMask};
    std::atomic<StreamState> state{StreamState::Stopped};
    bool voice_started{};
    std::atomic<HRESULT> failure_result{S_OK};
    std::atomic<bool> failure_reported{};
    std::atomic<u64> submitted{};
    std::atomic<u64> completed{};
    std::atomic<u64> submit_failures{};
    std::atomic<u64> starvations{};
    std::atomic<u32> min_depth{SlotCount};
    std::atomic<u32> max_depth{};
    bool queue_empty_latched{};
    std::condition_variable_any wake_cv;
    std::mutex wake_mutex;
    std::mutex voice_mutex;
    std::jthread worker;
    RealtimePacer pacer;
};

} // Anonymous namespace

void SetXAudio2ProfileEnabled(const bool enabled) noexcept {
    profile_enabled.store(enabled, std::memory_order_relaxed);
}

std::vector<std::string> ListXAudio2SinkDevices(const bool capture) {
    return {capture ? "null" : auto_device_name};
}

u32 GetXAudio2Latency() {
    return SlotFrames * SlotCount;
}

void XAudio2Sink::EngineCallback::OnCriticalError(const HRESULT error) {
    last_error.store(error, std::memory_order_relaxed);
    failed.store(true, std::memory_order_release);
}

XAudio2Sink::XAudio2Sink(std::string_view) {}

XAudio2Sink::~XAudio2Sink() {
    Shutdown();
}

bool XAudio2Sink::Initialize() {
    if (initialization_attempted) {
        return IsAvailable();
    }
    initialization_attempted = true;

    const HRESULT com_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    com_initialized = SUCCEEDED(com_result);
    if (com_initialized) {
        com_thread_id = GetCurrentThreadId();
    }
    if (FAILED(com_result) && com_result != RPC_E_CHANGED_MODE) {
        LOG_ERROR(Audio_Sink, "XAudio2: CoInitializeEx failed (HRESULT {:#010x})",
                  static_cast<u32>(com_result));
    }

    HRESULT result = XAudio2Create(engine.GetAddressOf(), 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(result)) {
        failed = true;
        LOG_ERROR(Audio_Sink,
                  "XAudio2Create failed (HRESULT {:#010x}); using timed silent output",
                  static_cast<u32>(result));
        return false;
    }
    result = engine->RegisterForCallbacks(&engine_callback);
    if (FAILED(result)) {
        failed = true;
        LOG_ERROR(Audio_Sink,
                  "XAudio2 RegisterForCallbacks failed (HRESULT {:#010x}); using timed silent "
                  "output",
                  static_cast<u32>(result));
        return false;
    }
    callback_registered = true;
    result = engine->CreateMasteringVoice(&mastering_voice, OutputChannels, TargetSampleRate, 0,
                                          nullptr, nullptr, AudioCategory_GameMedia);
    if (FAILED(result)) {
        failed = true;
        LOG_ERROR(Audio_Sink,
                  "XAudio2 CreateMasteringVoice failed (HRESULT {:#010x}); using timed silent "
                  "output",
                  static_cast<u32>(result));
        return false;
    }

    LOG_INFO(Service_Audio,
             "XAudio2 output ready (PCM16, {} Hz, stereo, {} x {} frames = {} ms queued)",
             TargetSampleRate, SlotCount, SlotFrames,
             SlotCount * SlotFrames * 1000 / TargetSampleRate);
    return true;
}

void XAudio2Sink::Shutdown() {
    CloseStreams();
    if (engine != nullptr && callback_registered) {
        engine->UnregisterForCallbacks(&engine_callback);
        callback_registered = false;
    }
    if (mastering_voice != nullptr) {
        mastering_voice->DestroyVoice();
        mastering_voice = nullptr;
    }
    engine.Reset();
    if (com_initialized && com_thread_id == GetCurrentThreadId()) {
        CoUninitialize();
        com_initialized = false;
    }
}

SinkStream* XAudio2Sink::AcquireSinkStream(Core::System& system, const u32 system_channels_,
                                           const std::string& name, const StreamType type) {
    std::scoped_lock lock{mutex};
    system_channels = system_channels_;
    SinkStreamPtr stream;
    if (type == StreamType::In) {
        stream = std::make_unique<NullSinkStreamImpl>(system, type);
        stream->SetSystemChannels(system_channels_);
    } else {
        static_cast<void>(Initialize());
        stream = std::make_unique<XAudio2SinkStream>(*this, system, system_channels_, name, type);
    }
    stream->SetDeviceVolume(device_volume);
    auto* result = stream.get();
    sink_streams.emplace_back(std::move(stream));
    return result;
}

void XAudio2Sink::CloseStream(SinkStream* stream) {
    std::scoped_lock lock{mutex};
    const auto it = std::find_if(sink_streams.begin(), sink_streams.end(),
                                 [stream](const auto& item) { return item.get() == stream; });
    if (it != sink_streams.end()) {
        sink_streams.erase(it);
    }
}

void XAudio2Sink::CloseStreams() {
    std::vector<SinkStreamPtr> streams;
    {
        std::scoped_lock lock{mutex};
        streams.swap(sink_streams);
    }
    streams.clear();
}

f32 XAudio2Sink::GetDeviceVolume() const {
    return device_volume;
}

void XAudio2Sink::SetDeviceVolume(const f32 volume) {
    std::scoped_lock lock{mutex};
    device_volume = volume;
    for (auto& stream : sink_streams) {
        stream->SetDeviceVolume(volume);
    }
}

void XAudio2Sink::SetSystemVolume(const f32 volume) {
    std::scoped_lock lock{mutex};
    for (auto& stream : sink_streams) {
        stream->SetSystemVolume(volume);
    }
}

bool XAudio2Sink::IsAvailable() const noexcept {
    return engine != nullptr && mastering_voice != nullptr && !failed.load(std::memory_order_acquire);
}

HRESULT XAudio2Sink::FailureResult() const noexcept {
    const HRESULT result = engine_callback.last_error.load(std::memory_order_relaxed);
    return FAILED(result) ? result : E_FAIL;
}

IXAudio2* XAudio2Sink::Engine() const noexcept {
    return engine.Get();
}

void XAudio2Sink::LogPerformance(const std::string_view stream_name, const u64 submitted_,
                                 const u64 completed_, const u64 submit_failures_,
                                 const u64 starvations_, const u32 min_depth_,
                                 const u32 max_depth_) {
    if (!profile_enabled.load(std::memory_order_relaxed) || engine == nullptr) {
        return;
    }
    XAUDIO2_PERFORMANCE_DATA performance{};
    engine->GetPerformanceData(&performance);
    LOG_INFO(Service_Audio,
             "XAudio2 profile '{}': submitted {} completed {} failures {}, starvations {}, queue "
             "{}..{}, latency {} samples, glitches {}, voices {}/{}, memory {} KiB",
             stream_name, submitted_, completed_, submit_failures_, starvations_, min_depth_,
             max_depth_, performance.CurrentLatencyInSamples,
             performance.GlitchesSinceEngineStarted, performance.ActiveSourceVoiceCount,
             performance.TotalSourceVoiceCount, performance.MemoryUsageInBytes / 1024);
}

} // namespace AudioCore::Sink
