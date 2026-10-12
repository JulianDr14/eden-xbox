// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <span>

#include "audio_core/adsp/apps/audio_renderer/audio_renderer.h"
#include "audio_core/adsp/apps/audio_renderer/command_buffer.h"
#include "audio_core/audio_core.h"
#include "audio_core/common/audio_renderer_parameter.h"
#include "audio_core/common/common.h"
#include "audio_core/common/feature_support.h"
#include "audio_core/common/workbuffer_allocator.h"
#include "audio_core/renderer/behavior/info_updater.h"
#include "audio_core/renderer/command/command_buffer.h"
#include "audio_core/renderer/command/command_generator.h"
#include "audio_core/renderer/command/command_list_header.h"
#include "audio_core/renderer/effect/effect_info_base.h"
#include "audio_core/renderer/effect/effect_reset.h"
#include "audio_core/renderer/effect/effect_result_state.h"
#include "audio_core/renderer/memory/memory_pool_info.h"
#include "audio_core/renderer/memory/pool_mapper.h"
#include "audio_core/renderer/mix/mix_info.h"
#include "audio_core/renderer/nodes/edge_matrix.h"
#include "audio_core/renderer/nodes/node_states.h"
#include "audio_core/renderer/sink/sink_info_base.h"
#include "audio_core/renderer/system.h"
#include "audio_core/renderer/upsampler/upsampler_info.h"
#include "audio_core/renderer/voice/voice_channel_resource.h"
#include "audio_core/renderer/voice/voice_info.h"
#include "audio_core/renderer/voice/voice_state.h"
#include "common/alignment.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/hle/kernel/k_event.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_transfer_memory.h"
#include "core/hle/service/pulse_service.h"
#include "core/memory.h"

namespace AudioCore::Renderer {

u64 System::GetWorkBufferSize(const AudioRendererParameterInternal& params) {
    BehaviorInfo behavior;
    behavior.SetUserLibRevision(params.revision);

    u64 size{0};

    size += Common::AlignUp(params.mixes * sizeof(s32), 0x40);
    size += params.sub_mixes * MaxEffects * sizeof(s32);
    size += (params.sub_mixes + 1) * sizeof(MixInfo);
    size += params.voices * (sizeof(VoiceInfo) + sizeof(VoiceChannelResource) + sizeof(VoiceState));
    size += Common::AlignUp((params.sub_mixes + 1) * sizeof(MixInfo*), 0x10);
    size += Common::AlignUp(params.voices * sizeof(VoiceInfo*), 0x10);
    size += Common::AlignUp(((params.sinks + params.sub_mixes) * TargetSampleCount * sizeof(s32) +
                             params.sample_count * sizeof(s32)) *
                                (params.mixes + MaxChannels),
                            0x40);

    if (behavior.IsSplitterSupported()) {
        const auto node_size{NodeStates::GetWorkBufferSize(params.sub_mixes + 1)};
        const auto edge_size{EdgeMatrix::GetWorkBufferSize(params.sub_mixes + 1)};
        size += Common::AlignUp(node_size + edge_size, 0x10);
    }

    size += SplitterContext::CalcWorkBufferSize(behavior, params);
    size += (params.effects + params.voices * MaxWaveBuffers) * sizeof(MemoryPoolInfo);

    if (behavior.IsEffectInfoVersion2Supported()) {
        size += params.effects * sizeof(EffectResultState);
    }
    size += 0x50;

    size = Common::AlignUp(size, 0x40);

    size += (params.sinks + params.sub_mixes) * sizeof(UpsamplerInfo);
    size += params.effects * sizeof(EffectInfoBase);
    size += Common::AlignUp(params.voices * sizeof(VoiceState), 0x40);
    size += params.sinks * sizeof(SinkInfoBase);

    if (behavior.IsEffectInfoVersion2Supported()) {
        size += params.effects * sizeof(EffectResultState);
    }

    if (params.perf_frames > 0) {
        auto perf_size{PerformanceManager::GetRequiredBufferSizeForPerformanceMetricsPerFrame(
            behavior, params)};
        size += Common::AlignUp(perf_size * (params.perf_frames + 1) + 0xC0, 0x100);
    }

    if (behavior.IsVariadicCommandBufferSizeSupported()) {
        size += CommandGenerator::CalculateCommandBufferSize(behavior, params) + (0x40 - 1) * 2;
    } else {
        size += 0x18000 + (0x40 - 1) * 2;
    }

    size = Common::AlignUp(size, 0x1000);
    return size;
}

System::System(Core::System& core_, Kernel::KEvent* adsp_rendered_event_)
    : core{core_}, audio_renderer{core.AudioCore().ADSP().AudioRenderer()},
      adsp_rendered_event{adsp_rendered_event_} {}

Result System::Initialize(const AudioRendererParameterInternal& params,
                          Kernel::KTransferMemory* transfer_memory, u64 transfer_memory_size,
                          Kernel::KProcess* process_handle_, u64 applet_resource_user_id_,
                          s32 session_id_) {
    if (!CheckValidRevision(params.revision)) {
        return Service::Audio::ResultInvalidRevision;
    }

    if (GetWorkBufferSize(params) > transfer_memory_size) {
        return Service::Audio::ResultInsufficientBuffer;
    }

    if (process_handle_ == 0) {
        return Service::Audio::ResultInvalidHandle;
    }

    behavior.SetUserLibRevision(params.revision);

    process_handle = process_handle_;
    applet_resource_user_id = applet_resource_user_id_;
    session_id = session_id_;

    sample_rate = params.sample_rate;
    sample_count = params.sample_count;
    mix_buffer_count = static_cast<s16>(params.mixes);
    voice_channels = MaxChannels;
    upsampler_count = params.sinks + params.sub_mixes;
    memory_pool_count = params.effects + params.voices * MaxWaveBuffers;
    render_device = params.rendering_device;
    execution_mode = params.execution_mode;

    // A Pulse restore passes no transfer memory: guest memory already holds what it had.
    if (transfer_memory) {
        process_handle->GetMemory().ZeroBlock(transfer_memory->GetSourceAddress(),
                                              transfer_memory_size);
    }

    // Note: We're not actually using the transfer memory because it's a pain to code for.
    // Allocate the memory normally instead and hope the game doesn't try to read anything back
    workbuffer = std::make_unique<u8[]>(transfer_memory_size);
    workbuffer_size = transfer_memory_size;

    PoolMapper pool_mapper(process_handle, false);
    pool_mapper.InitializeSystemPool(memory_pool_info, workbuffer.get(), workbuffer_size);

    WorkbufferAllocator allocator({workbuffer.get(), workbuffer_size}, workbuffer_size);

    samples_workbuffer =
        allocator.Allocate<s32>((voice_channels + mix_buffer_count) * sample_count, 0x10);
    if (samples_workbuffer.empty()) {
        return Service::Audio::ResultInsufficientBuffer;
    }

    auto upsampler_workbuffer{allocator.Allocate<s32>(
        (voice_channels + mix_buffer_count) * TargetSampleCount * upsampler_count, 0x10)};
    if (upsampler_workbuffer.empty()) {
        return Service::Audio::ResultInsufficientBuffer;
    }

    depop_buffer =
        allocator.Allocate<s32>(Common::AlignUp(static_cast<u32>(mix_buffer_count), 0x40), 0x40);
    if (depop_buffer.empty()) {
        return Service::Audio::ResultInsufficientBuffer;
    }

    // invalidate samples_workbuffer DSP cache

    auto voice_infos{allocator.Allocate<VoiceInfo>(params.voices, 0x10)};
    for (auto& voice_info : voice_infos) {
        std::construct_at<VoiceInfo>(&voice_info);
    }

    if (voice_infos.empty()) {
        return Service::Audio::ResultInsufficientBuffer;
    }

    auto sorted_voice_infos{allocator.Allocate<VoiceInfo*>(params.voices, 0x10)};
    if (sorted_voice_infos.empty()) {
        return Service::Audio::ResultInsufficientBuffer;
    }

    std::memset(sorted_voice_infos.data(), 0, sorted_voice_infos.size_bytes());

    auto voice_channel_resources{allocator.Allocate<VoiceChannelResource>(params.voices, 0x10)};
    u32 i{0};
    for (auto& voice_channel_resource : voice_channel_resources) {
        std::construct_at<VoiceChannelResource>(&voice_channel_resource, i++);
    }

    if (voice_channel_resources.empty()) {
        return Service::Audio::ResultInsufficientBuffer;
    }

    auto voice_cpu_states{allocator.Allocate<VoiceState>(params.voices, 0x10)};
    if (voice_cpu_states.empty()) {
        return Service::Audio::ResultInsufficientBuffer;
    }

    for (auto& voice_state : voice_cpu_states) {
        voice_state = {};
    }

    auto mix_infos{allocator.Allocate<MixInfo>(params.sub_mixes + 1, 0x10)};

    if (mix_infos.empty()) {
        return Service::Audio::ResultInsufficientBuffer;
    }

    u32 effect_process_order_count{0};
    std::span<s32> effect_process_order_buffer{};

    if (params.effects > 0) {
        effect_process_order_count = params.effects * (params.sub_mixes + 1);
        effect_process_order_buffer = allocator.Allocate<s32>(effect_process_order_count, 0x10);
        if (effect_process_order_buffer.empty()) {
            return Service::Audio::ResultInsufficientBuffer;
        }
    }

    i = 0;
    for (auto& mix_info : mix_infos) {
        std::construct_at<MixInfo>(
            &mix_info, effect_process_order_buffer.subspan(i * params.effects, params.effects),
            params.effects, this->behavior);
        i++;
    }

    auto sorted_mix_infos{allocator.Allocate<MixInfo*>(params.sub_mixes + 1, 0x10)};
    if (sorted_mix_infos.empty()) {
        return Service::Audio::ResultInsufficientBuffer;
    }

    std::memset(sorted_mix_infos.data(), 0, sorted_mix_infos.size_bytes());

    if (behavior.IsSplitterSupported()) {
        u64 node_state_size{NodeStates::GetWorkBufferSize(params.sub_mixes + 1)};
        u64 edge_matrix_size{EdgeMatrix::GetWorkBufferSize(params.sub_mixes + 1)};

        auto node_states_workbuffer{allocator.Allocate<u8>(node_state_size, 1)};
        auto edge_matrix_workbuffer{allocator.Allocate<u8>(edge_matrix_size, 1)};

        if (node_states_workbuffer.empty() || edge_matrix_workbuffer.size() == 0) {
            return Service::Audio::ResultInsufficientBuffer;
        }

        mix_context.Initialize(sorted_mix_infos, mix_infos, params.sub_mixes + 1,
                               effect_process_order_buffer, effect_process_order_count,
                               node_states_workbuffer, node_state_size, edge_matrix_workbuffer,
                               edge_matrix_size);
    } else {
        mix_context.Initialize(sorted_mix_infos, mix_infos, params.sub_mixes + 1,
                               effect_process_order_buffer, effect_process_order_count, {}, 0, {},
                               0);
    }

    upsampler_manager = allocator.Allocate<UpsamplerManager>(1, 0x10).data();
    if (upsampler_manager == nullptr) {
        return Service::Audio::ResultInsufficientBuffer;
    }

    memory_pool_workbuffer = allocator.Allocate<MemoryPoolInfo>(memory_pool_count, 0x10);
    for (auto& memory_pool : memory_pool_workbuffer) {
        std::construct_at<MemoryPoolInfo>(&memory_pool, MemoryPoolInfo::Location::DSP);
    }

    if (memory_pool_workbuffer.empty() && memory_pool_count > 0) {
        return Service::Audio::ResultInsufficientBuffer;
    }

    if (!splitter_context.Initialize(behavior, params, allocator)) {
        return Service::Audio::ResultInsufficientBuffer;
    }

    std::span<EffectResultState> effect_result_states_cpu{};
    if (behavior.IsEffectInfoVersion2Supported() && params.effects > 0) {
        effect_result_states_cpu = allocator.Allocate<EffectResultState>(params.effects, 0x10);
        if (effect_result_states_cpu.empty()) {
            return Service::Audio::ResultInsufficientBuffer;
        }
        std::memset(effect_result_states_cpu.data(), 0, effect_result_states_cpu.size_bytes());
    }

    allocator.Align(0x40);

    unk_2B0 = allocator.GetSize() - allocator.GetCurrentOffset();
    unk_2A8 = {&workbuffer[allocator.GetCurrentOffset()], unk_2B0};

    upsampler_infos = allocator.Allocate<UpsamplerInfo>(upsampler_count, 0x40);
    for (auto& upsampler_info : upsampler_infos) {
        std::construct_at<UpsamplerInfo>(&upsampler_info);
    }

    std::construct_at<UpsamplerManager>(upsampler_manager, upsampler_count, upsampler_infos,
                                        upsampler_workbuffer);

    if (upsampler_infos.empty()) {
        return Service::Audio::ResultInsufficientBuffer;
    }

    auto effect_infos{allocator.Allocate<EffectInfoBase>(params.effects, 0x40)};
    for (auto& effect_info : effect_infos) {
        std::construct_at<EffectInfoBase>(&effect_info);
    }

    if (effect_infos.empty() && params.effects > 0) {
        return Service::Audio::ResultInsufficientBuffer;
    }

    std::span<EffectResultState> effect_result_states_dsp{};
    if (behavior.IsEffectInfoVersion2Supported() && params.effects > 0) {
        effect_result_states_dsp = allocator.Allocate<EffectResultState>(params.effects, 0x40);
        if (effect_result_states_dsp.empty()) {
            return Service::Audio::ResultInsufficientBuffer;
        }
        std::memset(effect_result_states_dsp.data(), 0, effect_result_states_dsp.size_bytes());
    }

    effect_context.Initialize(effect_infos, params.effects, effect_result_states_cpu,
                              effect_result_states_dsp, effect_result_states_dsp.size());

    auto sinks{allocator.Allocate<SinkInfoBase>(params.sinks, 0x10)};
    for (auto& sink : sinks) {
        std::construct_at<SinkInfoBase>(&sink);
    }

    if (sinks.empty()) {
        return Service::Audio::ResultInsufficientBuffer;
    }

    sink_context.Initialize(sinks, params.sinks);

    auto voice_dsp_states{allocator.Allocate<VoiceState>(params.voices, 0x40)};
    if (voice_dsp_states.empty()) {
        return Service::Audio::ResultInsufficientBuffer;
    }

    for (auto& voice_state : voice_dsp_states) {
        voice_state = {};
    }

    voice_context.Initialize(sorted_voice_infos, voice_infos, voice_channel_resources,
                             voice_cpu_states, voice_dsp_states, params.voices);

    if (params.perf_frames > 0) {
        const auto perf_workbuffer_size{
            PerformanceManager::GetRequiredBufferSizeForPerformanceMetricsPerFrame(behavior,
                                                                                   params) *
                (params.perf_frames + 1) +
            0xC};
        performance_workbuffer = allocator.Allocate<u8>(perf_workbuffer_size, 0x40);
        if (performance_workbuffer.empty()) {
            return Service::Audio::ResultInsufficientBuffer;
        }
        std::memset(performance_workbuffer.data(), 0, performance_workbuffer.size_bytes());
        performance_manager.Initialize(performance_workbuffer, performance_workbuffer.size_bytes(),
                                       params, behavior, memory_pool_info);
    }

    render_time_limit_percent = 100;
    drop_voice = params.voice_drop_enabled && params.execution_mode == ExecutionMode::Auto;
    drop_voice_param = 1.0f;
    num_voices_dropped = 0;

    allocator.Align(0x40);
    command_workbuffer_size = allocator.GetRemainingSize();
    command_workbuffer = allocator.Allocate<u8>(command_workbuffer_size, 0x40);
    if (command_workbuffer.empty()) {
        return Service::Audio::ResultInsufficientBuffer;
    }

    command_buffer_size = 0;
    reset_command_buffers = true;

    // nn::audio::dsp::FlushDataCache(transferMemory, transferMemorySize);

    if (behavior.IsCommandProcessingTimeEstimatorVersion5Supported()) {
        command_processing_time_estimator =
            std::make_unique<CommandProcessingTimeEstimatorVersion5>(sample_count,
                                                                     mix_buffer_count);
    } else if (behavior.IsCommandProcessingTimeEstimatorVersion4Supported()) {
        command_processing_time_estimator =
            std::make_unique<CommandProcessingTimeEstimatorVersion4>(sample_count,
                                                                     mix_buffer_count);
    } else if (behavior.IsCommandProcessingTimeEstimatorVersion3Supported()) {
        command_processing_time_estimator =
            std::make_unique<CommandProcessingTimeEstimatorVersion3>(sample_count,
                                                                     mix_buffer_count);
    } else if (behavior.IsCommandProcessingTimeEstimatorVersion2Supported()) {
        command_processing_time_estimator =
            std::make_unique<CommandProcessingTimeEstimatorVersion2>(sample_count,
                                                                     mix_buffer_count);
    } else {
        command_processing_time_estimator =
            std::make_unique<CommandProcessingTimeEstimatorVersion1>(sample_count,
                                                                     mix_buffer_count);
    }

    initialized = true;
    return ResultSuccess;
}

void System::Finalize() {
    if (!initialized) {
        return;
    }

    if (active) {
        Stop();
    }

    applet_resource_user_id = 0;

    PoolMapper pool_mapper(process_handle, false);
    pool_mapper.Unmap(memory_pool_info);

    if (process_handle) {
        pool_mapper.ClearUseState(memory_pool_workbuffer, memory_pool_count);
        for (auto& memory_pool : memory_pool_workbuffer) {
            if (memory_pool.IsMapped()) {
                pool_mapper.Unmap(memory_pool);
            }
        }

        // dsp::ProcessCleanup
        // close handle
    }
    initialized = false;
}

void System::Start() {
    std::scoped_lock l{lock};
    frames_elapsed = 0;
    state = State::Started;
    active = true;
}

void System::Stop() {
    {
        std::scoped_lock l{lock};
        state = State::Stopped;
        active = false;
    }

    if (execution_mode == ExecutionMode::Auto) {
        terminate_event.Wait();
    }
}

Result System::Update(std::span<const u8> input, std::span<u8> performance, std::span<u8> output) {
    std::scoped_lock l{lock};

    const auto start_time{core.CoreTiming().GetGlobalTimeNs().count()};
    std::memset(output.data(), 0, output.size());

    InfoUpdater info_updater(input, output, process_handle, behavior);

    auto result{info_updater.UpdateBehaviorInfo(behavior)};
    if (result.IsError()) {
        LOG_ERROR(Service_Audio, "Failed to update BehaviorInfo!");
        return result;
    }

    result = info_updater.UpdateMemoryPools(memory_pool_workbuffer, memory_pool_count);
    if (result.IsError()) {
        LOG_ERROR(Service_Audio, "Failed to update MemoryPools!");
        return result;
    }

    result = info_updater.UpdateVoiceChannelResources(voice_context);
    if (result.IsError()) {
        LOG_ERROR(Service_Audio, "Failed to update VoiceChannelResources!");
        return result;
    }

    result = info_updater.UpdateVoices(voice_context, memory_pool_workbuffer, memory_pool_count);
    if (result.IsError()) {
        LOG_ERROR(Service_Audio, "Failed to update Voices!");
        return result;
    }

    result = info_updater.UpdateEffects(effect_context, active, memory_pool_workbuffer,
                                        memory_pool_count);
    if (result.IsError()) {
        LOG_ERROR(Service_Audio, "Failed to update Effects!");
        return result;
    }

    if (behavior.IsSplitterSupported()) {
        result = info_updater.UpdateSplitterInfo(splitter_context);
        if (result.IsError()) {
            LOG_ERROR(Service_Audio, "Failed to update SplitterInfo!");
            return result;
        }
    }

    result =
        info_updater.UpdateMixes(mix_context, mix_buffer_count, effect_context, splitter_context);
    if (result.IsError()) {
        LOG_ERROR(Service_Audio, "Failed to update Mixes!");
        return result;
    }

    result = info_updater.UpdateSinks(sink_context, memory_pool_workbuffer, memory_pool_count);
    if (result.IsError()) {
        LOG_ERROR(Service_Audio, "Failed to update Sinks!");
        return result;
    }

    PerformanceManager* perf_manager{nullptr};
    if (performance_manager.IsInitialized()) {
        perf_manager = &performance_manager;
    }

    result =
        info_updater.UpdatePerformanceBuffer(performance, performance.size_bytes(), perf_manager);
    if (result.IsError()) {
        LOG_ERROR(Service_Audio, "Failed to update PerformanceBuffer!");
        return result;
    }

    result = info_updater.UpdateErrorInfo(behavior);
    if (result.IsError()) {
        LOG_ERROR(Service_Audio, "Failed to update ErrorInfo!");
        return result;
    }

    if (behavior.IsElapsedFrameCountSupported()) {
        result = info_updater.UpdateRendererInfo(frames_elapsed);
        if (result.IsError()) {
            LOG_ERROR(Service_Audio, "Failed to update RendererInfo!");
            return result;
        }
    }

    result = info_updater.CheckConsumedSize();
    if (result.IsError()) {
        LOG_ERROR(Service_Audio, "Invalid consume size!");
        return result;
    }

    adsp_rendered_event->Clear(core.Kernel());
    num_times_updated++;

    const auto end_time{core.CoreTiming().GetGlobalTimeNs().count()};
    ticks_spent_updating += end_time - start_time;

    return ResultSuccess;
}

namespace {

constexpr u32 PulseRendererVersion = 1;
constexpr s32 PulseNoPool = -1;
constexpr s32 PulseSystemPool = -2;

} // Anonymous namespace

void System::PulseSave(Service::Pulse::StateWriter& out) {
    static_assert(std::is_trivially_copyable_v<MemoryPoolInfo>);
    static_assert(std::is_trivially_copyable_v<VoiceInfo>);
    static_assert(std::is_trivially_copyable_v<VoiceState>);

    std::scoped_lock l{lock};
    if (!initialized) throw std::logic_error("Pulse: audio renderer not initialized");

    // Memory pools are stored by index: their host pointers differ in the restored process.
    const auto pool_index = [&](const AddressInfo& address) -> s32 {
        const auto* pool = address.GetPool();
        if (!pool) return PulseNoPool;
        if (pool == &memory_pool_info) return PulseSystemPool;
        if (pool >= memory_pool_workbuffer.data() &&
            pool < memory_pool_workbuffer.data() + memory_pool_workbuffer.size()) {
            return static_cast<s32>(pool - memory_pool_workbuffer.data());
        }
        throw std::logic_error("Pulse: audio address outside the memory pools");
    };

    out.U32(PulseRendererVersion);
    out.Bool(active);
    out.U64(frames_elapsed);
    out.U32(render_time_limit_percent);
    out.Bool(drop_voice);
    out.Raw(drop_voice_param);

    out.U32(memory_pool_count);
    for (u32 i = 0; i < memory_pool_count; i++) out.Raw(memory_pool_workbuffer[i]);

    // Voices in use, with their wavebuffer addresses' pools; then every channel resource they or
    // the game hold, with the decode state the renderer keeps per resource.
    const auto voice_count = voice_context.GetCount();
    std::vector<bool> resources(voice_count);
    std::vector<u32> voices;
    for (u32 i = 0; i < voice_count; i++) {
        const auto& voice = voice_context.GetInfo(i);
        if (voice.in_use) voices.push_back(i);
        if (voice_context.GetChannelResource(i).in_use) resources[i] = true;
        if (!voice.in_use) continue;
        for (s8 c = 0; c < voice.channel_count && c < MaxChannels; c++) {
            if (voice.channel_resource_ids[c] < voice_count) {
                resources[voice.channel_resource_ids[c]] = true;
            }
        }
    }
    out.U32(voice_count);
    out.U32(static_cast<u32>(voices.size()));
    for (const auto i : voices) {
        const auto& voice = voice_context.GetInfo(i);
        out.U32(i);
        out.Raw(voice);
        out.S32(pool_index(voice.data_address));
        for (const auto& wavebuffer : voice.wavebuffers) {
            out.S32(pool_index(wavebuffer.buffer_address));
            out.S32(pool_index(wavebuffer.context_address));
        }
    }
    out.U32(static_cast<u32>(std::count(resources.begin(), resources.end(), true)));
    for (u32 i = 0; i < voice_count; i++) {
        if (!resources[i]) continue;
        const auto& resource = voice_context.GetChannelResource(i);
        out.U32(i);
        out.Bool(resource.in_use);
        out.Raw(resource.mix_volumes);
        out.Raw(resource.prev_mix_volumes);
        out.Raw(voice_context.GetState(i));
        out.Raw(voice_context.GetDspSharedState(i));
    }

    // Effects: the type alone. The game's next update resends every effect and, finding the
    // buffers unmapped, attaches them again and starts a fresh effect state.
    const auto effect_count = effect_context.GetCount();
    out.U32(effect_count);
    for (u32 i = 0; i < effect_count; i++) {
        out.U8(static_cast<u8>(effect_context.GetInfo(i).GetType()));
    }

    // Mixes may be sent dirty-only, so the renderer's copy is the only one.
    const auto mix_count = mix_context.GetCount();
    out.U32(static_cast<u32>(mix_count));
    for (s32 i = 0; i < mix_count; i++) {
        const auto& mix = *mix_context.GetInfo(i);
        out.Raw(mix.volume);
        out.U32(mix.sample_rate);
        out.U32(static_cast<u16>(mix.buffer_count));
        out.Bool(mix.in_use);
        out.Bool(mix.enabled);
        out.S32(mix.mix_id);
        out.S32(mix.node_id);
        out.U32(static_cast<u16>(mix.buffer_offset));
        out.S32(mix.distance_from_final_mix);
        out.S32(mix.dst_mix_id);
        out.Raw(mix.mix_volumes);
        out.S32(mix.dst_splitter_id);
        out.U32(static_cast<u32>(mix.effect_order_buffer.size()));
        for (const auto order : mix.effect_order_buffer) out.S32(order);
    }
    auto& edges = mix_context.GetEdgeMatrix();
    const auto nodes = edges.GetNodeCount();
    out.U32(nodes);
    for (u32 from = 0; from < nodes; from++) {
        for (u32 to = 0; to < nodes; to++) out.Bool(edges.Connected(from, to));
    }

    // Splitters, with their destination chains as indices.
    const auto info_count = splitter_context.GetInfoCount();
    const auto data_count = splitter_context.GetDataCount();
    const auto data_index = [&](const SplitterDestinationData* data) -> s32 {
        if (!data) return -1;
        const auto* base = &splitter_context.GetData(0);
        if (data < base || data >= base + data_count) {
            throw std::logic_error("Pulse: splitter destination outside its table");
        }
        return static_cast<s32>(data - base);
    };
    out.U32(info_count);
    for (u32 i = 0; i < info_count; i++) {
        const auto& info = splitter_context.GetInfo(static_cast<s32>(i));
        out.U32(info.sample_rate);
        out.U32(info.destination_count);
        out.Bool(info.has_new_connection);
        out.S32(data_count ? data_index(info.destinations) : -1);
        out.U32(info.channel_count);
    }
    out.U32(data_count);
    for (u32 i = 0; i < data_count; i++) {
        const auto& data = splitter_context.GetData(i);
        out.S32(data.destination_id);
        out.Raw(data.mix_volumes);
        out.Raw(data.prev_mix_volumes);
        out.Raw(data.biquad_filters);
        out.S32(data_index(data.next));
        out.Bool(data.in_use);
        out.Bool(data.need_update);
    }
}

void System::PulseLoad(Service::Pulse::StateReader& in) {
    std::scoped_lock l{lock};
    if (!initialized) throw std::logic_error("Pulse: audio renderer not initialized");
    if (in.U32() != PulseRendererVersion) {
        throw std::invalid_argument("Pulse: audio renderer state version");
    }
    const auto expect = [](bool fits, const char* what) {
        if (!fits) throw std::invalid_argument(std::string{"Pulse: audio renderer "} + what);
    };
    const auto pool_at = [&](s32 index) -> MemoryPoolInfo* {
        if (index == PulseNoPool) return nullptr;
        if (index == PulseSystemPool) return &memory_pool_info;
        expect(index >= 0 && static_cast<u32>(index) < memory_pool_count, "memory pool index");
        return &memory_pool_workbuffer[index];
    };

    const bool was_active = in.Bool();
    const u64 frames = in.U64();
    render_time_limit_percent = in.U32();
    drop_voice = in.Bool();
    drop_voice_param = in.Raw<f32>();

    expect(in.U32() == memory_pool_count, "memory pool count");
    for (u32 i = 0; i < memory_pool_count; i++) {
        memory_pool_workbuffer[i] = in.Raw<MemoryPoolInfo>();
    }

    const auto voice_count = voice_context.GetCount();
    expect(in.U32() == voice_count, "voice count");
    const auto voices = in.Count(sizeof(VoiceInfo));
    for (u32 v = 0; v < voices; v++) {
        const auto i = in.U32();
        expect(i < voice_count, "voice index");
        auto& voice = voice_context.GetInfo(i);
        voice = in.Raw<VoiceInfo>();
        voice.data_address.SetPool(pool_at(in.S32()));
        for (auto& wavebuffer : voice.wavebuffers) {
            wavebuffer.buffer_address.SetPool(pool_at(in.S32()));
            wavebuffer.context_address.SetPool(pool_at(in.S32()));
        }
    }
    const auto resources = in.Count(sizeof(VoiceState));
    for (u32 r = 0; r < resources; r++) {
        const auto i = in.U32();
        expect(i < voice_count, "channel resource index");
        auto& resource = voice_context.GetChannelResource(i);
        resource.in_use = in.Bool();
        resource.mix_volumes = in.Raw<decltype(resource.mix_volumes)>();
        resource.prev_mix_volumes = in.Raw<decltype(resource.prev_mix_volumes)>();
        voice_context.GetState(i) = in.Raw<VoiceState>();
        voice_context.GetDspSharedState(i) = in.Raw<VoiceState>();
    }

    const auto effect_count = effect_context.GetCount();
    expect(in.U32() == effect_count, "effect count");
    for (u32 i = 0; i < effect_count; i++) {
        const auto type = static_cast<EffectInfoBase::Type>(in.U8());
        expect(type <= EffectInfoBase::Type::Compressor, "effect type");
        auto& effect = effect_context.GetInfo(i);
        ResetEffect(&effect, type);
        if (type != EffectInfoBase::Type::Invalid) effect.PulseMarkUnmapped();
    }

    const auto mix_count = mix_context.GetCount();
    expect(in.U32() == static_cast<u32>(mix_count), "mix count");
    for (s32 i = 0; i < mix_count; i++) {
        auto& mix = *mix_context.GetInfo(i);
        mix.volume = in.Raw<f32>();
        mix.sample_rate = in.U32();
        mix.buffer_count = static_cast<s16>(in.U32());
        mix.in_use = in.Bool();
        mix.enabled = in.Bool();
        mix.mix_id = in.S32();
        mix.node_id = in.S32();
        mix.buffer_offset = static_cast<s16>(in.U32());
        mix.distance_from_final_mix = in.S32();
        mix.dst_mix_id = in.S32();
        mix.mix_volumes = in.Raw<decltype(mix.mix_volumes)>();
        mix.dst_splitter_id = in.S32();
        expect(in.U32() == mix.effect_order_buffer.size(), "mix effect order size");
        for (auto& order : mix.effect_order_buffer) order = in.S32();
    }
    auto& edges = mix_context.GetEdgeMatrix();
    const auto nodes = edges.GetNodeCount();
    expect(in.U32() == nodes, "mix graph size");
    for (u32 from = 0; from < nodes; from++) {
        for (u32 to = 0; to < nodes; to++) {
            if (in.Bool()) {
                edges.Connect(from, to);
            } else {
                edges.Disconnect(from, to);
            }
        }
    }

    const auto info_count = splitter_context.GetInfoCount();
    const auto data_count = splitter_context.GetDataCount();
    const auto data_at = [&](s32 index) -> SplitterDestinationData* {
        if (index < 0) return nullptr;
        expect(static_cast<u32>(index) < data_count, "splitter destination index");
        return &splitter_context.GetData(static_cast<u32>(index));
    };
    expect(in.U32() == info_count, "splitter count");
    for (u32 i = 0; i < info_count; i++) {
        auto& info = splitter_context.GetInfo(static_cast<s32>(i));
        info.sample_rate = in.U32();
        info.destination_count = in.U32();
        info.has_new_connection = in.Bool();
        info.destinations = data_at(in.S32());
        info.channel_count = in.U32();
    }
    expect(in.U32() == data_count, "splitter destination count");
    for (u32 i = 0; i < data_count; i++) {
        auto& data = splitter_context.GetData(i);
        data.destination_id = in.S32();
        data.mix_volumes = in.Raw<decltype(data.mix_volumes)>();
        data.prev_mix_volumes = in.Raw<decltype(data.prev_mix_volumes)>();
        data.biquad_filters = in.Raw<decltype(data.biquad_filters)>();
        data.next = data_at(in.S32());
        data.in_use = in.Bool();
        data.need_update = in.Bool();
    }

    // The order the renderer walks the mixes in is derived; UpdateMixes rebuilds it the same way.
    if (behavior.IsSplitterSupported() && splitter_context.UsingSplitter()) {
        expect(mix_context.TSortInfo(splitter_context), "mix graph");
    } else {
        mix_context.SortInfo();
    }

    frames_elapsed = frames;
    if (was_active) {
        state = State::Started;
        active = true;
    }
}

u32 System::GetRenderingTimeLimit() const {
    return render_time_limit_percent;
}

void System::SetRenderingTimeLimit(u32 limit) {
    render_time_limit_percent = limit;
}

u32 System::GetSessionId() const {
    return session_id;
}

u32 System::GetSampleRate() const {
    return sample_rate;
}

u32 System::GetSampleCount() const {
    return sample_count;
}

u32 System::GetMixBufferCount() const {
    return mix_buffer_count;
}

ExecutionMode System::GetExecutionMode() const {
    return execution_mode;
}

u32 System::GetRenderingDevice() const {
    return render_device;
}

bool System::IsActive() const {
    return active;
}

void System::SendCommandToDsp() {
    std::scoped_lock l{lock};

    if (initialized) {
        if (active) {
            terminate_event.Reset();
            const auto remaining_command_count{audio_renderer.GetRemainCommandCount(session_id)};
            u64 command_size{0};

            if (remaining_command_count) {
                adsp_behind = true;
                command_size = command_buffer_size;
            } else {
                command_size = GenerateCommand(command_workbuffer, command_workbuffer_size);
            }

            auto translated_addr{
                memory_pool_info.Translate(CpuAddr(command_workbuffer.data()), command_size)};

            auto time_limit_percent{70.0f};
            if (behavior.IsAudioRendererProcessingTimeLimit80PercentSupported()) {
                time_limit_percent = 80.0f;
            } else if (behavior.IsAudioRendererProcessingTimeLimit75PercentSupported()) {
                time_limit_percent = 75.0f;
            } else {
                // result ignored and 70 is used anyway
                behavior.IsAudioRendererProcessingTimeLimit70PercentSupported();
                time_limit_percent = 70.0f;
            }

            auto time_limit{
                static_cast<u64>((time_limit_percent / 100) * 2'880'000.0 *
                                 (static_cast<f32>(render_time_limit_percent) / 100.0f))};
            audio_renderer.SetCommandBuffer(session_id, translated_addr, command_size, time_limit,
                                            applet_resource_user_id, process_handle,
                                            reset_command_buffers);
            reset_command_buffers = false;
            command_buffer_size = command_size;
            if (remaining_command_count == 0) {
                adsp_rendered_event->Signal(core.Kernel());
            }
        } else {
            audio_renderer.ClearRemainCommandCount(session_id);
            terminate_event.Set();
        }
    }
}

u64 System::GenerateCommand(std::span<u8> in_command_buffer,
                            [[maybe_unused]] u64 command_buffer_size_) {
    PoolMapper::ClearUseState(memory_pool_workbuffer, memory_pool_count);
    const auto start_time{core.CoreTiming().GetGlobalTimeNs().count()};

    auto command_list_header{reinterpret_cast<CommandListHeader*>(in_command_buffer.data())};

    command_list_header->buffer_count = static_cast<s16>(voice_channels + mix_buffer_count);
    command_list_header->sample_count = sample_count;
    command_list_header->sample_rate = sample_rate;
    command_list_header->samples_buffer = samples_workbuffer;

    const auto performance_initialized{performance_manager.IsInitialized()};
    if (performance_initialized) {
        performance_manager.TapFrame(adsp_behind, num_voices_dropped, render_start_tick);
        adsp_behind = false;
        num_voices_dropped = 0;
        render_start_tick = 0;
    }

    s8 channel_count{2};
    if (execution_mode == ExecutionMode::Auto) {
        const auto& sink{core.AudioCore().GetOutputSink()};
        channel_count = static_cast<s8>(sink.GetDeviceChannels());
    }

    AudioRendererSystemContext render_context{
        .session_id{session_id},
        .channels{channel_count},
        .mix_buffer_count{mix_buffer_count},
        .behavior{&behavior},
        .depop_buffer{depop_buffer},
        .upsampler_manager{upsampler_manager},
        .memory_pool_info{&memory_pool_info},
    };

    CommandBuffer command_buffer{
        .command_list{in_command_buffer},
        .sample_count{sample_count},
        .sample_rate{sample_rate},
        .size{sizeof(CommandListHeader)},
        .count{0},
        .estimated_process_time{0},
        .memory_pool{&memory_pool_info},
        .time_estimator{command_processing_time_estimator.get()},
        .behavior{&behavior},
    };

    PerformanceManager* perf_manager{nullptr};
    if (performance_initialized) {
        perf_manager = &performance_manager;
    }

    CommandGenerator command_generator{command_buffer, *command_list_header, render_context,
                                       voice_context,  mix_context,          effect_context,
                                       sink_context,   splitter_context,     perf_manager};

    voice_context.SortInfo();
    command_generator.GenerateVoiceCommands();

    const auto start_estimated_time{drop_voice_param *
                                    static_cast<f32>(command_buffer.estimated_process_time)};

    command_generator.GenerateSubMixCommands();
    command_generator.GenerateFinalMixCommands();
    command_generator.GenerateSinkCommands();

    if (drop_voice) {
        f32 time_limit_percent{70.0f};
        if (render_context.behavior->IsAudioRendererProcessingTimeLimit80PercentSupported()) {
            time_limit_percent = 80.0f;
        } else if (render_context.behavior
                       ->IsAudioRendererProcessingTimeLimit75PercentSupported()) {
            time_limit_percent = 75.0f;
        } else {
            // result is ignored
            render_context.behavior->IsAudioRendererProcessingTimeLimit70PercentSupported();
            time_limit_percent = 70.0f;
        }

        const auto end_estimated_time{drop_voice_param *
                                      static_cast<f32>(command_buffer.estimated_process_time)};

        const auto dsp_time_limit{((time_limit_percent / 100.0f) * 2'880'000.0f) *
                                  (static_cast<f32>(render_time_limit_percent) / 100.0f)};

        const auto estimated_time{start_estimated_time - end_estimated_time};

        const auto time_limit{static_cast<u32>((std::max)(dsp_time_limit + estimated_time, 0.0f))};
        num_voices_dropped =
            DropVoices(command_buffer, static_cast<u32>(start_estimated_time), time_limit);
    }

    command_list_header->buffer_size = command_buffer.size;
    command_list_header->command_count = command_buffer.count;

    voice_context.UpdateStateByDspShared();

    if (render_context.behavior->IsEffectInfoVersion2Supported()) {
        effect_context.UpdateStateByDspShared();
    }

    const auto end_time{core.CoreTiming().GetGlobalTimeNs().count()};
    total_ticks_elapsed += end_time - start_time;
    num_command_lists_generated++;
    render_start_tick = audio_renderer.GetRenderingStartTick(session_id);
    frames_elapsed++;

    return command_buffer.size;
}

f32 System::GetVoiceDropParameter() const {
    return drop_voice_param;
}

void System::SetVoiceDropParameter(f32 voice_drop_) {
    drop_voice_param = voice_drop_;
}

u32 System::DropVoices(CommandBuffer& command_buffer, u32 estimated_process_time, u32 time_limit) {
    u32 i{0};
    auto command_list{command_buffer.command_list.data() + sizeof(CommandListHeader)};
    ICommand* cmd{nullptr};

    // Find a first valid voice to drop
    while (i < command_buffer.count) {
        cmd = reinterpret_cast<ICommand*>(command_list);
        if (cmd->type == CommandId::Performance ||
            cmd->type == CommandId::DataSourcePcmInt16Version1 ||
            cmd->type == CommandId::DataSourcePcmInt16Version2 ||
            cmd->type == CommandId::DataSourcePcmFloatVersion1 ||
            cmd->type == CommandId::DataSourcePcmFloatVersion2 ||
            cmd->type == CommandId::DataSourceAdpcmVersion1 ||
            cmd->type == CommandId::DataSourceAdpcmVersion2) {
            break;
        }
        command_list += cmd->size;
        i++;
    }

    if (cmd == nullptr || command_buffer.count == 0 || i >= command_buffer.count) {
        return 0;
    }

    auto voices_dropped{0};
    while (i < command_buffer.count) {
        const auto node_id{cmd->node_id};
        const auto node_id_type{cmd->node_id >> 28};
        const auto node_id_base{(cmd->node_id >> 16) & 0xFFF};

        // If the new estimated process time falls below the limit, we're done dropping.
        if (estimated_process_time <= time_limit) {
            break;
        }

        if (node_id_type != 1) {
            break;
        }

        // Don't drop voices marked with the highest priority.
        auto& voice_info{voice_context.GetInfo(node_id_base)};
        if (voice_info.priority == HighestVoicePriority) {
            break;
        }

        voices_dropped++;
        voice_info.voice_dropped = true;

        // First iteration should drop the voice, and then iterate through all of the commands tied
        // to the voice. We don't need reverb on a voice which we've just removed, for example.
        // Depops can't be removed otherwise we'll introduce audio popping, and we don't
        // remove perf commands. Lower the estimated time for each command dropped.
        while (i < command_buffer.count && cmd->node_id == node_id) {
            if (cmd->type == CommandId::DepopPrepare) {
                cmd->enabled = true;
            } else if (cmd->enabled && cmd->type != CommandId::Performance) {
                cmd->enabled = false;
                estimated_process_time -= static_cast<u32>(
                    drop_voice_param * static_cast<f32>(cmd->estimated_process_time));
            }
            command_list += cmd->size;
            cmd = reinterpret_cast<ICommand*>(command_list);
            i++;
        }
        i++;
    }
    return voices_dropped;
}

} // namespace AudioCore::Renderer
