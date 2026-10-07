// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>

#include "common/bug_tracker.h"
#include "common/logging.h"
#include "common/scope_exit.h"
#include "video_core/gpu.h"
#include "video_core/memory_manager.h"
#include "video_core/perf_counters.h"
#include "video_core/renderer_d3d12/d3d12_log.h"
#include "video_core/renderer_d3d12/d3d12_rasterizer.h"
#include "video_core/renderer_d3d12/d3d12_scheduler.h"

// Indirect draws: ExecuteIndirect records built on the GPU, or read on the CPU for the
// topologies D3D12 cannot draw directly.

namespace D3D12 {

void RasterizerD3D12::DrawIndirect() {
    ++draw_counter;
    const IndirectParams& params = maxwell3d->draw_manager.indirect_state;
    if (params.is_byte_count) {
        // Transform feedback draws; HasDrawTransformFeedback() is false, so the macro should
        // have drawn them directly.
        BUG_TRACK(DrawSkipped, "byte-count (transform feedback) indirect draw skipped");
        WarnOnceLog(logged_byte_count_draw, "byte-count indirect draws are skipped");
        return;
    }
    if (params.max_draw_counts == 0 || CullsEveryPrimitive()) {
        return;
    }
    if (BufferCacheRuntime::IsEmulatedTopology(maxwell3d->draw_manager.draw_state.topology)) {
        DrawIndirectOnCpu(params);
        return;
    }
    VideoCore::Perf::ScopedNsTimer draw_timer{VideoCore::Perf::Counter::DrawNs};
    SCOPE_EXIT {
        FlushIfUploadHeavy();
        gpu.TickWork();
    };
    gpu_memory->FlushCaching();
    ApplyPendingStateInvalidation();

    GraphicsPipeline* const pipeline = pipeline_cache.CurrentGraphicsPipeline();
    if (!pipeline) {
        return;
    }
    std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
    // With the indirect state set, Configure also synchronizes the argument and count buffers.
    PipelineBindings bindings;
    buffer_cache.SetDrawIndirect(&params);
    pipeline->Configure(params.is_indexed,
                        {*maxwell3d, *gpu_memory, buffer_cache, texture_cache, descriptor_queue,
                         sampler_heap, scheduler},
                        bindings);
    buffer_cache.SetDrawIndirect(nullptr);
    if (!pipeline->Handle()) {
        return; // D3D12 rejected the PSO (logged when it was built)
    }
    ID3D12CommandSignature* const signature = pipeline->Layout().DrawSignature(params.is_indexed);
    Buffer* const buffer = buffer_cache.GetDrawIndirectBuffer().first;
    const u32 offset = buffer_cache.GetDrawIndirectBuffer().second;
    if (!signature || !buffer->Handle()) {
        buffer_runtime.ApplyGeometry(scheduler.CommandList());
        return;
    }
    const Framebuffer* const framebuffer = texture_cache.GetFramebuffer();
    framebuffer->PrepareAttachments(bindings.depth_sampled);

    // Records of INDIRECT_DRAW(_INDEXED)_WORDS: the CPU writes is_indexed_draw, the y/z flips and
    // the draw index; GPU copies add the guest's arguments and, as the first two runtime-data
    // words, its first vertex (base vertex when indexed) and base instance, which are the last
    // two words of both guest layouts. The count, when there is one, goes after the records.
    const u32 draw_count = static_cast<u32>(params.max_draw_counts);
    const u32 guest_words = params.is_indexed ? 5 : 4;
    const u32 record_words = INDIRECT_DRAW_CONSTANT_WORDS + guest_words;
    const u64 record_size = record_words * sizeof(u32);
    const u64 guest_stride = params.stride != 0 ? params.stride : guest_words * sizeof(u32);
    std::array<D3D12_VIEWPORT, Maxwell::NumViewports> viewports{};
    const ViewportState viewport_state = ComputeViewports(viewports);
    indirect_words.assign(static_cast<size_t>(draw_count) * record_words + 1, 0);
    for (u32 draw = 0; draw < draw_count; ++draw) {
        u32* const record = indirect_words.data() + static_cast<size_t>(draw) * record_words;
        record[2] = params.is_indexed ? 1 : 0;
        record[3] = viewport_state.yz_flip_mask;
        record[4] = draw;
    }
    const IndirectArgumentRing::Range range = indirect_args.Upload(indirect_words);
    ID3D12GraphicsCommandList* const cmd = scheduler.CommandList();
    buffer->Transition(D3D12_RESOURCE_STATE_GENERIC_READ);
    for (u32 draw = 0; draw < draw_count; ++draw) {
        const u64 src = offset + draw * guest_stride;
        const u64 dst = range.offset + draw * record_size;
        cmd->CopyBufferRegion(range.buffer, dst, buffer->Handle(),
                              src + (guest_words - 2) * sizeof(u32), 2 * sizeof(u32));
        cmd->CopyBufferRegion(range.buffer, dst + INDIRECT_DRAW_CONSTANT_WORDS * sizeof(u32),
                              buffer->Handle(), src, guest_words * sizeof(u32));
    }
    const u64 count_offset = range.offset + draw_count * record_size;
    if (params.include_count) {
        const auto [count_buffer, count_src] = buffer_cache.GetDrawIndirectCount();
        if (count_buffer->Handle()) {
            count_buffer->Transition(D3D12_RESOURCE_STATE_GENERIC_READ);
            cmd->CopyBufferRegion(range.buffer, count_offset, count_buffer->Handle(), count_src,
                                  sizeof(u32));
        }
    }
    indirect_args.Ready(range);

    const DrawParams draw_params{
        .num_vertices = 0,
        .num_instances = 0,
        .first_index = 0,
        .base_vertex = 0,
        .base_instance = 0,
        .is_indexed = params.is_indexed,
        .runtime_first_vertex = 0,
    };
    BindDrawState(*pipeline, bindings, *framebuffer, draw_params,
                  maxwell3d->draw_manager.draw_state.topology);
    scheduler.CommandList()->ExecuteIndirect(signature, draw_count, range.buffer, range.offset,
                                             params.include_count ? range.buffer : nullptr,
                                             params.include_count ? count_offset : 0);
    // The command signature wrote the runtime data constants: the next draw sets them again.
    command_state.root_args.valid = false;
    if (!logged_indirect_draw) {
        LOG_INFO(Render, "D3D12: first indirect draw recorded ({}, up to {} draws{})",
                 params.is_indexed ? "indexed" : "non-indexed", draw_count,
                 params.include_count ? ", GPU count" : "");
        logged_indirect_draw = true;
    }
}

void RasterizerD3D12::DrawIndirectOnCpu(const IndirectParams& params) {
    // The topologies D3D12 lacks are rebuilt from guest memory on the CPU, so these draws read
    // their arguments there too. ReadBlock flushes what the GPU wrote first, which can stall.
    if (!logged_cpu_indirect_draw) {
        LOG_INFO(Render, "D3D12: indirect draws of emulated topologies read their arguments on "
                         "the CPU");
        logged_cpu_indirect_draw = true;
    }
    const u32 guest_words = params.is_indexed ? 5 : 4;
    const u64 guest_stride = params.stride != 0 ? params.stride : guest_words * sizeof(u32);
    u64 draw_count = params.max_draw_counts;
    if (params.include_count) {
        u32 count = 0;
        gpu_memory->ReadBlock(params.count_start_address, &count, sizeof(count));
        draw_count = std::min<u64>(draw_count, count);
    }
    auto& draw_state = maxwell3d->draw_manager.draw_state;
    const auto saved_vertex_buffer = draw_state.vertex_buffer;
    const auto saved_index_buffer = draw_state.index_buffer;
    const auto saved_base_index = draw_state.base_index;
    const auto saved_base_instance = draw_state.base_instance;
    for (u64 draw = 0; draw < draw_count; ++draw) {
        std::array<u32, 5> args{};
        gpu_memory->ReadBlock(params.indirect_start_address + draw * guest_stride, args.data(),
                              guest_words * sizeof(u32));
        if (params.is_indexed) {
            draw_state.index_buffer.count = args[0];
            draw_state.index_buffer.first = args[2];
            draw_state.base_index = args[3];
            draw_state.base_instance = args[4];
        } else {
            draw_state.vertex_buffer.count = args[0];
            draw_state.vertex_buffer.first = args[2];
            draw_state.base_instance = args[3];
        }
        Draw(params.is_indexed, args[1]);
    }
    draw_state.vertex_buffer = saved_vertex_buffer;
    draw_state.index_buffer = saved_index_buffer;
    draw_state.base_index = saved_base_index;
    draw_state.base_instance = saved_base_instance;
}

} // namespace D3D12
