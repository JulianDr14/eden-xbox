// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <type_traits>
#include <utility>
#include <vector>

#include <boost/container/small_vector.hpp>

#include "common/memory_ledger.h"
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
class Scheduler;

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
    Scheduler& scheduler;
};

/// Root arguments of one draw, filled by GraphicsPipeline::Configure.
struct PipelineBindings {
    D3D12_GPU_DESCRIPTOR_HANDLE resource_table{};
    D3D12_GPU_DESCRIPTOR_HANDLE sampler_table{};
    std::array<u32, PUSH_CONSTANT_WORDS> push_constants{};
    /// Integer sampler states (PipelineLayout::IntegerSamplerIndex), 0 when the layout has none.
    D3D12_GPU_VIRTUAL_ADDRESS integer_samplers{};
    /// When set (draw trace), Configure appends the views of the sampled textures here.
    std::vector<VideoCommon::ImageViewId>* trace_views{};
    /// With trace_views: each texture's sampler (Sampler::Describe), in the same order.
    std::vector<std::string>* trace_filters{};
    /// With trace_views: the Shader::TextureType each texture is declared as.
    std::vector<u32>* trace_types{};
    /// A texture of the draw is the bound depth buffer: bind it read-only (DEPTH_SAMPLED_STATE).
    bool depth_sampled{};
};

/// Integer sampler states of one draw or dispatch, indexed by texture binding. Only the rows of
/// integer textures are written: the shaders never read the others, so they are not cleared.
class IntegerSamplerTable {
public:
    void Add(u32 binding, const Sampler& sampler, const ImageView& view) {
        rows.push_back({binding, sampler.IntegerState(static_cast<u32>(std::max(view.range.extent.levels, 1) - 1))});
        num_bindings = std::max(num_bindings, binding + 1);
    }
    /// Copies the rows to the staging stream; returns their address, or 0 if there are none.
    D3D12_GPU_VIRTUAL_ADDRESS Upload(BufferCache& buffer_cache) const;

private:
    boost::container::small_vector<std::pair<u32, eden_integer_sampler_state>, 8> rows;
    u32 num_bindings{};
};

/// A guest graphics pipeline: signed DXIL of each stage, its root signature and the PSO, built on
/// a worker when one is given. Mirrors Vulkan::GraphicsPipeline (transitions, IsBuilt).
///
/// The PSO, the driver's compiled pipeline and by far its largest part, is resident only while it
/// is drawn (PipelineResidencyPolicy): a dormant pipeline keeps its DXIL, and Wake builds the PSO
/// again from it.
class GraphicsPipeline {
public:
    enum class State : u8 {
        Building, ///< DXIL or PSO being built on a worker.
        Ready,    ///< PSO built, or the pipeline failed (Handle() null): drawable or skipped.
        Dormant,  ///< DXIL kept, no PSO: Wake builds it.
    };

    static constexpr size_t NUM_STAGES = Maxwell::MaxShaderStage;
    using DxilStages = std::array<std::vector<u8>, NUM_STAGES>;
    using SharedDxilStages = std::shared_ptr<const DxilStages>;

    /// compile_dxil (SPIR-V to DXIL, the costly part of a new pipeline) runs with the PSO build,
    /// on the worker when one is given; it throws std::exception when the pipeline cannot be made.
    GraphicsPipeline(const Device& device, const TextureCacheRuntime& texture_runtime,
                     VideoCore::ShaderNotify* shader_notify, Common::ThreadWorker* worker_thread,
                     const GraphicsPipelineCacheKey& key, std::function<SharedDxilStages(u64)> compile_dxil,
                     const std::array<const Shader::Info*, NUM_STAGES>& infos,
                     const PipelineLayout& layout, bool build_pso = true);
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
        return build_state.load(std::memory_order::acquire) == State::Ready;
    }

    [[nodiscard]] bool IsDormant() const noexcept {
        return build_state.load(std::memory_order::acquire) == State::Dormant;
    }

    /// Blocks until the worker finished the PSO. A dormant pipeline must be woken first.
    void WaitBuilt();

    /// GPU thread: a dormant pipeline queues the build of its PSO from the kept DXIL.
    void Wake(Common::ThreadWorker& worker);

    /// GPU thread: releases the PSO of a ready pipeline once the GPU is done with it, keeping the
    /// DXIL to build it again. Pipelines that failed have nothing to release. True if released.
    bool Evict(Scheduler& scheduler);

    /// GPU thread: the frame this pipeline was last drawn in.
    void Touch(u64 frame) noexcept {
        last_used_frame = frame;
    }
    [[nodiscard]] u64 LastUsedFrame() const noexcept {
        return last_used_frame;
    }

    /// GPU thread: true only the first time, so a session records each pipeline it draws once.
    [[nodiscard]] bool MarkDrawn() noexcept {
        return !std::exchange(drawn, true);
    }

    /// Key().Hash(), computed once: it identifies the pipeline across sessions.
    [[nodiscard]] u64 KeyHash() const noexcept {
        return key_hash;
    }

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
        return has_stage[stage];
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
    void Build();

    void Publish(State next);

    const Device& device;
    const TextureCacheRuntime& texture_runtime;
    const GraphicsPipelineCacheKey key;
    const u64 key_hash;
    const PipelineLayout& layout;
    SharedDxilStages dxil; ///< Immutable linked bytecode shared across fixed-state PSOs.
    std::array<bool, NUM_STAGES> has_stage{};
    std::array<Shader::Info, NUM_STAGES> stage_infos;
    std::array<u32, NUM_STAGES> enabled_uniform_buffer_masks{};
    VideoCommon::UniformBufferSizes uniform_buffer_sizes{};
    bool has_images{};

    std::vector<GraphicsPipelineCacheKey> transition_keys;
    std::vector<GraphicsPipeline*> transitions;

    ComPtr<ID3D12PipelineState> pipeline_state;

    std::mutex build_mutex;
    std::condition_variable build_condvar;
    std::atomic<State> build_state{State::Building};

    u64 last_used_frame{}; ///< GPU thread only.
    bool drawn{};          ///< GPU thread only.
    Common::MemoryCharge pso_charge; ///< While the PSO is resident.

    /// The object itself, mostly its stages' Shader::Info. Its DXIL is charged where it is shared.
    Common::MemoryCharge charge{Common::MemoryAccount::Pipelines, sizeof(GraphicsPipeline)};
};

} // namespace D3D12
