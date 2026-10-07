// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

#include <spirv_to_dxil.h>

#include "common/bug_tracker.h"
#include "video_core/renderer_d3d12/d3d12_log.h"
#include "video_core/renderer_d3d12/d3d12_maxwell_to_d3d12.h"
#include "video_core/renderer_d3d12/d3d12_rasterizer.h"
#include "video_core/renderer_d3d12/d3d12_scheduler.h"

// The graphics state of a draw: render targets, viewports, scissors, dynamic state and
// root arguments, set only when they differ from what the command list holds.

namespace D3D12 {

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
        command_state.root_args = {};
    }
    scheduler.SetPipelineState(pipeline.Handle());
    MarkGpuCommands(cmd, "draw VS/PS", pipeline.Key().unique_hashes[1],
                    pipeline.Key().unique_hashes[5]);

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
    if (regs.stencil_two_side_enable != 0 && regs.stencil_back_ref != regs.stencil_front_ref) {
        BUG_TRACK(UnsupportedState, "different front ({}) and back ({}) stencil references; the "
                                    "front one is used",
                  regs.stencil_front_ref, regs.stencil_back_ref);
        WarnOnceLog(logged_stencil_ref,
                    "different front and back stencil references; using the front");
    }

    SetGraphicsRootArguments(cmd, layout, bindings, params);

    const D3D12_PRIMITIVE_TOPOLOGY d3d_topology =
        MaxwellToD3D12::PrimitiveTopology(topology, regs.patch_vertices);
    if (!command_state.valid || command_state.topology != d3d_topology) {
        cmd->IASetPrimitiveTopology(d3d_topology);
        command_state.topology = d3d_topology;
    }
    buffer_runtime.ApplyGeometry(cmd);
    command_state.valid = true;
}

void RasterizerD3D12::SetGraphicsRootArguments(ID3D12GraphicsCommandList* cmd,
                                               const PipelineLayout& layout,
                                               const PipelineBindings& bindings,
                                               const DrawParams& params) {
    // Consecutive draws of one pipeline mostly repeat their root arguments: only changes are set.
    auto& cached = command_state.root_args;
    if (!cached.valid || cached.push_constants != bindings.push_constants) {
        cmd->SetGraphicsRoot32BitConstants(PipelineLayout::PUSH_CONSTANTS_INDEX,
                                           PUSH_CONSTANT_WORDS, bindings.push_constants.data(), 0);
        cached.push_constants = bindings.push_constants;
    }
    dxil_spirv_vertex_runtime_data runtime_data{};
    runtime_data.first_vertex = params.runtime_first_vertex;
    runtime_data.base_instance = params.base_instance;
    runtime_data.is_indexed_draw = params.is_indexed;
    runtime_data.yz_flip_mask = command_state.viewport.yz_flip_mask;
    runtime_data.viewport_width = command_state.viewport.width;
    runtime_data.viewport_height = command_state.viewport.height;
    constexpr size_t runtime_size = sizeof(runtime_data) / sizeof(u32);
    static_assert(runtime_size <= std::tuple_size_v<decltype(cached.runtime_words)>);
    std::array<u32, std::tuple_size_v<decltype(cached.runtime_words)>> runtime_words{};
    std::memcpy(runtime_words.data(), &runtime_data, sizeof(runtime_data));
    if (!cached.valid || cached.runtime_words != runtime_words) {
        cmd->SetGraphicsRoot32BitConstants(
            PipelineLayout::RUNTIME_DATA_INDEX,
            std::min<UINT>(layout.RuntimeDataWords(), static_cast<UINT>(runtime_size)),
            runtime_words.data(), 0);
        cached.runtime_words = runtime_words;
    }
    if (layout.ResourceTableIndex() != PipelineLayout::NO_TABLE &&
        (!cached.valid || cached.resource_table != bindings.resource_table.ptr)) {
        cmd->SetGraphicsRootDescriptorTable(layout.ResourceTableIndex(), bindings.resource_table);
        cached.resource_table = bindings.resource_table.ptr;
    }
    if (layout.SamplerTableIndex() != PipelineLayout::NO_TABLE &&
        (!cached.valid || cached.sampler_table != bindings.sampler_table.ptr)) {
        cmd->SetGraphicsRootDescriptorTable(layout.SamplerTableIndex(), bindings.sampler_table);
        cached.sampler_table = bindings.sampler_table.ptr;
    }
    if (layout.IntegerSamplerIndex() != PipelineLayout::NO_TABLE && bindings.integer_samplers &&
        (!cached.valid || cached.integer_samplers != bindings.integer_samplers)) {
        cmd->SetGraphicsRootConstantBufferView(layout.IntegerSamplerIndex(),
                                               bindings.integer_samplers);
        cached.integer_samplers = bindings.integer_samplers;
    }
    cached.valid = true;
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

} // namespace D3D12
