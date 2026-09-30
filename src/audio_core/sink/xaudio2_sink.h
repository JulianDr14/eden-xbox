// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string_view>
#include <vector>

#include <wrl/client.h>
#include <xaudio2.h>

#include "audio_core/sink/sink.h"

namespace AudioCore::Sink {

void SetXAudio2ProfileEnabled(bool enabled) noexcept;
std::vector<std::string> ListXAudio2SinkDevices(bool capture);
u32 GetXAudio2Latency();

class XAudio2Sink final : public Sink {
public:
    explicit XAudio2Sink(std::string_view device_id);
    ~XAudio2Sink() override;

    SinkStream* AcquireSinkStream(Core::System& system, u32 system_channels,
                                  const std::string& name, StreamType type) override;
    void CloseStream(SinkStream* stream) override;
    void CloseStreams() override;
    f32 GetDeviceVolume() const override;
    void SetDeviceVolume(f32 volume) override;
    void SetSystemVolume(f32 volume) override;

    bool IsAvailable() const noexcept;
    HRESULT FailureResult() const noexcept;
    IXAudio2* Engine() const noexcept;
    void LogPerformance(std::string_view stream_name, u64 submitted, u64 completed,
                        u64 submit_failures, u64 starvations, u32 min_depth, u32 max_depth);

private:
    class EngineCallback final : public IXAudio2EngineCallback {
    public:
        explicit EngineCallback(std::atomic<bool>& failed_) : failed{failed_} {}
        void STDMETHODCALLTYPE OnProcessingPassStart() override {}
        void STDMETHODCALLTYPE OnProcessingPassEnd() override {}
        void STDMETHODCALLTYPE OnCriticalError(HRESULT error) override;

        std::atomic<HRESULT> last_error{S_OK};

    private:
        std::atomic<bool>& failed;
    };

    bool Initialize();
    void Shutdown();

    mutable std::mutex mutex;
    std::vector<SinkStreamPtr> sink_streams;
    Microsoft::WRL::ComPtr<IXAudio2> engine;
    IXAudio2MasteringVoice* mastering_voice{};
    std::atomic<bool> failed{};
    EngineCallback engine_callback{failed};
    bool initialization_attempted{};
    bool callback_registered{};
    bool com_initialized{};
    unsigned long com_thread_id{};
};

} // namespace AudioCore::Sink
