// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>
#include <memory>
#include <span>
#include <stop_token>

#include "common/container/unordered_map.h"
#include "common/thread_worker.h"
#include "shader_recompiler/frontend/ir/basic_block.h"
#include "shader_recompiler/frontend/ir/value.h"
#include "shader_recompiler/frontend/maxwell/control_flow.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/object_pool.h"
#include "shader_recompiler/profile.h"
#include "video_core/host1x/gpu_device_memory_manager.h"
#include "video_core/rasterizer_interface.h"
#include "video_core/renderer_d3d12/d3d12_compute_pipeline.h"
#include "video_core/renderer_d3d12/d3d12_graphics_pipeline.h"
#include "video_core/renderer_d3d12/d3d12_linked_shader_cache.h"
#include "video_core/renderer_d3d12/d3d12_memory_guard.h"
#include "video_core/renderer_d3d12/d3d12_root_signature.h"
#include "video_core/renderer_vulkan/fixed_pipeline_state.h"
#include "video_core/shader_cache.h"

namespace Shader {
class Environment;
}

namespace VideoCore {
class ShaderNotify;
}

namespace D3D12 {

class ShaderCompiler;
class TextureCacheRuntime;

struct ShaderPools {
    void ReleaseContents() {
        flow_block.ReleaseContents();
        block.ReleaseContents();
        inst.ReleaseContents();
    }

    Shader::ObjectPool<Shader::IR::Inst> inst{8192};
    Shader::ObjectPool<Shader::IR::Block> block{32};
    Shader::ObjectPool<Shader::Maxwell::Flow::Block> flow_block{32};
};

/// Guest shaders -> D3D12 pipelines, modeled on Vulkan::PipelineCache.
///
/// Maxwell code goes through the shared recompiler to SPIR-V (one binding counter across the
/// stages), then spirv_to_dxil translates and links the stages and dxil.dll signs them. The PSO is
/// created on a worker; until it is ready the draw is skipped, like Vulkan's asynchronous shaders.
/// The disk cache stores the guest environments, as Vulkan does, and rebuilds from them at boot.
class PipelineCache : public VideoCommon::ShaderCache {
public:
    PipelineCache(Tegra::MaxwellDeviceMemoryManager& device_memory, const Device& device,
                  const ShaderCompiler& compiler, const TextureCacheRuntime& texture_runtime,
                  VideoCore::ShaderNotify& shader_notify);
    ~PipelineCache();

    void GuardMemory(const CacheMemorySnapshot& snapshot) {
        // Optional cache: emptied when headroom is low, given back only after sustained headroom
        // (OptionalCacheGate). The cache mutex is taken only on those two transitions.
        switch (linked_shaders_gate.Update(snapshot.app_used, snapshot.app_limit)) {
        case OptionalCacheGate::Action::Trim:
            linked_shaders.SetBudget(0);
            break;
        case OptionalCacheGate::Action::Restore:
            linked_shaders.SetBudget(decltype(linked_shaders)::DEFAULT_BUDGET);
            break;
        case OptionalCacheGate::Action::None:
            break;
        }
    }

    /// The pipeline of the current Maxwell state, or null while it is still compiling (or if it
    /// failed to compile).
    [[nodiscard]] GraphicsPipeline* CurrentGraphicsPipeline();

    [[nodiscard]] ComputePipeline* CurrentComputePipeline();

    void LoadDiskResources(u64 title_id, std::stop_token stop_loading,
                           const VideoCore::DiskResourceLoadCallback& callback);

private:
    [[nodiscard]] GraphicsPipeline* CurrentGraphicsPipelineSlowPath();

    [[nodiscard]] GraphicsPipeline* BuiltPipeline(GraphicsPipeline* pipeline) const noexcept;

    std::unique_ptr<GraphicsPipeline> CreateGraphicsPipeline();

    std::unique_ptr<GraphicsPipeline> CreateGraphicsPipeline(
        ShaderPools& pools, const GraphicsPipelineCacheKey& key,
        std::span<Shader::Environment* const> envs, bool build_in_parallel);

    std::unique_ptr<ComputePipeline> CreateComputePipeline(const ComputePipelineCacheKey& key,
                                                           const VideoCommon::ShaderInfo* shader);

    std::unique_ptr<ComputePipeline> CreateComputePipeline(ShaderPools& pools,
                                                           const ComputePipelineCacheKey& key,
                                                           Shader::Environment& env,
                                                           bool build_in_parallel);

    const Device& device;
    const ShaderCompiler& compiler;
    const TextureCacheRuntime& texture_runtime;
    VideoCore::ShaderNotify& shader_notify;
    bool use_asynchronous_shaders{};

    GraphicsPipelineCacheKey graphics_key{};
    GraphicsPipeline* current_pipeline{};

    RootSignatureCache root_signatures;

    ::Common::unordered_map<ComputePipelineCacheKey, std::unique_ptr<ComputePipeline>>
        compute_cache;
    ::Common::unordered_map<GraphicsPipelineCacheKey, std::unique_ptr<GraphicsPipeline>>
        graphics_cache;

    ShaderPools main_pools;

    Shader::Profile profile;
    Shader::HostTranslateInfo host_info;
    /// Every dynamic feature off: D3D12 bakes that state into the PSO.
    Vulkan::DynamicFeatures dynamic_features{};

    std::filesystem::path pipeline_cache_filename;

    LinkedShaderCache<GraphicsPipeline::DxilStages> linked_shaders;
    OptionalCacheGate linked_shaders_gate;

    // Last, so they are joined before the pipelines they build are destroyed.
    Common::ThreadWorker workers;
    Common::ThreadWorker serialization_thread;
};

} // namespace D3D12
