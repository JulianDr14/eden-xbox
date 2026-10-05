// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cmath>
#include <bit>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>

#include <spirv_to_dxil.h>

#include "common/alignment.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "video_core/control/channel_state.h"
#include "video_core/dirty_flags.h"
#include "video_core/engines/kepler_compute.h"
#include "video_core/gpu.h"
#include "video_core/memory_manager.h"
#include "video_core/framebuffer_config.h"
#include "video_core/renderer_d3d12/d3d12_maxwell_to_d3d12.h"
#include "video_core/perf_counters.h"
#include "video_core/renderer_d3d12/d3d12_rasterizer.h"
#include "video_core/renderer_d3d12/d3d12_scheduler.h"
#include "video_core/surface.h"
#include "video_core/texture_cache/util.h"

namespace D3D12 {

namespace {

/// Guest query types the D3D12 query cache counts on the host (see d3d12_query_cache.cpp); the
/// rest are written by QueryFallback. Same mapping as the OpenGL backend.
std::optional<VideoCore::QueryType> MaxwellToVideoCoreQuery(VideoCommon::QueryType type) {
    switch (type) {
    case VideoCommon::QueryType::PrimitivesGenerated:
    case VideoCommon::QueryType::VtgPrimitivesOut:
        return VideoCore::QueryType::PrimitivesGenerated;
    case VideoCommon::QueryType::ZPassPixelCount64:
        return VideoCore::QueryType::SamplesPassed;
    case VideoCommon::QueryType::StreamingPrimitivesSucceeded:
        return VideoCore::QueryType::TfbPrimitivesWritten;
    default:
        return std::nullopt;
    }
}

/// Small unsigned float of the packed formats (R11G11B10: 5-bit exponent, 6 or 5 mantissa bits).
float UnpackSmallFloat(u32 bits, u32 mantissa_bits) {
    const u32 exponent = bits >> mantissa_bits;
    const u32 mantissa = bits & ((1U << mantissa_bits) - 1);
    const float scale = static_cast<float>(1U << mantissa_bits);
    if (exponent == 0) {
        return std::ldexp(static_cast<float>(mantissa) / scale, -14);
    }
    if (exponent == 31) {
        return mantissa != 0 ? std::numeric_limits<float>::quiet_NaN()
                             : std::numeric_limits<float>::infinity();
    }
    return std::ldexp(1.0f + static_cast<float>(mantissa) / scale, static_cast<int>(exponent) - 15);
}

float UnpackHalf(u16 bits) {
    const float magnitude = UnpackSmallFloat(bits & 0x7FFF, 10);
    return (bits & 0x8000) != 0 ? -magnitude : magnitude;
}

/// One texel of a render target dump as RGBA floats; false for formats the dump cannot read.
u32 DumpTexelBytes(DXGI_FORMAT format);

bool ReadTexel(DXGI_FORMAT format, const u8* src, std::array<float, 4>& rgba) {
    // Only this texel's bytes: the last texel of a 1- or 2-byte format ends the readback.
    u32 word = 0;
    std::memcpy(&word, src, std::min<u32>(DumpTexelBytes(format), 4));
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        for (size_t i = 0; i < 4; ++i) {
            rgba[i] = static_cast<float>(src[i]) / 255.0f;
        }
        return true;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        rgba = {src[2] / 255.0f, src[1] / 255.0f, src[0] / 255.0f, src[3] / 255.0f};
        return true;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        rgba = {(word & 0x3FF) / 1023.0f, ((word >> 10) & 0x3FF) / 1023.0f,
                ((word >> 20) & 0x3FF) / 1023.0f, (word >> 30) / 3.0f};
        return true;
    case DXGI_FORMAT_R11G11B10_FLOAT:
        rgba = {UnpackSmallFloat(word & 0x7FF, 6), UnpackSmallFloat((word >> 11) & 0x7FF, 6),
                UnpackSmallFloat(word >> 22, 5), 1.0f};
        return true;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        for (size_t i = 0; i < 4; ++i) {
            u16 half;
            std::memcpy(&half, src + i * 2, 2);
            rgba[i] = UnpackHalf(half);
        }
        return true;
    case DXGI_FORMAT_R16G16_TYPELESS:
    case DXGI_FORMAT_R16G16_FLOAT:
        rgba = {UnpackHalf(static_cast<u16>(word)), UnpackHalf(static_cast<u16>(word >> 16)), 0.0f,
                1.0f};
        return true;
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_R16_FLOAT: {
        const float value = UnpackHalf(static_cast<u16>(word));
        rgba = {value, value, value, 1.0f};
        return true;
    }
    case DXGI_FORMAT_R16_UNORM: {
        const float value = static_cast<float>(word & 0xFFFF) / 65535.0f;
        rgba = {value, value, value, 1.0f};
        return true;
    }
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_R32_FLOAT: {
        const float value = std::bit_cast<float>(word);
        rgba = {value, value, value, 1.0f};
        return true;
    }
    case DXGI_FORMAT_R24G8_TYPELESS: {
        // Depth plane of D24S8: depth in the low 24 bits.
        const float value = static_cast<float>(word & 0xFFFFFF) / 16777215.0f;
        rgba = {value, value, value, 1.0f};
        return true;
    }
    case DXGI_FORMAT_R8_TYPELESS:
    case DXGI_FORMAT_R8_UNORM:
        rgba = {src[0] / 255.0f, src[0] / 255.0f, src[0] / 255.0f, 1.0f};
        return true;
    case DXGI_FORMAT_R8G8_TYPELESS:
    case DXGI_FORMAT_R8G8_UNORM:
        rgba = {src[0] / 255.0f, src[1] / 255.0f, 0.0f, 1.0f};
        return true;
    default:
        return false;
    }
}

u32 DumpTexelBytes(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return 8;
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_R16_FLOAT:
    case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_R8G8_TYPELESS:
    case DXGI_FORMAT_R8G8_UNORM:
        return 2;
    case DXGI_FORMAT_R8_TYPELESS:
    case DXGI_FORMAT_R8_UNORM:
        return 1;
    default:
        return 4;
    }
}
} // Anonymous namespace

RasterizerD3D12::RasterizerD3D12(Tegra::GPU& gpu_,
                                 Tegra::MaxwellDeviceMemoryManager& device_memory_,
                                 const Device& device, Scheduler& scheduler_,
                                 const ShaderCompiler& compiler,
                                 BufferCacheRuntime& buffer_runtime_,
                                 TextureCacheRuntime& texture_runtime,
                                 DescriptorRing& descriptor_ring_, SamplerHeap& sampler_heap_,
                                 BlitImageHelper& blit_helper_, StagingBufferPool& staging)
    : gpu{gpu_}, device_memory{device_memory_}, scheduler{scheduler_}, staging{staging},
      buffer_runtime{buffer_runtime_}, texture_runtime{texture_runtime}, descriptor_ring{descriptor_ring_},
      sampler_heap{sampler_heap_}, blit_helper{blit_helper_},
      indirect_args{device, scheduler_, staging},
      descriptor_queue{device.Get(), descriptor_ring_},
      buffer_cache{device_memory_, buffer_runtime_}, texture_cache{texture_runtime, device_memory_},
      pipeline_cache{device_memory_, device, compiler, texture_runtime, gpu_.ShaderNotify()},
      query_cache{*this, device_memory_, device, scheduler_},
      accelerate_dma{buffer_cache, texture_cache},
      fence_manager{*this, gpu_, texture_cache, buffer_cache, query_cache, scheduler_} {
    buffer_runtime.SetDescriptorQueue(&descriptor_queue);
    scheduler.RegisterOnReset([this] { InvalidateCommandListState(); });
    LOG_INFO(Render, "D3D12: rasterizer active (draws, clears, caches, DMA and pipelines)");
}

RasterizerD3D12::~RasterizerD3D12() {
    buffer_runtime.SetDescriptorQueue(nullptr);
}

void RasterizerD3D12::Draw(bool is_indexed, u32 instance_count) {
    ++draw_counter;
    VideoCore::Perf::ScopedNsTimer draw_timer{VideoCore::Perf::Counter::DrawNs};
    SCOPE_EXIT {
        FlushIfUploadHeavy();
        gpu.TickWork();
    };
    gpu_memory->FlushCaching();
    ApplyPendingStateInvalidation();

    GraphicsPipeline* const pipeline = pipeline_cache.CurrentGraphicsPipeline();
    if (!pipeline) {
        if (trace_draws) {
            TraceDraw("draw skipped (no pipeline)", nullptr, nullptr, {}, 0, 0);
        }
        return;
    }
    std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};

    PipelineBindings bindings;
    std::vector<VideoCommon::ImageViewId> traced_views;
    std::vector<std::string> traced_filters;
    std::vector<u32> traced_types;
    std::vector<TracedBuffer> traced_buffers;
    if (trace_draws) {
        bindings.trace_views = &traced_views;
        bindings.trace_filters = &traced_filters;
        bindings.trace_types = &traced_types;
        if (trace_dumps) {
            buffer_runtime.SetTraceBuffers(&traced_buffers);
        }
    }
    pipeline->Configure(is_indexed,
                        {*maxwell3d, *gpu_memory, buffer_cache, texture_cache, descriptor_queue,
                         sampler_heap},
                        bindings);
    buffer_runtime.SetTraceBuffers(nullptr);
    if (!pipeline->Handle()) {
        if (trace_draws) {
            TraceDraw("draw skipped (PSO rejected)", pipeline, nullptr, traced_views, 0, 0);
        }
        return; // D3D12 rejected the PSO (logged when it was built)
    }
    VideoCore::Perf::ScopedNsTimer record_timer{VideoCore::Perf::Counter::DrawRecordNs};
    const Framebuffer* const framebuffer = texture_cache.GetFramebuffer();
    framebuffer->PrepareAttachments(bindings.depth_sampled);

    const auto& draw_state = maxwell3d->draw_manager.draw_state;
    DrawParams params{
        .num_vertices = is_indexed ? draw_state.index_buffer.count : draw_state.vertex_buffer.count,
        .num_instances = instance_count,
        .first_index = is_indexed ? draw_state.index_buffer.first : 0,
        .base_vertex = is_indexed ? draw_state.base_index : draw_state.vertex_buffer.first,
        .base_instance = draw_state.base_instance,
        .is_indexed = is_indexed,
        .runtime_first_vertex = is_indexed ? draw_state.base_index : draw_state.vertex_buffer.first,
    };
    if (!is_indexed && BufferCacheRuntime::IsEmulatedTopology(draw_state.topology)) {
        buffer_runtime.EmulateTopology(draw_state.topology, draw_state.vertex_buffer.first,
                                       draw_state.vertex_buffer.count);
    }
    if (const std::optional<u32> rewritten = buffer_runtime.RewrittenIndexCount()) {
        // Indexed from a rewritten list that starts at index 0; guest indices keep the guest's
        // base vertex, generated ones are absolute vertex numbers.
        params.num_vertices = *rewritten;
        params.first_index = 0;
        params.base_vertex = is_indexed ? draw_state.base_index : 0;
        params.runtime_first_vertex = params.base_vertex;
        params.is_indexed = true;
    }
    if (params.num_vertices == 0 || params.num_instances == 0) {
        buffer_runtime.ApplyGeometry(scheduler.CommandList());
        if (trace_draws) {
            TraceDraw("draw skipped (empty)", pipeline, framebuffer, traced_views,
                      params.num_vertices, params.num_instances);
        }
        return;
    }
    RecordDraw(*pipeline, bindings, *framebuffer, params, draw_state.topology);
    if (trace_draws) {
        if (!traced_filters.empty()) {
            LOG_INFO(Render, "D3D12 trace #{} samplers: {} types: {}", trace_index,
                     fmt::join(traced_filters, " / "), fmt::join(traced_types, " "));
        }
        TraceDraw(params.is_indexed ? "draw indexed" : "draw", pipeline, framebuffer,
                  traced_views, params.num_vertices, params.num_instances);
        if (trace_dumps) {
            CheckTracedBuffers(traced_buffers, params, trace_non_finite_grew);
        }
    }
    if (!logged_first_draw) {
        LOG_INFO(Render,
                 "D3D12: first guest draw recorded ({} {} vertices, {} instances, {} RTs{})",
                 params.is_indexed ? "indexed" : "non-indexed", params.num_vertices,
                 params.num_instances, framebuffer->ColorTargets().size(),
                 framebuffer->DepthTarget().ptr ? ", depth" : "");
        logged_first_draw = true;
    }
}

void RasterizerD3D12::DrawIndirect() {
    ++draw_counter;
    const IndirectParams& params = maxwell3d->draw_manager.indirect_state;
    if (params.is_byte_count) {
        // Transform feedback draws; HasDrawTransformFeedback() is false, so the macro should
        // have drawn them directly.
        if (!logged_byte_count_draw) {
            LOG_WARNING(Render, "D3D12: byte-count indirect draws are skipped");
            logged_byte_count_draw = true;
        }
        return;
    }
    if (params.max_draw_counts == 0) {
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
                         sampler_heap},
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

void RasterizerD3D12::RecordDraw(const GraphicsPipeline& pipeline,
                                 const PipelineBindings& bindings, const Framebuffer& framebuffer,
                                 const DrawParams& params,
                                 Maxwell::PrimitiveTopology topology) {
    BindDrawState(pipeline, bindings, framebuffer, params, topology);
    ID3D12GraphicsCommandList* const cmd = scheduler.CommandList();
    VideoCore::Perf::Add(VideoCore::Perf::Counter::Draws, 1);
    if (params.is_indexed) {
        cmd->DrawIndexedInstanced(params.num_vertices, params.num_instances, params.first_index,
                                  static_cast<INT>(params.base_vertex), params.base_instance);
    } else {
        cmd->DrawInstanced(params.num_vertices, params.num_instances, params.base_vertex,
                           params.base_instance);
    }
}

void RasterizerD3D12::BindDrawState(const GraphicsPipeline& pipeline,
                                    const PipelineBindings& bindings,
                                    const Framebuffer& framebuffer, const DrawParams& params,
                                    Maxwell::PrimitiveTopology topology) {
    const auto& regs = maxwell3d->regs;
    const PipelineLayout& layout = pipeline.Layout();
    ID3D12GraphicsCommandList* const cmd = scheduler.CommandList();

    if (!command_state.heaps_bound) {
        ID3D12DescriptorHeap* const heaps[] = {descriptor_ring.Heap(), sampler_heap.Heap()};
        cmd->SetDescriptorHeaps(2, heaps);
        command_state.heaps_bound = true;
    }
    if (!command_state.valid || command_state.graphics_root != layout.Handle()) {
        cmd->SetGraphicsRootSignature(layout.Handle());
        command_state.graphics_root = layout.Handle();
    }
    scheduler.SetPipelineState(pipeline.Handle());

    const auto color_targets = framebuffer.ColorTargets();
    const D3D12_CPU_DESCRIPTOR_HANDLE depth = framebuffer.DepthTarget(bindings.depth_sampled);
    bool targets_changed = !command_state.valid ||
                           command_state.num_color_targets != color_targets.size() ||
                           command_state.depth_target != depth.ptr;
    for (size_t index = 0; index < color_targets.size(); ++index) {
        targets_changed |= command_state.color_targets[index] != color_targets[index].ptr;
    }
    if (targets_changed) {
        cmd->OMSetRenderTargets(static_cast<UINT>(color_targets.size()),
                                color_targets.empty() ? nullptr : color_targets.data(), FALSE,
                                depth.ptr ? &depth : nullptr);
        command_state.num_color_targets = static_cast<u32>(color_targets.size());
        command_state.color_targets.fill(0);
        for (size_t index = 0; index < color_targets.size(); ++index) {
            command_state.color_targets[index] = color_targets[index].ptr;
        }
        command_state.depth_target = depth.ptr;
    }

    if (!command_state.valid || state_tracker.TouchViewports()) {
        command_state.viewport = UpdateViewports(cmd);
    }
    if (!command_state.valid || state_tracker.TouchScissors()) {
        UpdateScissors(cmd);
    }
    const std::array blend_factor{regs.blend_color.r, regs.blend_color.g, regs.blend_color.b,
                                  regs.blend_color.a};
    if (!command_state.valid || state_tracker.TouchBlendConstants() ||
        command_state.blend_factor != blend_factor) {
        cmd->OMSetBlendFactor(blend_factor.data());
        command_state.blend_factor = blend_factor;
    }
    // D3D12 has a single reference for both faces.
    if (!command_state.valid || state_tracker.TouchStencilReference() ||
        command_state.stencil_ref != regs.stencil_front_ref) {
        cmd->OMSetStencilRef(regs.stencil_front_ref);
        command_state.stencil_ref = regs.stencil_front_ref;
    }
    if (regs.stencil_two_side_enable != 0 && regs.stencil_back_ref != regs.stencil_front_ref &&
        !logged_stencil_ref) {
        LOG_WARNING(Render, "D3D12: different front and back stencil references; using the front");
        logged_stencil_ref = true;
    }

    cmd->SetGraphicsRoot32BitConstants(PipelineLayout::PUSH_CONSTANTS_INDEX, PUSH_CONSTANT_WORDS,
                                       bindings.push_constants.data(), 0);
    dxil_spirv_vertex_runtime_data runtime_data{};
    runtime_data.first_vertex = params.runtime_first_vertex;
    runtime_data.base_instance = params.base_instance;
    runtime_data.is_indexed_draw = params.is_indexed;
    runtime_data.yz_flip_mask = command_state.viewport.yz_flip_mask;
    runtime_data.viewport_width = command_state.viewport.width;
    runtime_data.viewport_height = command_state.viewport.height;
    std::array<u32, sizeof(runtime_data) / sizeof(u32)> runtime_words{};
    std::memcpy(runtime_words.data(), &runtime_data, sizeof(runtime_data));
    cmd->SetGraphicsRoot32BitConstants(PipelineLayout::RUNTIME_DATA_INDEX,
                                       std::min<UINT>(layout.RuntimeDataWords(),
                                                      static_cast<UINT>(runtime_words.size())),
                                       runtime_words.data(), 0);
    if (layout.ResourceTableIndex() != PipelineLayout::NO_TABLE) {
        cmd->SetGraphicsRootDescriptorTable(layout.ResourceTableIndex(), bindings.resource_table);
    }
    if (layout.SamplerTableIndex() != PipelineLayout::NO_TABLE) {
        cmd->SetGraphicsRootDescriptorTable(layout.SamplerTableIndex(), bindings.sampler_table);
    }

    const D3D12_PRIMITIVE_TOPOLOGY d3d_topology =
        MaxwellToD3D12::PrimitiveTopology(topology, regs.patch_vertices);
    if (!command_state.valid || command_state.topology != d3d_topology) {
        cmd->IASetPrimitiveTopology(d3d_topology);
        command_state.topology = d3d_topology;
    }
    buffer_runtime.ApplyGeometry(cmd);
    command_state.valid = true;
}

void RasterizerD3D12::InvalidateGraphicsState() {
    command_state.valid = false;
    command_state.graphics_root = nullptr;
}

void RasterizerD3D12::InvalidateCommandListState() {
    command_state = {};
    state_invalidation_pending = channel_bound;
}

void RasterizerD3D12::ApplyPendingStateInvalidation() {
    if (state_invalidation_pending && channel_bound) {
        state_tracker.InvalidateState();
        state_invalidation_pending = false;
    }
}

RasterizerD3D12::ViewportState RasterizerD3D12::UpdateViewports(ID3D12GraphicsCommandList* cmd) {
    std::array<D3D12_VIEWPORT, Maxwell::NumViewports> viewports{};
    const ViewportState state = ComputeViewports(viewports);
    cmd->RSSetViewports(static_cast<UINT>(viewports.size()), viewports.data());
    return state;
}

RasterizerD3D12::ViewportState RasterizerD3D12::ComputeViewports(
    std::array<D3D12_VIEWPORT, Maxwell::NumViewports>& viewports) const {
    const auto& regs = maxwell3d->regs;
    ViewportState state{};
    for (size_t index = 0; index < viewports.size(); ++index) {
        // Vulkan's viewport (vk_rasterizer.cpp GetViewportState), then D3D12's: a Vulkan viewport
        // of positive height maps NDC y = -1 to its top, a D3D12 one to its bottom, so those get
        // the shader's y-flip; negative heights (lower-left origin, NegativeY swizzle) are the
        // D3D12 orientation already and only need a positive height (as Dozen does).
        const auto& src = regs.viewport_transform[index];
        const float x = src.translate_x - src.scale_x;
        float width = src.scale_x * 2.0f;
        float y = src.translate_y - src.scale_y;
        float height = src.scale_y * 2.0f;
        if (regs.window_origin.mode != Maxwell::WindowOrigin::Mode::UpperLeft) {
            y += static_cast<f32>(regs.surface_clip.height);
            height = -height;
        }
        if (src.swizzle.y == Maxwell::ViewportSwizzle::NegativeY) {
            y += height;
            height = -height;
        }
        const float reduce_z = regs.depth_mode == Maxwell::DepthMode::MinusOneToOne ? 1.0f : 0.0f;
        float min_depth = std::clamp(src.translate_z - src.scale_z * reduce_z, 0.0f, 1.0f);
        float max_depth = std::clamp(src.translate_z + src.scale_z, 0.0f, 1.0f);
        if (width == 0.0f) {
            width = 1.0f;
        }
        if (height == 0.0f) {
            height = 1.0f;
        }
        D3D12_VIEWPORT& viewport = viewports[index];
        viewport.TopLeftX = width > 0.0f ? x : x + width;
        viewport.Width = std::abs(width);
        if (height > 0.0f) {
            state.yz_flip_mask |= 1u << index;
            viewport.TopLeftY = y;
            viewport.Height = height;
        } else {
            viewport.TopLeftY = y + height;
            viewport.Height = -height;
        }
        if (min_depth > max_depth) {
            // D3D12 wants MinDepth <= MaxDepth: swap them and flip z in the shader instead.
            state.yz_flip_mask |= 1u << (DXIL_SPIRV_Z_FLIP_SHIFT + index);
            std::swap(min_depth, max_depth);
        }
        viewport.MinDepth = min_depth;
        viewport.MaxDepth = max_depth;
        viewport.TopLeftX = std::clamp(viewport.TopLeftX, -32768.0f, 32767.0f);
        viewport.TopLeftY = std::clamp(viewport.TopLeftY, -32768.0f, 32767.0f);
        viewport.Width = std::min(viewport.Width, 32767.0f - viewport.TopLeftX);
        viewport.Height = std::min(viewport.Height, 32767.0f - viewport.TopLeftY);
    }
    state.width = viewports[0].Width;
    state.height = viewports[0].Height;
    return state;
}

void RasterizerD3D12::UpdateScissors(ID3D12GraphicsCommandList* cmd) {
    std::array<D3D12_RECT, Maxwell::NumViewports> scissors{};
    for (size_t index = 0; index < scissors.size(); ++index) {
        scissors[index] = ScissorRect(index);
    }
    cmd->RSSetScissorRects(static_cast<UINT>(scissors.size()), scissors.data());
}

D3D12_RECT RasterizerD3D12::ScissorRect(size_t index) const {
    // vk_rasterizer.cpp GetScissorState; D3D12 always scissors, so a disabled test covers the
    // largest render target.
    constexpr LONG MAX_EXTENT = D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION;
    const auto& regs = maxwell3d->regs;
    const auto& src = regs.scissor_test[index];
    if (!src.enable) {
        return {0, 0, MAX_EXTENT, MAX_EXTENT};
    }
    const bool lower_left = regs.window_origin.mode != Maxwell::WindowOrigin::Mode::UpperLeft;
    const s32 clip_height = static_cast<s32>(regs.surface_clip.height);
    s32 min_y = lower_left ? clip_height - static_cast<s32>(src.max_y) : static_cast<s32>(src.min_y);
    s32 max_y = lower_left ? clip_height - static_cast<s32>(src.min_y) : static_cast<s32>(src.max_y);
    min_y = std::clamp<s32>(min_y, 0, MAX_EXTENT);
    max_y = std::clamp<s32>(max_y, min_y, MAX_EXTENT);
    const s32 min_x = std::clamp<s32>(static_cast<s32>(src.min_x), 0, MAX_EXTENT);
    const s32 max_x = std::clamp<s32>(static_cast<s32>(src.max_x), min_x, MAX_EXTENT);
    return {min_x, min_y, max_x, max_y};
}

void RasterizerD3D12::DrawTexture() {
    ++draw_counter;
    SCOPE_EXIT {
        gpu.TickWork();
    };
    std::scoped_lock lock{texture_cache.mutex};
    texture_cache.SynchronizeDescriptors(false);
    texture_cache.UpdateRenderTargets(false);

    // Vulkan's DrawTexture: a scaled blit of the texture into render target 0.
    const auto& state = maxwell3d->draw_manager.draw_texture_state;
    const Sampler& sampler = *texture_cache.GetSampler(state.src_sampler, false);
    const ImageView& texture = texture_cache.GetImageView(state.src_texture);
    const Framebuffer* const framebuffer = texture_cache.GetFramebuffer();
    if (!framebuffer->HasColor(0)) {
        return;
    }
    using namespace VideoCore::Surface;
    const PixelFormat format = PixelFormatFromRenderTargetFormat(maxwell3d->regs.rt[0].format);
    if (IsPixelFormatInteger(format) || IsPixelFormatInteger(texture.format)) {
        if (!logged_draw_texture) {
            LOG_WARNING(Render, "D3D12: DrawTexture with integer formats is skipped");
            logged_draw_texture = true;
        }
        return;
    }
    const VideoCommon::Region2D dst_region{
        {static_cast<s32>(state.dst_x0), static_cast<s32>(state.dst_y0)},
        {static_cast<s32>(state.dst_x1), static_cast<s32>(state.dst_y1)}};
    const VideoCommon::Region2D src_region{
        {static_cast<s32>(state.src_x0), static_cast<s32>(state.src_y0)},
        {static_cast<s32>(state.src_x1), static_cast<s32>(state.src_y1)}};
    framebuffer->PrepareAttachments();
    if (trace_draws) {
        TraceDraw(fmt::format("draw texture from {} {}x{}", texture.format, texture.size.width,
                              texture.size.height),
                  nullptr, framebuffer, {}, 4, 1);
    }
    texture.TransitionImage(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    blit_helper.BlitColor(
        {framebuffer->ColorTargets()[0], framebuffer->ColorFormat(0), framebuffer->Samples()},
        texture.Handle(Shader::TextureType::Color2D), {sampler.Handle(), sampler.Key()},
        dst_region, src_region, {texture.size.width, texture.size.height});
    InvalidateGraphicsState();
}

void RasterizerD3D12::Clear(u32 layer_count) {
    ++draw_counter;
    VideoCore::Perf::ScopedNsTimer clear_timer{VideoCore::Perf::Counter::ClearNs};
    ApplyPendingStateInvalidation();
    gpu_memory->FlushCaching();
    const auto& regs = maxwell3d->regs;
    const bool use_color = regs.clear_surface.R || regs.clear_surface.G || regs.clear_surface.B ||
                           regs.clear_surface.A;
    const bool use_depth = regs.clear_surface.Z;
    const bool use_stencil = regs.clear_surface.S;
    if (!use_color && !use_depth && !use_stencil) {
        return;
    }
    std::scoped_lock lock{texture_cache.mutex};
    texture_cache.UpdateRenderTargets(true);
    const Framebuffer* const framebuffer = texture_cache.GetFramebuffer();
    framebuffer->PrepareAttachments();
    if (layer_count > 1 || regs.clear_surface.layer != 0) {
        // The views cover every layer they were created with.
        if (!logged_layer_clear) {
            LOG_WARNING(Render, "D3D12: layered clears clear every layer of the view");
            logged_layer_clear = true;
        }
    }

    const VideoCommon::Extent2D extent = framebuffer->Extent();
    D3D12_RECT rect{0, 0, static_cast<LONG>(extent.width), static_cast<LONG>(extent.height)};
    if (regs.clear_control.use_scissor) {
        const D3D12_RECT scissor = ScissorRect(0);
        rect.left = std::max(rect.left, scissor.left);
        rect.top = std::max(rect.top, scissor.top);
        rect.right = std::min(rect.right, scissor.right);
        rect.bottom = std::min(rect.bottom, scissor.bottom);
    }
    if (trace_draws) {
        TraceDraw(fmt::format("clear rt{} mask {}{}{}{} depth {} stencil {} rect {},{}-{},{} "
                              "color {:.3f},{:.3f},{:.3f},{:.3f}",
                              regs.clear_surface.RT.Value(), regs.clear_surface.R.Value(),
                              regs.clear_surface.G.Value(), regs.clear_surface.B.Value(),
                              regs.clear_surface.A.Value(), use_depth, use_stencil, rect.left,
                              rect.top, rect.right, rect.bottom, regs.clear_color[0],
                              regs.clear_color[1], regs.clear_color[2], regs.clear_color[3]),
                  nullptr, framebuffer, {}, 0, 0);
    }
    if (rect.right <= rect.left || rect.bottom <= rect.top) {
        return;
    }
    ID3D12GraphicsCommandList* const cmd = scheduler.CommandList();

    const u32 color_attachment = regs.clear_surface.RT;
    if (use_color && framebuffer->HasColor(color_attachment)) {
        const bool full_mask = regs.clear_surface.R && regs.clear_surface.G &&
                               regs.clear_surface.B && regs.clear_surface.A;
        using namespace VideoCore::Surface;
        const PixelFormat format =
            PixelFormatFromRenderTargetFormat(regs.rt[color_attachment].format);
        if (!full_mask) {
            // Drawn with the write mask in the PSO (Vulkan uses the blend constant instead).
            if (IsPixelFormatInteger(format)) {
                if (!logged_integer_clear) {
                    LOG_WARNING(Render,
                                "D3D12: masked clears of integer render targets are skipped");
                    logged_integer_clear = true;
                }
            } else {
                const u8 mask = static_cast<u8>(regs.clear_surface.R | regs.clear_surface.G << 1 |
                                                regs.clear_surface.B << 2 |
                                                regs.clear_surface.A << 3);
                blit_helper.ClearColor({framebuffer->ColorTargets()[color_attachment],
                                        framebuffer->ColorFormat(color_attachment),
                                        framebuffer->Samples()},
                                       mask, regs.clear_color, rect);
                InvalidateGraphicsState();
            }
        } else {
            std::array<f32, 4> color{};
            // Integer targets take the value converted to the integer range, as Vulkan's
            // clear; D3D12 converts the floats to integers.
            if (!IsPixelFormatInteger(format)) {
                color = regs.clear_color;
            } else if (!IsPixelFormatSignedInteger(format)) {
                const size_t int_size = PixelComponentSizeBitsInteger(format);
                for (size_t i = 0; i < 4; ++i) {
                    color[i] = static_cast<f32>(static_cast<u32>(
                        static_cast<f32>(static_cast<u64>(int_size) << 1U) * regs.clear_color[i]));
                }
            } else {
                const size_t int_size = PixelComponentSizeBitsInteger(format);
                for (size_t i = 0; i < 4; ++i) {
                    color[i] = static_cast<f32>(static_cast<s32>(
                        static_cast<f32>(static_cast<s64>(int_size - 1) << 1) *
                        (regs.clear_color[i] - 0.5f)));
                }
            }
            cmd->ClearRenderTargetView(framebuffer->ColorTargets()[color_attachment], color.data(),
                                       1, &rect);
        }
    }
    const D3D12_CPU_DESCRIPTOR_HANDLE depth = framebuffer->DepthTarget();
    if ((use_depth || use_stencil) && depth.ptr) {
        D3D12_CLEAR_FLAGS clear_flags{};
        if (use_depth) {
            clear_flags |= D3D12_CLEAR_FLAG_DEPTH;
        }
        // A partial stencil mask (as Vulkan, 0 counts as full) is drawn, depth included.
        const bool stencil_partial = use_stencil && framebuffer->HasStencil() &&
                                     regs.stencil_front_mask != 0xFF &&
                                     regs.stencil_front_mask != 0;
        if (stencil_partial) {
            blit_helper.ClearDepthStencil(
                {depth, framebuffer->DepthFormat(), framebuffer->Samples()}, use_depth,
                regs.clear_depth, static_cast<u8>(regs.stencil_front_mask),
                static_cast<u8>(regs.clear_stencil), rect);
            InvalidateGraphicsState();
            clear_flags = {};
        } else if (use_stencil && framebuffer->HasStencil()) {
            clear_flags |= D3D12_CLEAR_FLAG_STENCIL;
        }
        if (clear_flags != 0) {
            cmd->ClearDepthStencilView(depth, clear_flags, regs.clear_depth,
                                       static_cast<u8>(regs.clear_stencil), 1, &rect);
        }
    }
}

void RasterizerD3D12::DispatchCompute() {
    ++draw_counter;
    VideoCore::Perf::ScopedNsTimer dispatch_timer{VideoCore::Perf::Counter::DispatchNs};
    SCOPE_EXIT {
        FlushIfUploadHeavy();
    };
    gpu_memory->FlushCaching();
    ApplyPendingStateInvalidation();

    ComputePipeline* const pipeline = pipeline_cache.CurrentComputePipeline();
    if (!pipeline) {
        return;
    }
    std::scoped_lock lock{texture_cache.mutex, buffer_cache.mutex};
    PipelineBindings bindings;
    pipeline->Configure({*kepler_compute, *gpu_memory, buffer_cache, texture_cache,
                         descriptor_queue, sampler_heap},
                        bindings);
    if (!pipeline->Handle()) {
        return; // D3D12 rejected the PSO (logged when it was built)
    }

    const auto& qmd = kepler_compute->launch_description;
    const PipelineLayout& layout = pipeline->Layout();
    const std::array<u32, 3> dim{qmd.grid_dim_x, qmd.grid_dim_y, qmd.grid_dim_z};
    std::optional<IndirectArgumentRing::Range> indirect_range;
    if (const std::optional<GPUVAddr> indirect = kepler_compute->GetIndirectComputeAddress()) {
        // The group counts are also runtime data (root constants): the record carries them
        // twice, once for the constants and once for the dispatch, both copied on the GPU. The
        // D3D12 limit of 65535 groups cannot be checked here.
        const auto [buffer, offset] = buffer_cache.ObtainBuffer(
            *indirect, 3 * sizeof(u32), VideoCommon::ObtainBufferSynchronize::FullSynchronize,
            VideoCommon::ObtainBufferOperation::DiscardWrite);
        if (!layout.DispatchSignature() || !buffer || !buffer->Handle()) {
            return;
        }
        indirect_range = indirect_args.Allocate(INDIRECT_DISPATCH_WORDS * sizeof(u32));
        buffer->Transition(D3D12_RESOURCE_STATE_GENERIC_READ);
        ID3D12GraphicsCommandList* const copy_cmd = scheduler.CommandList();
        for (u32 copy = 0; copy < 2; ++copy) {
            copy_cmd->CopyBufferRegion(
                indirect_range->buffer,
                indirect_range->offset + copy * INDIRECT_DISPATCH_CONSTANT_WORDS * sizeof(u32),
                buffer->Handle(), offset, 3 * sizeof(u32));
        }
        indirect_args.Ready(*indirect_range);
    } else {
        constexpr u32 max_dim = D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION;
        if (dim[0] == 0 || dim[1] == 0 || dim[2] == 0) {
            return;
        }
        if (dim[0] > max_dim || dim[1] > max_dim || dim[2] > max_dim) {
            LOG_WARNING(Render, "D3D12: dispatch of {}x{}x{} groups exceeds the limit, skipped",
                        dim[0], dim[1], dim[2]);
            return;
        }
    }

    ID3D12GraphicsCommandList* const cmd = scheduler.CommandList();
    if (!command_state.heaps_bound) {
        ID3D12DescriptorHeap* const heaps[] = {descriptor_ring.Heap(), sampler_heap.Heap()};
        cmd->SetDescriptorHeaps(2, heaps);
        command_state.heaps_bound = true;
    }
    cmd->SetComputeRootSignature(layout.Handle());
    scheduler.SetPipelineState(pipeline->Handle());
    cmd->SetComputeRoot32BitConstants(PipelineLayout::PUSH_CONSTANTS_INDEX, PUSH_CONSTANT_WORDS,
                                      bindings.push_constants.data(), 0);
    dxil_spirv_compute_runtime_data runtime_data{};
    runtime_data.group_count_x = dim[0];
    runtime_data.group_count_y = dim[1];
    runtime_data.group_count_z = dim[2];
    std::array<u32, sizeof(runtime_data) / sizeof(u32)> runtime_words{};
    std::memcpy(runtime_words.data(), &runtime_data, sizeof(runtime_data));
    cmd->SetComputeRoot32BitConstants(PipelineLayout::RUNTIME_DATA_INDEX,
                                      std::min<UINT>(layout.RuntimeDataWords(),
                                                     static_cast<UINT>(runtime_words.size())),
                                      runtime_words.data(), 0);
    if (layout.ResourceTableIndex() != PipelineLayout::NO_TABLE) {
        cmd->SetComputeRootDescriptorTable(layout.ResourceTableIndex(), bindings.resource_table);
    }
    if (layout.SamplerTableIndex() != PipelineLayout::NO_TABLE) {
        cmd->SetComputeRootDescriptorTable(layout.SamplerTableIndex(), bindings.sampler_table);
    }
    VideoCore::Perf::Add(VideoCore::Perf::Counter::Dispatches, 1);
    if (indirect_range) {
        cmd->ExecuteIndirect(layout.DispatchSignature(), 1, indirect_range->buffer,
                             indirect_range->offset, nullptr, 0);
        if (!logged_indirect_dispatch) {
            LOG_INFO(Render, "D3D12: first indirect dispatch recorded (groups of {}x{}x{})",
                     qmd.block_dim_x, qmd.block_dim_y, qmd.block_dim_z);
            logged_indirect_dispatch = true;
        }
        return;
    }
    cmd->Dispatch(dim[0], dim[1], dim[2]);
    if (!logged_first_dispatch) {
        LOG_INFO(Render, "D3D12: first guest dispatch recorded ({}x{}x{} groups of {}x{}x{})",
                 dim[0], dim[1], dim[2], qmd.block_dim_x, qmd.block_dim_y, qmd.block_dim_z);
        logged_first_dispatch = true;
    }
}

std::optional<RasterizerD3D12::DisplayTexture> RasterizerD3D12::AccelerateDisplay(
    const Tegra::FramebufferConfig& config, DAddr framebuffer_addr) {
    if (!framebuffer_addr) {
        return std::nullopt;
    }
    std::scoped_lock lock{texture_cache.mutex};
    const auto [image_view, scaled] =
        texture_cache.TryFindFramebufferImageView(config, framebuffer_addr);
    if (!image_view) {
        return std::nullopt;
    }
    image_view->TransitionImage(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return DisplayTexture{
        .srv = image_view->Handle(Shader::TextureType::Color2D),
        .width = image_view->size.width,
        .height = image_view->size.height,
    };
}

void RasterizerD3D12::ResetCounter(VideoCommon::QueryType type) {
    const auto host_type = MaxwellToVideoCoreQuery(type);
    if (host_type) query_cache.ResetCounter(*host_type);
}

void RasterizerD3D12::Query(GPUVAddr address, VideoCommon::QueryType type,
                            VideoCommon::QueryPropertiesFlags flags, u32 payload, u32) {
    const auto host_type = MaxwellToVideoCoreQuery(type);
    if (!host_type) return QueryFallback(address, type, flags, payload);
    const bool timeout = True(flags & VideoCommon::QueryPropertiesFlags::HasTimeout);
    query_cache.Query(address, *host_type,
                      timeout ? std::optional<u64>{gpu.GetTicks()} : std::nullopt);
}

void RasterizerD3D12::QueryFallback(GPUVAddr address, VideoCommon::QueryType type,
                                    VideoCommon::QueryPropertiesFlags flags, u32 payload) {
    if (!gpu_memory) return;
    if (type != VideoCommon::QueryType::Payload) payload = 1;
    auto operation = [this, address, flags, payload, memory = gpu_memory] {
        if (True(flags & VideoCommon::QueryPropertiesFlags::HasTimeout)) {
            memory->Write<u64>(address + 8, gpu.GetTicks());
            memory->Write<u64>(address, payload);
        } else {
            memory->Write<u32>(address, payload);
        }
    };
    if (True(flags & VideoCommon::QueryPropertiesFlags::IsAFence)) {
        SignalFence(std::move(operation));
    } else {
        operation();
    }
}

void RasterizerD3D12::BindGraphicsUniformBuffer(size_t stage, u32 index, GPUVAddr address,
                                                u32 size) {
    std::scoped_lock lock{buffer_cache.mutex};
    buffer_cache.BindGraphicsUniformBuffer(stage, index, address, size);
}
void RasterizerD3D12::DisableGraphicsUniformBuffer(size_t stage, u32 index) {
    buffer_cache.DisableGraphicsUniformBuffer(stage, index);
}
void RasterizerD3D12::FlushAll() { scheduler.Finish(); }

void RasterizerD3D12::FlushRegion(DAddr address, u64 size, VideoCommon::CacheType which) {
    if (!address || !size) return;
    if (True(which & VideoCommon::CacheType::TextureCache)) {
        std::scoped_lock lock{texture_cache.mutex};
        texture_cache.DownloadMemory(address, size);
    }
    if (True(which & VideoCommon::CacheType::BufferCache)) {
        std::scoped_lock lock{buffer_cache.mutex};
        buffer_cache.DownloadMemory(address, size);
    }
    if (True(which & VideoCommon::CacheType::QueryCache)) query_cache.FlushRegion(address, size);
}

bool RasterizerD3D12::MustFlushRegion(DAddr address, u64 size, VideoCommon::CacheType which) {
    if (True(which & VideoCommon::CacheType::BufferCache)) {
        std::scoped_lock lock{buffer_cache.mutex};
        if (buffer_cache.IsRegionGpuModified(address, size)) return true;
    }
    if (Settings::IsGPULevelHigh() && True(which & VideoCommon::CacheType::TextureCache)) {
        std::scoped_lock lock{texture_cache.mutex};
        return texture_cache.IsRegionGpuModified(address, size);
    }
    return false;
}

VideoCore::RasterizerDownloadArea RasterizerD3D12::GetFlushArea(DAddr address, u64 size) {
    {
        std::scoped_lock lock{texture_cache.mutex};
        if (auto area = texture_cache.GetFlushArea(address, size)) return *area;
    }
    {
        std::scoped_lock lock{buffer_cache.mutex};
        if (auto area = buffer_cache.GetFlushArea(address, size)) return *area;
    }
    return {.start_address = Common::AlignDown(address, Core::DEVICE_PAGESIZE),
            .end_address = Common::AlignUp(address + size, Core::DEVICE_PAGESIZE),
            .preemtive = true};
}

void RasterizerD3D12::InvalidateRegion(DAddr address, u64 size, VideoCommon::CacheType which) {
    if (!address || !size) return;
    if (True(which & VideoCommon::CacheType::TextureCache)) {
        std::scoped_lock lock{texture_cache.mutex};
        texture_cache.WriteMemory(address, size);
    }
    if (True(which & VideoCommon::CacheType::BufferCache)) {
        std::scoped_lock lock{buffer_cache.mutex};
        buffer_cache.WriteMemory(address, size);
    }
    if (True(which & VideoCommon::CacheType::QueryCache)) query_cache.InvalidateRegion(address, size);
    if (True(which & VideoCommon::CacheType::ShaderCache)) pipeline_cache.InvalidateRegion(address, size);
}

bool RasterizerD3D12::OnCPUWrite(PAddr address, u64 size) {
    {
        std::scoped_lock lock{buffer_cache.mutex};
        if (buffer_cache.OnCPUWrite(address, size)) return true;
    }
    {
        std::scoped_lock lock{texture_cache.mutex};
        texture_cache.WriteMemory(address, size);
    }
    pipeline_cache.InvalidateRegion(address, size);
    return false;
}

void RasterizerD3D12::OnCacheInvalidation(PAddr address, u64 size) {
    InvalidateRegion(address, size, VideoCommon::CacheType::TextureCache |
                                        VideoCommon::CacheType::BufferCache);
}
void RasterizerD3D12::InvalidateGPUCache() { gpu.InvalidateGPUCache(); }
void RasterizerD3D12::UnmapMemory(DAddr address, u64 size) {
    { std::scoped_lock lock{texture_cache.mutex}; texture_cache.UnmapMemory(address, size); }
    { std::scoped_lock lock{buffer_cache.mutex}; buffer_cache.WriteMemory(address, size); }
    pipeline_cache.OnCacheInvalidation(address, size);
}
void RasterizerD3D12::ModifyGPUMemory(size_t as_id, GPUVAddr address, u64 size) {
    std::scoped_lock lock{texture_cache.mutex};
    texture_cache.UnmapGPUMemory(as_id, address, size);
}
void RasterizerD3D12::SignalFence(std::function<void()>&& f) { fence_manager.SignalFence(std::move(f)); }
void RasterizerD3D12::SyncOperation(std::function<void()>&& f) { fence_manager.SyncOperation(std::move(f)); }
void RasterizerD3D12::SignalSyncPoint(u32 value) { fence_manager.SignalSyncPoint(value); }
void RasterizerD3D12::SignalReference() { fence_manager.SignalOrdering(); }
void RasterizerD3D12::ReleaseFences(bool force) { fence_manager.WaitPendingFences(force); }
void RasterizerD3D12::FlushAndInvalidateRegion(DAddr a, u64 s, VideoCommon::CacheType w) {
    if (Settings::IsGPULevelHigh()) FlushRegion(a, s, w);
    InvalidateRegion(a, s, w);
}
void RasterizerD3D12::WaitForIdle() {
    // As Vulkan's (an event set and waited on inside the command buffer): a GPU-side barrier,
    // never a CPU wait. Games issue wait_for_idle dozens of times a frame; submitting and waiting
    // for the GPU on each one took about half of every frame (0.2.49 perf counters). One queue
    // runs in order and the caches transition what they hand over, so what is left to order are
    // UAV writes: a UAV barrier on every resource.
    const D3D12_RESOURCE_BARRIER barrier{.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV,
                                         .UAV = {.pResource = nullptr}};
    scheduler.CommandList()->ResourceBarrier(1, &barrier);
    fence_manager.SignalOrdering();
}
void RasterizerD3D12::FragmentBarrier() {
    // Fragment shaders' storage writes before later reads.
    const D3D12_RESOURCE_BARRIER barrier{.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV,
                                         .UAV = {.pResource = nullptr}};
    scheduler.CommandList()->ResourceBarrier(1, &barrier);
}
void RasterizerD3D12::TiledCacheBarrier() {
    // Nothing: the host has no tiled cache to flush (Vulkan does the same).
}
void RasterizerD3D12::FlushCommands() {
    // Called at the end of every guest command list: submit when something was drawn, or when
    // enough was uploaded. Loading screens upload thousands of small buffers without drawing;
    // left unsubmitted, the staging stream cannot be reused and every upload became a new
    // committed buffer until the console ran out of memory (0.2.49, 125 s into Wonder).
    constexpr u64 UPLOAD_FLUSH_BYTES = 8ULL << 20;
    if (draw_counter == 0 && staging.PendingUploadBytes() < UPLOAD_FLUSH_BYTES) {
        return;
    }
    draw_counter = 0;
    scheduler.Flush();
}
void RasterizerD3D12::FlushIfUploadHeavy() {
    if (removal_tripwire::removal_tripped.load(std::memory_order_relaxed)) return;
    // A loading frame may upload more textures than the 128 MiB staging stream holds within one
    // command list (151 MiB in Mario Wonder), and the stream only reuses submitted regions. The
    // reset callback invalidates cached state, so submitting between draws is safe.
    constexpr u64 UPLOAD_SUBMIT_BYTES = 32ULL << 20;
    if (staging.PendingUploadBytes() < UPLOAD_SUBMIT_BYTES) {
        return;
    }
    draw_counter = 0;
    scheduler.Flush();
}
void RasterizerD3D12::TickFrame() {
    draw_counter = 0;
    fence_manager.TickFrame();
    {
        // The fence worker shares staging with both caches. Retire resources only while neither
        // cache can add a reference or release a pinned readback concurrently.
        std::scoped_lock lock{texture_cache.mutex, buffer_cache.mutex};
        auto snapshot = texture_runtime.BeginMemoryGuardFrame();
        if (staging.GuardMemory(snapshot)) {
            // A retired ring can return 256 MiB at once. Refresh only on that rare transition,
            // so GC does not perform synchronous recovery using the pre-reclamation snapshot.
            snapshot = texture_runtime.BeginMemoryGuardFrame();
        }
        pipeline_cache.GuardMemory(snapshot);
        texture_cache.TickFrame();
        buffer_cache.TickFrame();
    }
}
bool RasterizerD3D12::AccelerateSurfaceCopy(const Tegra::Engines::Fermi2D::Surface& src,
                                            const Tegra::Engines::Fermi2D::Surface& dst,
                                            const Tegra::Engines::Fermi2D::Config& config) {
    std::scoped_lock lock{texture_cache.mutex};
    const bool blitted = texture_cache.BlitImage(dst, src, config);
    if (blitted) {
        InvalidateGraphicsState();
    }
    if (trace_draws) {
        TraceDraw(fmt::format("2D blit {:x} ({}x{}) -> {:x} ({}x{}) {}", src.Address(), src.width,
                              src.height, dst.Address(), dst.width, dst.height,
                              blitted ? "accelerated" : "left to the CPU"),
                  nullptr, nullptr, {}, 0, 0);
    }
    return blitted;
}

void RasterizerD3D12::SetDrawTrace(bool enabled, bool dump_targets) {
    if (!enabled && trace_dumps) {
        DumpTargets(traced_targets);
        LOG_INFO(Render, "D3D12 trace buffers: {} of {} bound buffers differ from guest memory",
                 traced_buffers_differing, traced_buffers_checked);
        LOG_INFO(Render, "D3D12 trace textures: {} of {} sampled images differ from guest memory",
                 traced_textures_differing, traced_textures_checked);
    }
    traced_buffers_checked = 0;
    traced_buffers_differing = 0;
    traced_textures_checked = 0;
    traced_textures_differing = 0;
    if (enabled && dump_targets) {
        std::error_code ec;
        const std::filesystem::path directory =
            Common::FS::GetEdenPath(Common::FS::EdenPath::LogDir) / "trace";
        std::filesystem::remove_all(directory, ec);
        std::filesystem::create_directories(directory, ec);
    }
    trace_draws = enabled;
    trace_dumps = enabled && dump_targets;
    trace_index = 0;
    traced_targets = {};
    traced_textures.clear();
    traced_non_finite.clear();
    traced_depth.clear();
    trace_non_finite_grew = false;
}

void RasterizerD3D12::DumpTextureNonFinite(const Image& image) {
    // Every level and layer (cube faces included): lighting inputs made on the GPU earlier.
    const u32 levels = static_cast<u32>(image.info.resources.levels);
    const u32 layers = image.info.type == VideoCommon::ImageType::e3D
                           ? 1U
                           : static_cast<u32>(image.info.resources.layers);
    u64 total = 0;
    std::string bad;
    for (u32 layer = 0; layer < std::min(layers, 128U); ++layer) {
        for (u32 level = 0; level < levels; ++level) {
            const std::optional<u64> count =
                DumpTarget(image, nullptr, image.Subresource(static_cast<s32>(level),
                                                             static_cast<s32>(layer)));
            if (!count) {
                return; // not decoded (compressed formats)
            }
            total += *count;
            if (*count != 0) {
                bad += fmt::format(" L{}/{}:{}", level, layer, *count);
            }
        }
    }
    LOG_INFO(Render, "D3D12 trace texture {} {}x{}x{} L{} @{:x} flags {:x}: non-finite {}{}",
             image.info.format, image.info.size.width, image.info.size.height,
             image.info.type == VideoCommon::ImageType::e3D ? image.info.size.depth : layers,
             levels, image.gpu_addr, static_cast<u32>(image.flags), total, bad);
}
void RasterizerD3D12::CheckTracedBuffers(std::span<const TracedBuffer> buffers,
                                         const DrawParams& params, bool verbose) {
    using Kind = TracedBuffer::Kind;
    constexpr u32 max_checked = 8U << 20;
    std::optional<std::pair<u32, u32>> index_range; // vertex numbers, base vertex included
    if (!params.is_indexed && params.num_vertices != 0) {
        index_range.emplace(params.base_vertex, params.base_vertex + params.num_vertices - 1);
    }
    for (const TracedBuffer& traced : buffers) {
        const char* const name = [&] {
            switch (traced.kind) {
            case Kind::Uniform:
                return "cbuf";
            case Kind::NullUniform:
                return "cbuf NULL";
            case Kind::StreamedUniform:
                return "cbuf streamed";
            case Kind::Vertex:
                return "vertex";
            case Kind::Index:
                return "index";
            case Kind::RewrittenIndex:
                return "index rewritten";
            }
            return "?";
        }();
        if (!traced.buffer || traced.size == 0) {
            if (!verbose && traced.kind != Kind::NullUniform) {
                continue;
            }
            LOG_INFO(Render,
                     "D3D12 trace buffers #{}: {} {} offset {} size {} (buffer {} bytes)",
                     trace_index - 1, name, traced.slot, traced.offset, traced.size,
                     traced.stride);
            continue;
        }
        const u32 size = std::min(traced.size, max_checked);
        std::vector<u8> guest(size);
        device_memory.ReadBlockUnsafe(traced.device_addr, guest.data(), size);
        if (traced.kind == Kind::Index && params.is_indexed && traced.stride != 0) {
            u32 low = UINT32_MAX;
            u32 high = 0;
            for (u32 i = 0; i < params.num_vertices; ++i) {
                const size_t at = static_cast<size_t>(params.first_index + i) * traced.stride;
                if (at + traced.stride > guest.size()) {
                    break;
                }
                u32 value = 0;
                std::memcpy(&value, guest.data() + at, traced.stride);
                low = std::min(low, value);
                high = std::max(high, value);
            }
            if (low <= high) {
                index_range.emplace(low + params.base_vertex, high + params.base_vertex);
            }
        }
        StagingBufferRef readback = staging.Request(size, MemoryUsage::Download, true);
        if (readback.mapped_span.size() < size) {
            staging.FreeDeferred(readback);
            continue;
        }
        traced.buffer->Transition(D3D12_RESOURCE_STATE_GENERIC_READ);
        scheduler.CommandList()->CopyBufferRegion(readback.buffer, readback.offset,
                                                  traced.buffer->Handle(), traced.offset, size);
        scheduler.Finish();
        const u8* const on_gpu = readback.mapped_span.data();
        u32 mismatched = 0;
        u32 first_mismatch = UINT32_MAX;
        u32 gpu_zero = 0;
        u32 guest_zero = 0;
        u32 gpu_non_finite = 0;
        u32 guest_non_finite = 0;
        for (u32 at = 0; at + 4 <= size; at += 4) {
            u32 a;
            u32 b;
            std::memcpy(&a, on_gpu + at, 4);
            std::memcpy(&b, guest.data() + at, 4);
            if (a != b) {
                ++mismatched;
                first_mismatch = std::min(first_mismatch, at);
            }
            gpu_zero += a == 0 ? 1 : 0;
            guest_zero += b == 0 ? 1 : 0;
            gpu_non_finite += (a & 0x7f800000U) == 0x7f800000U ? 1 : 0;
            guest_non_finite += (b & 0x7f800000U) == 0x7f800000U ? 1 : 0;
        }
        staging.FreeDeferred(readback);
        ++traced_buffers_checked;
        if (mismatched != 0) {
            ++traced_buffers_differing;
        } else if (!verbose) {
            continue;
        }
        std::string line = fmt::format(
            "D3D12 trace buffers #{}: {} {} @{:x}+{} size {} stride {}: {} of {} words differ "
            "from guest",
            trace_index - 1, name, traced.slot, traced.device_addr - traced.offset, traced.offset,
            traced.size, traced.stride, mismatched, size / 4);
        if (mismatched != 0) {
            u32 a = 0;
            u32 b = 0;
            std::memcpy(&a, on_gpu + first_mismatch, 4);
            std::memcpy(&b, guest.data() + first_mismatch, 4);
            line += fmt::format(" (first at {}: gpu {:08x} guest {:08x})", first_mismatch, a, b);
        }
        line += fmt::format(", zero words gpu {} guest {}, NaN/Inf words gpu {} guest {}", gpu_zero,
                            guest_zero, gpu_non_finite, guest_non_finite);
        if (traced.kind == Kind::Vertex && index_range && traced.stride != 0) {
            line += fmt::format(", holds {} vertices, draw reads {}..{}",
                                traced.size / traced.stride, index_range->first,
                                index_range->second);
            if (verbose && traced.stride % 4 == 0 && traced.stride <= 64) {
                // Range of each float column over the vertices the draw reads.
                const u32 columns = traced.stride / 4;
                std::array<float, 16> low;
                std::array<float, 16> high;
                low.fill(std::numeric_limits<float>::infinity());
                high.fill(-std::numeric_limits<float>::infinity());
                for (u32 vertex = index_range->first; vertex <= index_range->second; ++vertex) {
                    const size_t at = static_cast<size_t>(vertex) * traced.stride;
                    if (at + traced.stride > guest.size()) {
                        break;
                    }
                    for (u32 column = 0; column < columns; ++column) {
                        float value;
                        std::memcpy(&value, guest.data() + at + column * 4, 4);
                        low[column] = std::min(low[column], value);
                        high[column] = std::max(high[column], value);
                    }
                }
                for (u32 column = 0; column < columns; ++column) {
                    line += fmt::format(" | c{} {:.6g}..{:.6g}", column, low[column], high[column]);
                }
            }
        }
        LOG_INFO(Render, "{}", line);
    }
}

void RasterizerD3D12::DumpTargets(std::span<const VideoCommon::ImageId> targets) {
    for (size_t index = 0; index < targets.size(); ++index) {
        // The last slot is the depth buffer (its depth plane).
        const bool is_depth = index == VideoCommon::NUM_RT;
        if (targets[index] == VideoCommon::ImageId{} || !traced_images) {
            continue;
        }
        const Image* const target = &(*traced_images)[targets[index]];
        if (!target->Handle()) {
            continue; // deleted since
        }
        const std::string name = fmt::format(
            "{:03}_{}_{}_{}x{}_{:x}", trace_index == 0 ? 0 : trace_index - 1,
            is_depth ? std::string("ds") : fmt::format("rt{}", index), target->info.format,
            target->info.size.width, target->info.size.height, target->gpu_addr);
        DumpTarget(*target, &name);
    }
}

void RasterizerD3D12::CheckTracedTexture(const Image& target) {
    using VideoCommon::ImageFlagBits;
    using VideoCommon::ImageType;
    const VideoCommon::ImageInfo& info = target.info;
    const FormatInfo& transfer = target.TransferFormat();
    // What the GPU wrote, or the CPU wrote since the upload, differs from the upload for good.
    if (!target.Handle() || !transfer.supported || transfer.stencil_srv != DXGI_FORMAT_UNKNOWN ||
        info.num_samples > 1 || info.type == ImageType::e3D || info.type == ImageType::Linear ||
        info.type == ImageType::Buffer ||
        True(target.flags & (ImageFlagBits::GpuModified | ImageFlagBits::CpuModified))) {
        return;
    }
    // The texture cache's own upload path: unswizzle, then decode (ASTC) when converted.
    std::vector<u8> swizzled(target.guest_size_bytes);
    gpu_memory->ReadBlockUnsafe(target.gpu_addr, swizzled.data(), swizzled.size());
    std::vector<u8> unswizzled(target.unswizzled_size_bytes);
    auto copies = VideoCommon::UnswizzleImage(*gpu_memory, target.gpu_addr, info, swizzled,
                                              unswizzled);
    std::vector<u8> converted;
    std::span<const u8> expected = unswizzled;
    if (transfer.converted) {
        converted.resize(target.converted_size_bytes);
        VideoCommon::ConvertImage(unswizzled, info, converted, copies);
        expected = converted;
    }
    const VideoCore::Surface::PixelFormat copy_format = transfer.copy_format;
    const u32 block_w = VideoCore::Surface::DefaultBlockWidth(copy_format);
    const u32 block_h = VideoCore::Surface::DefaultBlockHeight(copy_format);
    const u32 block_bytes = VideoCore::Surface::BytesPerBlock(copy_format);
    const bool rgba8 = copy_format == VideoCore::Surface::PixelFormat::A8B8G8R8_UNORM;

    Image& image = const_cast<Image&>(target);
    ++traced_textures_checked;
    std::string detail;
    u32 differing_subresources = 0;
    for (const VideoCommon::BufferImageCopy& copy : copies) {
        const u32 level = static_cast<u32>(copy.image_subresource.base_level);
        const u32 row_length =
            copy.buffer_row_length != 0 ? copy.buffer_row_length : copy.image_extent.width;
        const u32 image_height =
            copy.buffer_image_height != 0 ? copy.buffer_image_height : copy.image_extent.height;
        const u32 blocks_x = std::max(1U, Common::DivCeil(copy.image_extent.width, block_w));
        const u32 rows = std::max(1U, Common::DivCeil(copy.image_extent.height, block_h));
        const u64 row_bytes =
            static_cast<u64>(std::max(1U, Common::DivCeil(row_length, block_w))) * block_bytes;
        const u64 slice = row_bytes * std::max(rows, Common::DivCeil(image_height, block_h));
        const u32 layers = static_cast<u32>(std::max(1, copy.image_subresource.num_layers));
        for (u32 layer = 0; layer < layers; ++layer) {
            const u32 subresource =
                image.Subresource(static_cast<s32>(level), static_cast<s32>(layer));
            const D3D12_RESOURCE_DESC desc = image.Handle()->GetDesc();
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
            UINT num_rows = 0;
            UINT64 total_bytes = 0;
            ID3D12Device* device = nullptr;
            image.Handle()->GetDevice(IID_PPV_ARGS(&device));
            device->GetCopyableFootprints(&desc, subresource, 1, 0, &footprint, &num_rows,
                                          nullptr, &total_bytes);
            device->Release();
            if (total_bytes == 0 || total_bytes == UINT64_MAX || num_rows < rows) {
                return;
            }
            StagingBufferRef readback =
                staging.Request(static_cast<size_t>(total_bytes), MemoryUsage::Download, true);
            const D3D12_RESOURCE_STATES previous = image.State();
            image.Transition(D3D12_RESOURCE_STATE_COPY_SOURCE);
            const D3D12_TEXTURE_COPY_LOCATION src{
                .pResource = image.Handle(),
                .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
                .SubresourceIndex = subresource,
            };
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT placed = footprint;
            placed.Offset = readback.offset;
            const D3D12_TEXTURE_COPY_LOCATION dst{
                .pResource = readback.buffer,
                .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
                .PlacedFootprint = placed,
            };
            scheduler.CommandList()->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            image.Transition(previous);
            scheduler.Finish();

            const u64 base = copy.buffer_offset + layer * slice;
            u64 differing = 0;
            u64 gpu_clear = 0;
            u64 guest_clear = 0;
            std::string first;
            for (u32 y = 0; y < rows; ++y) {
                const u8* const on_gpu = readback.mapped_span.data() +
                                         static_cast<size_t>(y) * footprint.Footprint.RowPitch;
                const u64 row_at = base + y * row_bytes;
                if (row_at + static_cast<u64>(blocks_x) * block_bytes > expected.size()) {
                    break;
                }
                const u8* const guest = expected.data() + row_at;
                for (u32 x = 0; x < blocks_x; ++x) {
                    const u8* const a = on_gpu + static_cast<size_t>(x) * block_bytes;
                    const u8* const b = guest + static_cast<size_t>(x) * block_bytes;
                    if (rgba8) {
                        gpu_clear += a[3] == 0 ? 1 : 0;
                        guest_clear += b[3] == 0 ? 1 : 0;
                    }
                    if (std::memcmp(a, b, block_bytes) == 0) {
                        continue;
                    }
                    if (differing++ == 0) {
                        u32 gpu_word = 0;
                        u32 guest_word = 0;
                        std::memcpy(&gpu_word, a, std::min(4U, block_bytes));
                        std::memcpy(&guest_word, b, std::min(4U, block_bytes));
                        first = fmt::format("first at {},{}: gpu {:08x} guest {:08x}", x, y,
                                            gpu_word, guest_word);
                    }
                }
            }
            staging.FreeDeferred(readback);
            if (differing == 0) {
                continue;
            }
            if (differing_subresources++ < 8) {
                detail += fmt::format(" | L{}/{}: {} of {} blocks differ ({})", level, layer,
                                      differing, static_cast<u64>(blocks_x) * rows, first);
                if (rgba8) {
                    detail += fmt::format(", alpha 0 gpu {} guest {}", gpu_clear, guest_clear);
                }
            }
        }
    }
    if (differing_subresources != 0) {
        ++traced_textures_differing;
    }
    {
        // The host layout the driver chose, to compare arrays that sample wrong with ones that do not.
        const D3D12_RESOURCE_DESC desc = image.Handle()->GetDesc();
        ID3D12Device* device = nullptr;
        image.Handle()->GetDevice(IID_PPV_ARGS(&device));
        const D3D12_RESOURCE_ALLOCATION_INFO allocation = device->GetResourceAllocationInfo(0, 1, &desc);
        u64 layer_bytes = 0;
        device->GetCopyableFootprints(&desc, 0, desc.MipLevels, 0, nullptr, nullptr, nullptr,
                                      &layer_bytes);
        device->Release();
        detail += fmt::format(" | host format {} {}x{}x{} mips {} flags 0x{:x}: allocation {} bytes "
                              "align {}, one layer's mip chain {} bytes",
                              static_cast<u32>(desc.Format), desc.Width, desc.Height,
                              desc.DepthOrArraySize, desc.MipLevels, static_cast<u32>(desc.Flags),
                              allocation.SizeInBytes, allocation.Alignment, layer_bytes);
    }
    LOG_INFO(Render, "D3D12 trace texture check {} {}x{}x{} L{} @{:x}{}: {}{}", info.format,
             info.size.width, info.size.height, info.resources.layers, info.resources.levels,
             target.gpu_addr, transfer.converted ? " (decoded on the CPU)" : "",
             differing_subresources == 0
                 ? std::string("matches guest memory")
                 : fmt::format("{} subresources differ", differing_subresources),
             detail);
}

std::string RasterizerD3D12::TraceDepthChanges(const Image& target) {
    ID3D12Resource* const resource = target.Handle();
    if (!resource) {
        return {};
    }
    const D3D12_RESOURCE_DESC desc = resource->GetDesc();
    if (desc.SampleDesc.Count > 1) {
        return {};
    }
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT num_rows = 0;
    UINT64 row_size = 0;
    UINT64 total_bytes = 0;
    ID3D12Device* device = nullptr;
    resource->GetDevice(IID_PPV_ARGS(&device));
    device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &num_rows, &row_size, &total_bytes);
    device->Release();
    const u32 width = footprint.Footprint.Width;
    if (total_bytes == 0 || total_bytes == UINT64_MAX || width == 0 || row_size < width) {
        return {};
    }
    StagingBufferRef readback =
        staging.Request(static_cast<size_t>(total_bytes), MemoryUsage::Download, true);
    Image& image = const_cast<Image&>(target);
    const D3D12_RESOURCE_STATES previous = image.State();
    image.Transition(D3D12_RESOURCE_STATE_COPY_SOURCE);
    const D3D12_TEXTURE_COPY_LOCATION src{
        .pResource = resource,
        .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
        .SubresourceIndex = 0,
    };
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT placed = footprint;
    placed.Offset = readback.offset;
    const D3D12_TEXTURE_COPY_LOCATION dst{
        .pResource = readback.buffer,
        .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
        .PlacedFootprint = placed,
    };
    scheduler.CommandList()->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    image.Transition(previous);
    scheduler.Finish();

    const size_t row_bytes = static_cast<size_t>(row_size);
    const u32 height = num_rows;
    std::vector<u8> now(row_bytes * height);
    for (u32 y = 0; y < height; ++y) {
        std::memcpy(now.data() + y * row_bytes,
                    readback.mapped_span.data() + static_cast<size_t>(y) * footprint.Footprint.RowPitch,
                    row_bytes);
    }
    staging.FreeDeferred(readback);

    std::vector<u8>& before = traced_depth[target.gpu_addr];
    if (before.size() != now.size()) {
        before = std::move(now);
        return " | depth first seen";
    }
    const size_t texel = row_bytes / width;
    constexpr u32 SCALE = 4;
    const u32 mask_width = Common::DivCeil(width, SCALE);
    const u32 mask_height = Common::DivCeil(height, SCALE);
    std::vector<u8> mask(static_cast<size_t>(mask_width) * mask_height);
    u64 changed = 0;
    u32 min_x = UINT32_MAX;
    u32 min_y = UINT32_MAX;
    u32 max_x = 0;
    u32 max_y = 0;
    for (u32 y = 0; y < height; ++y) {
        const u8* const a = before.data() + y * row_bytes;
        const u8* const b = now.data() + y * row_bytes;
        if (std::memcmp(a, b, row_bytes) == 0) {
            continue;
        }
        for (u32 x = 0; x < width; ++x) {
            if (std::memcmp(a + x * texel, b + x * texel, texel) == 0) {
                continue;
            }
            ++changed;
            min_x = std::min(min_x, x);
            min_y = std::min(min_y, y);
            max_x = std::max(max_x, x);
            max_y = std::max(max_y, y);
            mask[static_cast<size_t>(y / SCALE) * mask_width + x / SCALE] = 1;
        }
    }
    before.swap(now);
    if (changed == 0) {
        return " | depth unchanged";
    }
    // Bottom-up 32-bit BMP: white where the draw changed depth, black elsewhere.
    const u32 bmp_row = mask_width * 4;
    std::vector<u8> file(54 + static_cast<size_t>(bmp_row) * mask_height);
    const auto put32 = [&file](size_t offset, u32 value) { std::memcpy(&file[offset], &value, 4); };
    file[0] = 'B';
    file[1] = 'M';
    put32(2, static_cast<u32>(file.size()));
    put32(10, 54);
    put32(14, 40);
    put32(18, mask_width);
    put32(22, mask_height);
    file[26] = 1;
    file[28] = 32;
    put32(34, bmp_row * mask_height);
    for (u32 y = 0; y < mask_height; ++y) {
        u8* const out = file.data() + 54 + static_cast<size_t>(mask_height - 1 - y) * bmp_row;
        for (u32 x = 0; x < mask_width; ++x) {
            const u8 value = mask[static_cast<size_t>(y) * mask_width + x] ? 255 : 0;
            out[x * 4 + 0] = value;
            out[x * 4 + 1] = value;
            out[x * 4 + 2] = value;
            out[x * 4 + 3] = 255;
        }
    }
    const std::filesystem::path path = Common::FS::GetEdenPath(Common::FS::EdenPath::LogDir) /
                                       "trace" / fmt::format("{:03}_dz.bmp", trace_index - 1);
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(file.data()),
                 static_cast<std::streamsize>(file.size()));
    return fmt::format(" | depth changed {} texels in {},{}..{},{}", changed, min_x, min_y, max_x,
                       max_y);
}

std::optional<u64> RasterizerD3D12::DumpTarget(const Image& target, const std::string* bmp_name,
                                               u32 subresource) {
    ID3D12Resource* const resource = target.Handle();
    if (!resource) {
        return std::nullopt;
    }
    const D3D12_RESOURCE_DESC desc = resource->GetDesc();
    if (desc.SampleDesc.Count > 1) {
        return std::nullopt; // MSAA cannot be copied to a buffer
    }
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    u64 total_bytes = 0;
    ID3D12Device* device = nullptr;
    resource->GetDevice(IID_PPV_ARGS(&device));
    device->GetCopyableFootprints(&desc, subresource, 1, 0, &footprint, nullptr, nullptr,
                                  &total_bytes);
    device->Release();
    // The depth plane of D24S8 copies as R32 with the depth in its low 24 bits. R16_TYPELESS
    // holds halves or, for D16 and R16 images, normalized integers.
    const bool unorm16 = target.info.format == VideoCore::Surface::PixelFormat::D16_UNORM ||
                         target.info.format == VideoCore::Surface::PixelFormat::R16_UNORM;
    DXGI_FORMAT format = footprint.Footprint.Format;
    if (desc.Format == DXGI_FORMAT_R24G8_TYPELESS) {
        format = DXGI_FORMAT_R24G8_TYPELESS;
    } else if (unorm16) {
        format = DXGI_FORMAT_R16_UNORM;
    }
    std::array<float, 4> probe{};
    const std::array<u8, 8> zeros{};
    if (!ReadTexel(format, zeros.data(), probe)) {
        if (bmp_name) {
            LOG_INFO(Render, "D3D12 trace dump {}: DXGI format {} not decoded", *bmp_name,
                     static_cast<u32>(format));
        }
        return std::nullopt;
    }

    // Diagnostics only: the copy needs the image in COPY_SOURCE; its next use transitions it
    // back. Waiting here keeps one frame of dumps from holding a readback each.
    Image& image = const_cast<Image&>(target);
    const D3D12_RESOURCE_STATES previous = image.State();
    const u64 needed = static_cast<u64>(footprint.Footprint.RowPitch) *
                           (footprint.Footprint.Height - 1) +
                       static_cast<u64>(footprint.Footprint.Width) * DumpTexelBytes(format);
    if (total_bytes == 0 || total_bytes == UINT64_MAX || needed > total_bytes) {
        LOG_WARNING(Render, "D3D12 trace: cannot dump subresource {} of {} (footprint {} bytes)",
                    subresource, target.info.format, total_bytes);
        return std::nullopt;
    }
    StagingBufferRef readback = staging.Request(total_bytes, MemoryUsage::Download, true);
    if (readback.mapped_span.size() < needed) {
        LOG_WARNING(Render, "D3D12 trace: readback of {} bytes is smaller than {}",
                    readback.mapped_span.size(), needed);
        staging.FreeDeferred(readback);
        return std::nullopt;
    }
    image.Transition(D3D12_RESOURCE_STATE_COPY_SOURCE);
    const D3D12_TEXTURE_COPY_LOCATION src{
        .pResource = resource,
        .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
        .SubresourceIndex = subresource,
    };
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT placed = footprint;
    placed.Offset = readback.offset;
    const D3D12_TEXTURE_COPY_LOCATION dst{
        .pResource = readback.buffer,
        .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
        .PlacedFootprint = placed,
    };
    scheduler.CommandList()->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    image.Transition(previous);
    scheduler.Finish();

    const u32 width = static_cast<u32>(footprint.Footprint.Width);
    const u32 height = footprint.Footprint.Height;
    const u32 texel_bytes = DumpTexelBytes(format);
    const u32 row_bytes = width * 4;
    std::vector<u8> file;
    if (bmp_name) {
        file.resize(54 + static_cast<size_t>(row_bytes) * height);
        const auto put32 = [&file](size_t offset, u32 value) {
            std::memcpy(&file[offset], &value, 4);
        };
        file[0] = 'B';
        file[1] = 'M';
        put32(2, static_cast<u32>(file.size()));
        put32(10, 54);
        put32(14, 40);
        put32(18, width);
        put32(22, height);
        file[26] = 1;
        file[28] = 32;
        put32(34, row_bytes * height);
    }
    std::array<float, 4> min_value;
    std::array<float, 4> max_value;
    min_value.fill(std::numeric_limits<float>::infinity());
    max_value.fill(-std::numeric_limits<float>::infinity());
    std::array<double, 4> sum{};
    u64 non_finite = 0;
    for (u32 y = 0; y < height; ++y) {
        const u8* const row = readback.mapped_span.data() +
                              static_cast<size_t>(y) * footprint.Footprint.RowPitch;
        u8* const out = bmp_name ? file.data() + 54 + static_cast<size_t>(height - 1 - y) * row_bytes
                                 : nullptr;
        for (u32 x = 0; x < width; ++x) {
            std::array<float, 4> rgba{};
            ReadTexel(format, row + static_cast<size_t>(x) * texel_bytes, rgba);
            bool finite = true;
            for (size_t c = 0; c < 4; ++c) {
                if (!std::isfinite(rgba[c])) {
                    finite = false;
                    rgba[c] = 1.0f;
                }
                min_value[c] = std::min(min_value[c], rgba[c]);
                max_value[c] = std::max(max_value[c], rgba[c]);
                sum[c] += rgba[c];
            }
            non_finite += finite ? 0 : 1;
            if (out) {
                // As stored, clamped to [0, 1]; NaN/Inf texels in magenta.
                const auto to_byte = [](float value) {
                    return static_cast<u8>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
                };
                out[x * 4 + 0] = finite ? to_byte(rgba[2]) : 255;
                out[x * 4 + 1] = finite ? to_byte(rgba[1]) : 0;
                out[x * 4 + 2] = finite ? to_byte(rgba[0]) : 255;
                out[x * 4 + 3] = 255;
            }
        }
    }
    staging.FreeDeferred(readback);
    if (bmp_name) {
        const double count = static_cast<double>(width) * height;
        LOG_INFO(Render,
                 "D3D12 trace dump {}: min {:.4g} {:.4g} {:.4g} {:.4g} max {:.4g} {:.4g} {:.4g} "
                 "{:.4g} mean {:.4g} {:.4g} {:.4g} {:.4g} non-finite texels {}",
                 *bmp_name, min_value[0], min_value[1], min_value[2], min_value[3], max_value[0],
                 max_value[1], max_value[2], max_value[3], sum[0] / count, sum[1] / count,
                 sum[2] / count, sum[3] / count, non_finite);
        const std::filesystem::path path =
            Common::FS::GetEdenPath(Common::FS::EdenPath::LogDir) / "trace" / (*bmp_name + ".bmp");
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream.write(reinterpret_cast<const char*>(file.data()),
                     static_cast<std::streamsize>(file.size()));
    }
    return non_finite;
}

void RasterizerD3D12::TraceDraw(std::string_view what, const GraphicsPipeline* pipeline,
                                const Framebuffer* framebuffer,
                                std::span<const VideoCommon::ImageViewId> views, u32 vertices,
                                u32 instances) {
    const auto describe = [](const Image* image) {
        if (!image) {
            return std::string("none");
        }
        const auto& info = image->info;
        return fmt::format("{} {}x{}x{} L{} @{:x} flags {:x}", info.format, info.size.width,
                           info.size.height,
                           info.type == VideoCommon::ImageType::e3D ? info.size.depth
                                                                     : info.resources.layers,
                           info.resources.levels, image->gpu_addr,
                           static_cast<u32>(image->flags));
    };
    // The previous framebuffer is done (for now): keep what it holds. The copy lands after the
    // draw being traced, which only matters when both framebuffers share an image.
    if (framebuffer && trace_dumps) {
        std::array<VideoCommon::ImageId, VideoCommon::NUM_RT + 1> targets{};
        for (size_t index = 0; index < VideoCommon::NUM_RT; ++index) {
            targets[index] = framebuffer->ColorImageId(index);
        }
        targets[VideoCommon::NUM_RT] = framebuffer->DepthImageId();
        if (targets != traced_targets) {
            DumpTargets(traced_targets);
            traced_targets = targets;
            if (framebuffer->Images()) {
                traced_images = framebuffer->Images();
            }
        }
    }
    trace_non_finite_grew = false;
    std::string line = fmt::format("D3D12 trace #{}: {}", trace_index++, what);
    if (vertices != 0 || instances != 0) {
        line += fmt::format(" ({} vertices x{})", vertices, instances);
    }
    if (pipeline) {
        const GraphicsPipelineCacheKey& key = pipeline->Key();
        line += fmt::format(" | VS {:016x} PS {:016x} blend0 {:08x}", key.unique_hashes[1],
                            key.unique_hashes[5], key.state.attachments[0].raw);
        const auto& dynamic = key.state.dynamic_state;
        line += fmt::format(" depth test {} write {} func {} stencil {} cull {}",
                            dynamic.depth_test_enable.Value(), dynamic.depth_write_enable.Value(),
                            static_cast<u32>(dynamic.DepthTestFunc()),
                            dynamic.stencil_enable.Value(), dynamic.cull_enable.Value());
        line += fmt::format(" a2c {} alpha test {} ref {:.4g} early z {} msaa {}",
                            key.state.alpha_to_coverage_enabled.Value(),
                            key.state.alpha_test_func.Value(),
                            std::bit_cast<float>(key.state.alpha_test_ref),
                            key.state.early_z.Value(),
                            static_cast<u32>(key.state.msaa_mode.Value()));
    }
    if (framebuffer) {
        const VideoCommon::Extent2D extent = framebuffer->Extent();
        line += fmt::format(" | fb {}x{}", extent.width, extent.height);
        for (size_t index = 0; index < VideoCommon::NUM_RT; ++index) {
            if (const Image* const image = framebuffer->ColorImage(index)) {
                line += fmt::format(" rt{} [{}]", index, describe(image));
            }
        }
        if (const u32 missing = framebuffer->MissingColorMask()) {
            line += fmt::format(" MISSING RTV mask {:x}", missing);
        }
        if (const Image* const image = framebuffer->DepthImage()) {
            line += fmt::format(" ds [{}]", describe(image));
        }
        // Where NaN/Inf enter the frame: rt0 after every traced draw.
        if (const Image* const image = framebuffer->ColorImage(0); image && pipeline && trace_dumps) {
            const VideoCommon::SubresourceBase base = framebuffer->ColorBase(0);
            if (const std::optional<u64> non_finite =
                    DumpTarget(*image, nullptr, image->Subresource(base.level, base.layer))) {
                line += fmt::format(" | rt0 L{}/{} non-finite {}", base.level, base.layer,
                                    *non_finite);
                u64& previous = traced_non_finite[image->gpu_addr];
                trace_non_finite_grew = *non_finite > previous;
                previous = *non_finite;
            }
        }
        if (const Image* const depth = framebuffer->DepthImage(); depth && pipeline && trace_dumps) {
            line += TraceDepthChanges(*depth);
        }
    }
    for (const VideoCommon::ImageViewId id : views) {
        const ImageView& view = texture_cache.GetImageView(id);
        if (const Image* const image = view.SourceImage();
            image && trace_dumps && traced_textures.insert(image->gpu_addr).second) {
            DumpTextureNonFinite(*image);
            CheckTracedTexture(*image);
        }
        line += fmt::format(" | tex {} {}x{} type {} layers {}+{} of [{}]", view.format,
                            view.size.width, view.size.height, static_cast<u32>(view.type),
                            view.range.base.layer, view.range.extent.layers,
                            describe(view.SourceImage()));
    }
    LOG_INFO(Render, "{}", line);
}
Tegra::Engines::AccelerateDMAInterface& RasterizerD3D12::AccessAccelerateDMA() { return accelerate_dma; }
void RasterizerD3D12::AccelerateInlineToMemory(GPUVAddr address, size_t size,
                                               std::span<const u8> memory) {
    const auto cpu_address = gpu_memory->GpuToCpuAddress(address);
    if (!cpu_address) return gpu_memory->WriteBlock(address, memory.data(), size);
    gpu_memory->WriteBlockUnsafe(address, memory.data(), size);
    { std::scoped_lock lock{buffer_cache.mutex};
      if (!buffer_cache.InlineMemory(*cpu_address, size, memory)) buffer_cache.WriteMemory(*cpu_address, size); }
    { std::scoped_lock lock{texture_cache.mutex}; texture_cache.WriteMemory(*cpu_address, size); }
    pipeline_cache.InvalidateRegion(*cpu_address, size);
    query_cache.InvalidateRegion(*cpu_address, size);
}
void RasterizerD3D12::LoadDiskResources(u64 title_id, std::stop_token stop_loading,
                                        const VideoCore::DiskResourceLoadCallback& callback) {
    pipeline_cache.LoadDiskResources(title_id, stop_loading, callback);
}
void RasterizerD3D12::InitializeChannel(Tegra::Control::ChannelState& channel) {
    // The dirty flags the generic caches rely on (render targets, shaders, vertex and index
    // buffers, descriptors) are only raised by register writes listed in these tables; without
    // them a new render target or program is never picked up (Vulkan's StateTracker::SetupTables
    // does the same, plus its own flags).
    state_tracker.SetupTables(channel);
    CreateChannel(channel);
    { std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
      texture_cache.CreateChannel(channel); buffer_cache.CreateChannel(channel); }
    pipeline_cache.CreateChannel(channel);
    query_cache.CreateChannel(channel);
}
void RasterizerD3D12::BindChannel(Tegra::Control::ChannelState& channel) {
    BindToChannel(channel.bind_id);
    state_tracker.ChangeChannel(channel);
    channel_bound = true;
    state_tracker.InvalidateState();
    state_invalidation_pending = false;
    InvalidateGraphicsState();
    { std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
      texture_cache.BindToChannel(channel.bind_id); buffer_cache.BindToChannel(channel.bind_id); }
    pipeline_cache.BindToChannel(channel.bind_id);
    query_cache.BindToChannel(channel.bind_id);
}
void RasterizerD3D12::ReleaseChannel(s32 id) {
    EraseChannel(id);
    // EraseChannel clears current_channel_id when the active channel goes away. A later scheduler
    // reset (notably during shutdown) must not dereference StateTracker::flags from that channel.
    channel_bound = current_channel_id != UNSET_CHANNEL;
    if (!channel_bound) {
        state_invalidation_pending = false;
    }
    { std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
      texture_cache.EraseChannel(id); buffer_cache.EraseChannel(id); }
    pipeline_cache.EraseChannel(id);
    query_cache.EraseChannel(id);
}

AccelerateDMA::AccelerateDMA(BufferCache& buffers, TextureCache& textures)
    : buffer_cache{buffers}, texture_cache{textures} {}
bool AccelerateDMA::BufferCopy(GPUVAddr src, GPUVAddr dst, u64 amount) {
    std::scoped_lock lock{buffer_cache.mutex}; return buffer_cache.DMACopy(src, dst, amount);
}
bool AccelerateDMA::BufferClear(GPUVAddr address, u64 amount, u32 value) {
    std::scoped_lock lock{buffer_cache.mutex}; return buffer_cache.DMAClear(address, amount, value);
}
template <bool IS_UPLOAD>
bool AccelerateDMA::BufferImageCopy(const Tegra::DMA::ImageCopy& info,
                                    const Tegra::DMA::BufferOperand& buffer_operand,
                                    const Tegra::DMA::ImageOperand& image_operand) {
    std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
    const auto image_id = texture_cache.DmaImageId(image_operand, IS_UPLOAD);
    if (image_id == VideoCommon::NULL_IMAGE_ID) return false;
    const u32 size = static_cast<u32>(buffer_operand.pitch * buffer_operand.height);
    constexpr auto sync = VideoCommon::ObtainBufferSynchronize::FullSynchronize;
    const auto op = IS_UPLOAD ? VideoCommon::ObtainBufferOperation::DoNothing
                              : VideoCommon::ObtainBufferOperation::MarkAsWritten;
    const auto [buffer, offset] = buffer_cache.ObtainBuffer(buffer_operand.address, size, sync, op);
    // The texture code copies to and from the raw resource and expects it in COMMON (it relies
    // on implicit promotion), so end any state a draw gave it in this command list.
    buffer->Transition(D3D12_RESOURCE_STATE_COMMON);
    const auto [image, copy] = texture_cache.DmaBufferImageCopy(
        info, buffer_operand, image_operand, image_id, IS_UPLOAD);
    const std::span copies{&copy, 1};
    if constexpr (IS_UPLOAD) {
        texture_cache.PrepareImage(image_id, true, false);
        image->UploadMemory(buffer->Handle(), offset, copies);
    } else {
        if (offset % VideoCore::Surface::BytesPerBlock(image->info.format)) return false;
        texture_cache.DownloadImageIntoBuffer(image, buffer->Handle(), offset, copies,
                                              buffer_operand.address, size);
    }
    return true;
}
bool AccelerateDMA::ImageToBuffer(const Tegra::DMA::ImageCopy& i,
                                  const Tegra::DMA::ImageOperand& image,
                                  const Tegra::DMA::BufferOperand& buffer) {
    return BufferImageCopy<false>(i, buffer, image);
}
bool AccelerateDMA::BufferToImage(const Tegra::DMA::ImageCopy& i,
                                  const Tegra::DMA::BufferOperand& buffer,
                                  const Tegra::DMA::ImageOperand& image) {
    return BufferImageCopy<true>(i, buffer, image);
}

} // namespace D3D12
