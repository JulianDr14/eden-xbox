// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cstring>
#include <optional>

#include <spirv_to_dxil.h>

#include "common/alignment.h"
#include "common/bug_tracker.h"
#include "common/logging.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "video_core/control/channel_state.h"
#include "video_core/engines/kepler_compute.h"
#include "video_core/framebuffer_config.h"
#include "video_core/gpu.h"
#include "video_core/memory_manager.h"
#include "video_core/perf_counters.h"
#include "video_core/renderer_d3d12/d3d12_rasterizer.h"
#include "video_core/renderer_d3d12/d3d12_scheduler.h"

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
                         sampler_heap, scheduler},
                        bindings);
    buffer_runtime.SetTraceBuffers(nullptr);
    if (!pipeline->Handle()) {
        BUG_TRACK_KEY(DrawSkipped, reinterpret_cast<std::uintptr_t>(pipeline),
                      "draw skipped: the pipeline was rejected (VS {:016x} PS {:016x})",
                      pipeline->Key().unique_hashes[1], pipeline->Key().unique_hashes[5]);
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
                         descriptor_queue, sampler_heap, scheduler},
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
            BUG_TRACK(DrawSkipped, "dispatch of {}x{}x{} groups exceeds the D3D12 limit, skipped",
                      dim[0], dim[1], dim[2]);
            const Common::BugTracker::TapMute bug_tracker_mute;
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
    MarkGpuCommands(cmd, "dispatch CS", pipeline->UniqueHash(), 0);
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
    if (layout.IntegerSamplerIndex() != PipelineLayout::NO_TABLE && bindings.integer_samplers) {
        cmd->SetComputeRootConstantBufferView(layout.IntegerSamplerIndex(),
                                              bindings.integer_samplers);
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
    // A loading frame may upload more textures than the staging stream holds within one command
    // list (151 MiB in Mario Wonder; the stream is 256 MiB, and the memory guard shrinks it down to
    // nothing under pressure), and the stream only reuses submitted regions. The
    // reset callback invalidates cached state, so submitting between draws is safe.
    constexpr u64 UPLOAD_SUBMIT_BYTES = 32ULL << 20;
    if (staging.PendingUploadBytes() < UPLOAD_SUBMIT_BYTES) {
        return;
    }
    draw_counter = 0;
    scheduler.Flush();
}
void RasterizerD3D12::TickFrame() {
    Common::BugTracker::TickFrame();
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

} // namespace D3D12
