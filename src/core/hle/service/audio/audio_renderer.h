// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "audio_core/common/audio_renderer_parameter.h"
#include "audio_core/renderer/audio_renderer.h"
#include "core/hle/service/cmif_types.h"
#include "core/hle/service/kernel_helpers.h"
#include "core/hle/service/service.h"

namespace Kernel {
class KReadableEvent;
}

namespace Service::Audio {

class IAudioRenderer final : public ServiceFramework<IAudioRenderer> {
public:
    explicit IAudioRenderer(Core::System& system_, AudioCore::Renderer::Manager& manager_,
                            AudioCore::AudioRendererParameterInternal& params,
                            Kernel::KTransferMemory* transfer_memory, u64 transfer_memory_size,
                            Kernel::KProcess* process_handle_, u64 applet_resource_user_id,
                            s32 session_id);
    ~IAudioRenderer() override;

    // Pulse: the open parameters and the renderer's logical state (System::PulseSave); never
    // samples. A restore opens the renderer again with the same parameters, then applies it.
    std::string_view PulseKind() const override {
        return "IAudioRenderer";
    }
    void PulseCapture(Pulse::HandlerCapture& capture) const override;
    Kernel::KEvent* PulseEvent(u32 slot) override {
        return slot == 0 ? rendered_event : nullptr;
    }
    void PulseApply(Pulse::StateReader& state);

private:
    Result GetSampleRate(Out<u32> out_sample_rate);
    Result GetSampleCount(Out<u32> out_sample_count);
    Result GetState(Out<u32> out_state);
    Result GetMixBufferCount(Out<u32> out_mix_buffer_count);
    Result RequestUpdate(OutBuffer<BufferAttr_HipcMapAlias> out_buffer,
                         OutBuffer<BufferAttr_HipcMapAlias> out_performance_buffer,
                         InBuffer<BufferAttr_HipcMapAlias> input);
    Result RequestUpdateAuto(OutBuffer<BufferAttr_HipcAutoSelect> out_buffer,
                             OutBuffer<BufferAttr_HipcAutoSelect> out_performance_buffer,
                             InBuffer<BufferAttr_HipcAutoSelect> input);
    Result Start();
    Result Stop();
    Result QuerySystemEvent(OutCopyHandle<Kernel::KReadableEvent> out_event);
    Result SetRenderingTimeLimit(u32 rendering_time_limit);
    Result GetRenderingTimeLimit(Out<u32> out_rendering_time_limit);
    Result SetVoiceDropParameter(f32 voice_drop_parameter);
    Result GetVoiceDropParameter(Out<f32> out_voice_drop_parameter);

    KernelHelpers::ServiceContext service_context;
    Kernel::KEvent* rendered_event;
    AudioCore::Renderer::Manager& manager;
    std::unique_ptr<AudioCore::Renderer::Renderer> impl;
    Kernel::KProcess* process_handle;
    Common::ScratchBuffer<u8> output_buffer;
    Common::ScratchBuffer<u8> performance_buffer;
    AudioCore::AudioRendererParameterInternal pulse_params;
    u64 pulse_transfer_memory_size;
    u64 pulse_aruid;
};

} // namespace Service::Audio
