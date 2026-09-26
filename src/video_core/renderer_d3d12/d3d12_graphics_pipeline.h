// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <type_traits>
#include <vector>

#include "common/thread_worker.h"
#include "shader_recompiler/shader_info.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/renderer_d3d12/d3d12_buffer_cache.h"
#include "video_core/renderer_d3d12/d3d12_device.h"
#include "video_core/renderer_d3d12/d3d12_root_signature.h"
#include "video_core/renderer_d3d12/d3d12_texture_cache.h"
#include "video_core/renderer_vulkan/fixed_pipeline_state.h"

namespace VideoCore {
class ShaderNotify;
}

namespace Tegra {
class MemoryManager;
}

namespace D3D12 {

class GuestDescriptorQueue;
class SamplerHeap;

using Maxwell = Tegra::Engines::Maxwell3D::Regs;

/// Vulkan's pipeline key (the fixed state only depends on Maxwell, and with every dynamic feature
/// off it holds everything a PSO needs) plus the state D3D12 bakes into the PSO but Vulkan sets
/// dynamically. The extra fields are zero when their feature is disabled, so they do not split
/// pipelines that would be identical.
struct GraphicsPipelineCacheKey {
    std::array<u64, 6> unique_hashes;
    Vulkan::FixedPipelineState state;
    u32 depth_bias;              ///< f32 bits, Maxwell units (twice D3D12's)
    u32 depth_bias_clamp;        ///< f32 bits
    u32 slope_scaled_depth_bias; ///< f32 bits
    u8 stencil_read_mask;        ///< D3D12 has one pair for both faces: the front one
    u8 stencil_write_mask;
    u8 strip_cut; ///< D3D12_INDEX_BUFFER_STRIP_CUT_VALUE, from the index format
    u8 padding;

    size_t Hash() const noexcept;

    bool operator==(const GraphicsPipelineCacheKey& rhs) const noexcept;

    bool operator!=(const GraphicsPipelineCacheKey& rhs) const noexcept {
        return !operator==(rhs);
    }

    /// Refreshes the D3D12-only fields from the registers (state must be refreshed first).
    void RefreshStatic(const Maxwell& regs) noexcept;
};
static_assert(std::has_unique_object_representations_v<GraphicsPipelineCacheKey>);
static_assert(std::is_trivially_copyable_v<GraphicsPipelineCacheKey>);
static_assert(std::is_trivially_constructible_v<GraphicsPipelineCacheKey>);

} // namespace D3D12

namespace std {
template <>
struct hash<D3D12::GraphicsPipelineCacheKey> {
    size_t operator()(const D3D12::GraphicsPipelineCacheKey& k) const noexcept {
        return k.Hash();
    }
};
} // namespace std

namespace D3D12 {

/// What a pipeline binds its guest resources with (the rasterizer's caches and heaps).
struct PipelineBindContext {
    Tegra::Engines::Maxwell3D& maxwell3d;
    Tegra::MemoryManager& gpu_memory;
    BufferCache& buffer_cache;
    TextureCache& texture_cache;
    GuestDescriptorQueue& descriptor_queue;
    SamplerHeap& sampler_heap;
};

/// Root arguments of one draw, filled by GraphicsPipeline::Configure.
struct PipelineBindings {
    D3D12_GPU_DESCRIPTOR_HANDLE resource_table{};
    D3D12_GPU_DESCRIPTOR_HANDLE sampler_table{};
    std::array<u32, PUSH_CONSTANT_WORDS> push_constants{};
    /// When set (draw trace), Configure appends the views of the sampled textures here.
    std::vector<VideoCommon::ImageViewId>* trace_views{};
};

/// A guest graphics pipeline: signed DXIL of each stage, its root signature and the PSO, built on
/// a worker when one is given. Mirrors Vulkan::GraphicsPipeline (transitions, IsBuilt).
class GraphicsPipeline {
public:
    static constexpr size_t NUM_STAGES = Maxwell::MaxShaderStage;

    GraphicsPipeline(const Device& device, const TextureCacheRuntime& texture_runtime,
                     VideoCore::ShaderNotify* shader_notify, Common::ThreadWorker* worker_thread,
                     const GraphicsPipelineCacheKey& key,
                     std::array<std::vector<u8>, NUM_STAGES> dxil,
                     const std::array<const Shader::Info*, NUM_STAGES>& infos,
                     const PipelineLayout& layout);
    ~GraphicsPipeline();

    GraphicsPipeline(const GraphicsPipeline&) = delete;
    GraphicsPipeline& operator=(const GraphicsPipeline&) = delete;

    void AddTransition(GraphicsPipeline* transition);

    [[nodiscard]] GraphicsPipeline* Next(const GraphicsPipelineCacheKey& current_key) noexcept {
        if (key == current_key) {
            return this;
        }
        for (size_t i = 0; i < transition_keys.size(); ++i) {
            if (transition_keys[i] == current_key) {
                return transitions[i];
            }
        }
        return nullptr;
    }

    [[nodiscard]] bool IsBuilt() const noexcept {
        return is_built.load(std::memory_order::relaxed);
    }

    /// Blocks until the worker finished the PSO.
    void WaitBuilt();

    /// Null when D3D12 rejected the pipeline (logged once when it was built).
    [[nodiscard]] ID3D12PipelineState* Handle() const noexcept {
        return pipeline_state.Get();
    }

    [[nodiscard]] const PipelineLayout& Layout() const noexcept {
        return layout;
    }

    [[nodiscard]] const Shader::Info& StageInfo(size_t stage) const noexcept {
        return stage_infos[stage];
    }

    [[nodiscard]] bool HasStage(size_t stage) const noexcept {
        return !dxil[stage].empty();
    }

    [[nodiscard]] const GraphicsPipelineCacheKey& Key() const noexcept {
        return key;
    }

    /// Port of Vulkan's ConfigureImpl: synchronizes and binds the guest's buffers and textures,
    /// writes the descriptor tables and transitions sampled and storage images. The render
    /// targets are updated last (UpdateRenderTargets) but not transitioned. Records no
    /// command-list state, so a flush in the middle (descriptor heaps full) loses nothing.
    void Configure(bool is_indexed, const PipelineBindContext& context, PipelineBindings& out);

private:
    void Build(const TextureCacheRuntime& texture_runtime);

    const Device& device;
    const GraphicsPipelineCacheKey key;
    const PipelineLayout& layout;
    std::array<std::vector<u8>, NUM_STAGES> dxil;
    std::array<Shader::Info, NUM_STAGES> stage_infos;
    std::array<u32, NUM_STAGES> enabled_uniform_buffer_masks{};
    VideoCommon::UniformBufferSizes uniform_buffer_sizes{};
    bool has_images{};

    std::vector<GraphicsPipelineCacheKey> transition_keys;
    std::vector<GraphicsPipeline*> transitions;

    ComPtr<ID3D12PipelineState> pipeline_state;

    std::mutex build_mutex;
    std::condition_variable build_condvar;
    std::atomic_bool is_built{false};
};

} // namespace D3D12
