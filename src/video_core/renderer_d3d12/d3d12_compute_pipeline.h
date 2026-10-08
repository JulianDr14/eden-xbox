// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <type_traits>
#include <vector>

#include "common/memory_ledger.h"
#include "common/thread_worker.h"
#include "shader_recompiler/shader_info.h"
#include "video_core/renderer_d3d12/d3d12_device.h"
#include "video_core/renderer_d3d12/d3d12_graphics_pipeline.h"

namespace VideoCore {
class ShaderNotify;
}

namespace Tegra::Engines {
class KeplerCompute;
}

namespace D3D12 {

class PipelineLayout;
class Scheduler;

/// Same key as Vulkan's compute pipelines.
struct ComputePipelineCacheKey {
    u64 unique_hash;
    u32 shared_memory_size;
    std::array<u32, 3> workgroup_size;

    size_t Hash() const noexcept;

    bool operator==(const ComputePipelineCacheKey& rhs) const noexcept;

    bool operator!=(const ComputePipelineCacheKey& rhs) const noexcept {
        return !operator==(rhs);
    }
};
static_assert(std::has_unique_object_representations_v<ComputePipelineCacheKey>);
static_assert(std::is_trivially_copyable_v<ComputePipelineCacheKey>);
static_assert(std::is_trivially_constructible_v<ComputePipelineCacheKey>);

} // namespace D3D12

namespace std {
template <>
struct hash<D3D12::ComputePipelineCacheKey> {
    size_t operator()(const D3D12::ComputePipelineCacheKey& k) const noexcept {
        return k.Hash();
    }
};
} // namespace std

namespace D3D12 {

/// What a compute pipeline binds its guest resources with (the rasterizer's caches and heaps).
struct ComputeBindContext {
    Tegra::Engines::KeplerCompute& kepler_compute;
    Tegra::MemoryManager& gpu_memory;
    BufferCache& buffer_cache;
    TextureCache& texture_cache;
    GuestDescriptorQueue& descriptor_queue;
    SamplerHeap& sampler_heap;
    Scheduler& scheduler;
};

/// A guest compute pipeline: signed DXIL, its root signature and the PSO, built on a worker when
/// one is given. Mirrors Vulkan::ComputePipeline.
class ComputePipeline {
public:
    ComputePipeline(const Device& device, VideoCore::ShaderNotify* shader_notify,
                    Common::ThreadWorker* worker_thread, u64 unique_hash, std::vector<u8> dxil,
                    const Shader::Info& info, const PipelineLayout& layout);
    ~ComputePipeline();

    ComputePipeline(const ComputePipeline&) = delete;
    ComputePipeline& operator=(const ComputePipeline&) = delete;

    [[nodiscard]] bool IsBuilt() const noexcept {
        return is_built.load(std::memory_order::relaxed);
    }

    void WaitBuilt();

    /// Null when D3D12 rejected the pipeline.
    [[nodiscard]] ID3D12PipelineState* Handle() const noexcept {
        return pipeline_state.Get();
    }

    [[nodiscard]] const PipelineLayout& Layout() const noexcept {
        return layout;
    }

    [[nodiscard]] const Shader::Info& Info() const noexcept {
        return info;
    }

    /// Port of Vulkan's ComputePipeline::Configure: binds the launch's buffers and textures and
    /// writes the descriptor tables in root-signature order. Records no command-list state, like
    /// GraphicsPipeline::Configure.
    void Configure(const ComputeBindContext& context, PipelineBindings& out);

    [[nodiscard]] u64 UniqueHash() const noexcept {
        return unique_hash;
    }

private:
    void Build();

    const Device& device;
    const u64 unique_hash;
    const PipelineLayout& layout;
    std::vector<u8> dxil;
    Common::MemoryCharge dxil_charge;
    Shader::Info info;
    VideoCommon::ComputeUniformBufferSizes uniform_buffer_sizes{};
    Common::MemoryCharge charge{Common::MemoryAccount::Pipelines, sizeof(ComputePipeline)};

    ComPtr<ID3D12PipelineState> pipeline_state;
    Common::MemoryCharge pso_charge;

    std::mutex build_mutex;
    std::condition_variable build_condvar;
    std::atomic_bool is_built{false};
};

} // namespace D3D12
