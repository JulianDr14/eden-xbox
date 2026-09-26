// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <optional>

#include <spirv_to_dxil.h>

#include "common/alignment.h"
#include "common/logging.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "video_core/control/channel_state.h"
#include "video_core/dirty_flags.h"
#include "video_core/gpu.h"
#include "video_core/memory_manager.h"
#include "video_core/framebuffer_config.h"
#include "video_core/renderer_d3d12/d3d12_maxwell_to_d3d12.h"
#include "video_core/renderer_d3d12/d3d12_rasterizer.h"
#include "video_core/renderer_d3d12/d3d12_scheduler.h"
#include "video_core/surface.h"

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
                                 DescriptorRing& descriptor_ring_, SamplerHeap& sampler_heap_)
    : gpu{gpu_}, device_memory{device_memory_}, scheduler{scheduler_},
      buffer_runtime{buffer_runtime_}, descriptor_ring{descriptor_ring_},
      sampler_heap{sampler_heap_}, descriptor_queue{device.Get(), descriptor_ring_},
      buffer_cache{device_memory_, buffer_runtime_}, texture_cache{texture_runtime, device_memory_},
      pipeline_cache{device_memory_, device, compiler, texture_runtime, gpu_.ShaderNotify()},
      query_cache{*this, device_memory_, device, scheduler_},
      accelerate_dma{buffer_cache, texture_cache},
      fence_manager{*this, gpu_, texture_cache, buffer_cache, query_cache, scheduler_} {
    buffer_runtime.SetDescriptorQueue(&descriptor_queue);
    LOG_INFO(Render, "D3D12: rasterizer active (draws, clears, caches, DMA and pipelines)");
}

RasterizerD3D12::~RasterizerD3D12() {
    buffer_runtime.SetDescriptorQueue(nullptr);
}

void RasterizerD3D12::UnsupportedDraw(const char* operation) {
    if (!logged_phase4_draw) {
        LOG_WARNING(Render, "D3D12: guest {} skipped until phase 4.4", operation);
        logged_phase4_draw = true;
    }
}

void RasterizerD3D12::Draw(bool is_indexed, u32 instance_count) {
    SCOPE_EXIT {
        gpu.TickWork();
    };
    gpu_memory->FlushCaching();

    GraphicsPipeline* const pipeline = pipeline_cache.CurrentGraphicsPipeline();
    if (!pipeline) {
        return;
    }
    std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};

    // Every draw records its whole state (see RecordDraw), vertex buffers included: mark them
    // dirty so the buffer cache binds them again, as Vulkan's state tracker does for each new
    // command buffer.
    auto& flags = maxwell3d->dirty.flags;
    flags[VideoCommon::Dirty::VertexBuffers] = true;
    for (u32 index = 0; index < Maxwell::NumVertexArrays; ++index) {
        flags[VideoCommon::Dirty::VertexBuffer0 + index] = true;
    }

    PipelineBindings bindings;
    pipeline->Configure(is_indexed,
                        {*maxwell3d, *gpu_memory, buffer_cache, texture_cache, descriptor_queue,
                         sampler_heap},
                        bindings);
    if (!pipeline->Handle()) {
        return; // D3D12 rejected the PSO (logged when it was built)
    }
    const Framebuffer* const framebuffer = texture_cache.GetFramebuffer();
    framebuffer->PrepareAttachments();

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
        return;
    }
    RecordDraw(*pipeline, bindings, *framebuffer, params, draw_state.topology);
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
    const auto& regs = maxwell3d->regs;
    const PipelineLayout& layout = pipeline.Layout();
    ID3D12GraphicsCommandList* const cmd = scheduler.CommandList();

    ID3D12DescriptorHeap* const heaps[] = {descriptor_ring.Heap(), sampler_heap.Heap()};
    cmd->SetDescriptorHeaps(2, heaps);
    cmd->SetGraphicsRootSignature(layout.Handle());
    cmd->SetPipelineState(pipeline.Handle());

    const auto color_targets = framebuffer.ColorTargets();
    const D3D12_CPU_DESCRIPTOR_HANDLE depth = framebuffer.DepthTarget();
    cmd->OMSetRenderTargets(static_cast<UINT>(color_targets.size()),
                            color_targets.empty() ? nullptr : color_targets.data(), FALSE,
                            depth.ptr ? &depth : nullptr);

    ViewportState viewports = UpdateViewports(cmd);
    UpdateScissors(cmd);
    const float blend_factor[4] = {regs.blend_color.r, regs.blend_color.g, regs.blend_color.b,
                                   regs.blend_color.a};
    cmd->OMSetBlendFactor(blend_factor);
    // D3D12 has a single reference for both faces.
    cmd->OMSetStencilRef(regs.stencil_front_ref);
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
    runtime_data.yz_flip_mask = viewports.yz_flip_mask;
    runtime_data.viewport_width = viewports.width;
    runtime_data.viewport_height = viewports.height;
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

    cmd->IASetPrimitiveTopology(MaxwellToD3D12::PrimitiveTopology(topology, regs.patch_vertices));
    buffer_runtime.ApplyGeometry(cmd);

    if (params.is_indexed) {
        cmd->DrawIndexedInstanced(params.num_vertices, params.num_instances, params.first_index,
                                  static_cast<INT>(params.base_vertex), params.base_instance);
    } else {
        cmd->DrawInstanced(params.num_vertices, params.num_instances, params.base_vertex,
                           params.base_instance);
    }
}

RasterizerD3D12::ViewportState RasterizerD3D12::UpdateViewports(ID3D12GraphicsCommandList* cmd) {
    const auto& regs = maxwell3d->regs;
    std::array<D3D12_VIEWPORT, Maxwell::NumViewports> viewports{};
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
    cmd->RSSetViewports(static_cast<UINT>(viewports.size()), viewports.data());
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

void RasterizerD3D12::DrawTexture() { UnsupportedDraw("draw texture"); }

void RasterizerD3D12::Clear(u32 layer_count) {
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
    if (rect.right <= rect.left || rect.bottom <= rect.top) {
        return;
    }
    ID3D12GraphicsCommandList* const cmd = scheduler.CommandList();

    const u32 color_attachment = regs.clear_surface.RT;
    if (use_color && framebuffer->HasColor(color_attachment)) {
        const bool full_mask = regs.clear_surface.R && regs.clear_surface.G &&
                               regs.clear_surface.B && regs.clear_surface.A;
        if (!full_mask) {
            // Needs the clear-by-draw helper (phase 4.4).
            if (!logged_masked_clear) {
                LOG_WARNING(Render, "D3D12: color clears with a partial mask are skipped");
                logged_masked_clear = true;
            }
        } else {
            using namespace VideoCore::Surface;
            const PixelFormat format =
                PixelFormatFromRenderTargetFormat(regs.rt[color_attachment].format);
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
        if (use_stencil && framebuffer->HasStencil()) {
            if (regs.stencil_front_mask != 0xFF && regs.stencil_front_mask != 0 &&
                !logged_masked_clear) {
                LOG_WARNING(Render, "D3D12: masked stencil clears write every bit");
                logged_masked_clear = true;
            }
            clear_flags |= D3D12_CLEAR_FLAG_STENCIL;
        }
        if (clear_flags != 0) {
            cmd->ClearDepthStencilView(depth, clear_flags, regs.clear_depth,
                                       static_cast<u8>(regs.clear_stencil), 1, &rect);
        }
    }
}

void RasterizerD3D12::DispatchCompute() {
    [[maybe_unused]] ComputePipeline* const pipeline = pipeline_cache.CurrentComputePipeline();
    UnsupportedDraw("compute dispatch");
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
void RasterizerD3D12::WaitForIdle() { scheduler.Finish(); }
void RasterizerD3D12::FragmentBarrier() { scheduler.Flush(); }
void RasterizerD3D12::TiledCacheBarrier() { scheduler.Flush(); }
void RasterizerD3D12::FlushCommands() { scheduler.Flush(); }
void RasterizerD3D12::TickFrame() {
    fence_manager.TickFrame();
    { std::scoped_lock lock{texture_cache.mutex}; texture_cache.TickFrame(); }
    { std::scoped_lock lock{buffer_cache.mutex}; buffer_cache.TickFrame(); }
}
bool RasterizerD3D12::AccelerateSurfaceCopy(const Tegra::Engines::Fermi2D::Surface& src,
                                            const Tegra::Engines::Fermi2D::Surface& dst,
                                            const Tegra::Engines::Fermi2D::Config& config) {
    std::scoped_lock lock{texture_cache.mutex};
    return texture_cache.BlitImage(dst, src, config);
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
    VideoCommon::Dirty::SetupDirtyFlags(channel.payload->maxwell_3d.dirty.tables);
    CreateChannel(channel);
    { std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
      texture_cache.CreateChannel(channel); buffer_cache.CreateChannel(channel); }
    pipeline_cache.CreateChannel(channel);
    query_cache.CreateChannel(channel);
}
void RasterizerD3D12::BindChannel(Tegra::Control::ChannelState& channel) {
    BindToChannel(channel.bind_id);
    { std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
      texture_cache.BindToChannel(channel.bind_id); buffer_cache.BindToChannel(channel.bind_id); }
    pipeline_cache.BindToChannel(channel.bind_id);
    query_cache.BindToChannel(channel.bind_id);
}
void RasterizerD3D12::ReleaseChannel(s32 id) {
    EraseChannel(id);
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
