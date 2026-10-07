// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>

#include "common/bug_tracker.h"
#include "common/scope_exit.h"
#include "video_core/gpu.h"
#include "video_core/memory_manager.h"
#include "video_core/perf_counters.h"
#include "video_core/renderer_d3d12/d3d12_log.h"
#include "video_core/renderer_d3d12/d3d12_rasterizer.h"
#include "video_core/renderer_d3d12/d3d12_scheduler.h"

// Clears and DrawTexture.

namespace D3D12 {

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
        BUG_TRACK_KEY(DrawSkipped, static_cast<u64>(format) << 32 | static_cast<u32>(texture.format),
                      "DrawTexture with integer formats skipped (target {}, texture {})", format,
                      texture.format);
        WarnOnceLog(logged_draw_texture, "DrawTexture with integer formats is skipped");
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
    // As in Vulkan, the clear covers layers [layer, layer + layer_count) of the attachments. The
    // framebuffer's views cover all their layers; a narrower clear gets views of its own, freed
    // once recorded (RTVs and DSVs are read when the clear is recorded).
    const u32 first_layer = regs.clear_surface.layer;
    const u32 color_attachment = regs.clear_surface.RT;
    D3D12_CPU_DESCRIPTOR_HANDLE color_target{};
    D3D12_CPU_DESCRIPTOR_HANDLE depth_target = framebuffer->DepthTarget();
    D3D12_CPU_DESCRIPTOR_HANDLE color_layers{};
    D3D12_CPU_DESCRIPTOR_HANDLE depth_layers{};
    SCOPE_EXIT {
        texture_runtime.FreeLayerTarget(color_layers, false);
        texture_runtime.FreeLayerTarget(depth_layers, true);
    };
    bool layers_lost = false;
    if (use_color && framebuffer->HasColor(color_attachment)) {
        color_target = framebuffer->ColorTargets()[color_attachment];
        if (first_layer != 0 || layer_count != framebuffer->ColorLayers(color_attachment)) {
            color_layers = texture_runtime.LayerTarget(*framebuffer, color_attachment,
                                                       first_layer, layer_count);
            color_target = color_layers.ptr ? color_layers : color_target;
            layers_lost |= !color_layers.ptr;
        }
    }
    if ((use_depth || use_stencil) && depth_target.ptr &&
        (first_layer != 0 || layer_count != framebuffer->DepthLayers())) {
        depth_layers = texture_runtime.LayerTarget(*framebuffer, VideoCommon::NUM_RT, first_layer,
                                                   layer_count);
        depth_target = depth_layers.ptr ? depth_layers : depth_target;
        layers_lost |= !depth_layers.ptr;
    }
    if (layers_lost) {
        BUG_TRACK(CopySkipped, "layered clear (layer {}, {} layers) clears every layer of the view",
                  first_layer, layer_count);
        WarnOnceLog(logged_layer_clear, "a layered clear cleared every layer of the view");
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

    if (color_target.ptr) {
        const bool full_mask = regs.clear_surface.R && regs.clear_surface.G &&
                               regs.clear_surface.B && regs.clear_surface.A;
        using namespace VideoCore::Surface;
        const PixelFormat format =
            PixelFormatFromRenderTargetFormat(regs.rt[color_attachment].format);
        if (!full_mask) {
            // Drawn with the write mask in the PSO (Vulkan uses the blend constant instead).
            if (IsPixelFormatInteger(format)) {
                BUG_TRACK_KEY(CopySkipped, static_cast<u64>(format),
                              "masked clear of an integer render target ({}) skipped", format);
                WarnOnceLog(logged_integer_clear,
                            "masked clears of integer render targets are skipped");
            } else {
                const u8 mask = static_cast<u8>(regs.clear_surface.R | regs.clear_surface.G << 1 |
                                                regs.clear_surface.B << 2 |
                                                regs.clear_surface.A << 3);
                blit_helper.ClearColor({color_target, framebuffer->ColorFormat(color_attachment),
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
            cmd->ClearRenderTargetView(color_target, color.data(), 1, &rect);
        }
    }
    const D3D12_CPU_DESCRIPTOR_HANDLE depth = depth_target;
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

} // namespace D3D12
