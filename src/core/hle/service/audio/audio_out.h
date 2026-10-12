// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "audio_core/audio_out_manager.h"
#include "audio_core/out/audio_out_system.h"
#include "core/hle/service/cmif_types.h"
#include "core/hle/service/kernel_helpers.h"
#include "core/hle/service/service.h"

namespace Kernel {
class KReadableEvent;
}

namespace Service::Audio {

class IAudioOut : public ServiceFramework<IAudioOut> {
public:
    explicit IAudioOut(Core::System& system_, AudioCore::AudioOut::Manager& manager,
                       size_t session_id, const std::string& device_name,
                       const AudioCore::AudioOut::AudioOutParameter& in_params,
                       Kernel::KProcess* handle, u64 applet_resource_user_id);
    ~IAudioOut() override;

    std::shared_ptr<AudioCore::AudioOut::Out> GetImpl() {
        return impl;
    }

    // Pulse: the open parameters, the play state, volume and the buffers the guest has not taken
    // back. What was mid-playback is queued again: a resume may repeat a few milliseconds of sound.
    std::string_view PulseKind() const override {
        return "IAudioOut";
    }
    void PulseCapture(Pulse::HandlerCapture& capture) const override;
    Kernel::KEvent* PulseEvent(u32 slot) override {
        return slot == 0 ? event : nullptr;
    }
    void PulseApply(Pulse::StateReader& state);

    Result GetAudioOutState(Out<u32> out_state);
    Result Start();
    Result Stop();
    Result AppendAudioOutBuffer(
        InArray<AudioCore::AudioOut::AudioOutBuffer, BufferAttr_HipcMapAlias> audio_out_buffer,
        u64 buffer_client_ptr);
    Result AppendAudioOutBufferAuto(
        InArray<AudioCore::AudioOut::AudioOutBuffer, BufferAttr_HipcAutoSelect> audio_out_buffer,
        u64 buffer_client_ptr);
    Result RegisterBufferEvent(OutCopyHandle<Kernel::KReadableEvent> out_event);
    Result GetReleasedAudioOutBuffers(OutArray<u64, BufferAttr_HipcMapAlias> out_audio_buffer,
                                      Out<u32> out_count);
    Result GetReleasedAudioOutBuffersAuto(OutArray<u64, BufferAttr_HipcAutoSelect> out_audio_buffer,
                                          Out<u32> out_count);
    Result ContainsAudioOutBuffer(Out<bool> out_contains_buffer, u64 buffer_client_ptr);
    Result GetAudioOutBufferCount(Out<u32> out_buffer_count);
    Result GetAudioOutPlayedSampleCount(Out<u64> out_played_sample_count);
    Result FlushAudioOutBuffers(Out<bool> out_flushed);
    Result SetAudioOutVolume(f32 volume);
    Result GetAudioOutVolume(Out<f32> out_volume);

private:
    KernelHelpers::ServiceContext service_context;
    Kernel::KEvent* event;
    Kernel::KProcess* process;
    std::shared_ptr<AudioCore::AudioOut::Out> impl;
    std::string pulse_device_name;
    AudioCore::AudioOut::AudioOutParameter pulse_params;
    u64 pulse_aruid;
};

} // namespace Service::Audio
