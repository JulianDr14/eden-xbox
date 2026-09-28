// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <bit>
#include <exception>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

#include <boost/container/static_vector.hpp>

#include "common/fs/fs.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/settings.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/environment.h"
#include "shader_recompiler/exception.h"
#include "shader_recompiler/frontend/maxwell/translate_program.h"
#include "shader_recompiler/program_header.h"
#include "video_core/engines/kepler_compute.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/memory_manager.h"
#include "video_core/perf_counters.h"
#include "video_core/renderer_d3d12/d3d12_pipeline_cache.h"
#include "video_core/renderer_d3d12/d3d12_shader_compiler.h"
#include "video_core/renderer_d3d12/d3d12_texture_cache.h"
#include "video_core/renderer_vulkan/vk_state_tracker.h"
#include "video_core/shader_environment.h"
#include "video_core/shader_notify.h"

namespace D3D12 {

namespace {

using Shader::Backend::SPIRV::EmitSPIRV;
using Shader::Maxwell::ConvertLegacyToGeneric;
using Shader::Maxwell::GenerateGeometryPassthrough;
using Shader::Maxwell::MergeDualVertexPrograms;
using Shader::Maxwell::TranslateProgram;
using VideoCommon::ComputeEnvironment;
using VideoCommon::FileEnvironment;
using VideoCommon::GenericEnvironment;
using VideoCommon::GraphicsEnvironment;
using Vulkan::FixedPipelineState;

constexpr u32 CACHE_VERSION = 1;

template <typename Container>
auto MakeSpan(Container& container) {
    return std::span(container.data(), container.size());
}

// MaxwellToOutputTopology, MaxwellToCompareFunction, CastAttributeType and MakeRuntimeInfo follow
// vk_pipeline_cache.cpp. D3D12 has no transform feedback yet (phase 5) and no native OpenGL NDC,
// so the depth conversion always happens in the shader.

Shader::OutputTopology MaxwellToOutputTopology(Maxwell::PrimitiveTopology topology) {
    switch (topology) {
    case Maxwell::PrimitiveTopology::Points:
        return Shader::OutputTopology::PointList;
    case Maxwell::PrimitiveTopology::LineStrip:
        return Shader::OutputTopology::LineStrip;
    default:
        return Shader::OutputTopology::TriangleStrip;
    }
}

Shader::CompareFunction MaxwellToCompareFunction(Maxwell::ComparisonOp comparison) {
    switch (comparison) {
    case Maxwell::ComparisonOp::Never_D3D:
    case Maxwell::ComparisonOp::Never_GL:
        return Shader::CompareFunction::Never;
    case Maxwell::ComparisonOp::Less_D3D:
    case Maxwell::ComparisonOp::Less_GL:
        return Shader::CompareFunction::Less;
    case Maxwell::ComparisonOp::Equal_D3D:
    case Maxwell::ComparisonOp::Equal_GL:
        return Shader::CompareFunction::Equal;
    case Maxwell::ComparisonOp::LessEqual_D3D:
    case Maxwell::ComparisonOp::LessEqual_GL:
        return Shader::CompareFunction::LessThanEqual;
    case Maxwell::ComparisonOp::Greater_D3D:
    case Maxwell::ComparisonOp::Greater_GL:
        return Shader::CompareFunction::Greater;
    case Maxwell::ComparisonOp::NotEqual_D3D:
    case Maxwell::ComparisonOp::NotEqual_GL:
        return Shader::CompareFunction::NotEqual;
    case Maxwell::ComparisonOp::GreaterEqual_D3D:
    case Maxwell::ComparisonOp::GreaterEqual_GL:
        return Shader::CompareFunction::GreaterThanEqual;
    case Maxwell::ComparisonOp::Always_D3D:
    case Maxwell::ComparisonOp::Always_GL:
        return Shader::CompareFunction::Always;
    }
    return Shader::CompareFunction::Always;
}

Shader::AttributeType CastAttributeType(const FixedPipelineState::VertexAttribute& attr) {
    if (attr.enabled == 0) {
        return Shader::AttributeType::Disabled;
    }
    if (attr.Type() == Maxwell::VertexAttribute::Type::SNorm &&
        attr.Size() == Maxwell::VertexAttribute::Size::Size_A2_B10_G10_R10) {
        // DXGI has no signed 10:10:10:2: fetched as R10G10B10A2_UINT (VertexFormat) and normalized
        // in the shader. Mario Wonder packs normals like this; read as floats they were NaN.
        return Shader::AttributeType::SignedNormA2B10G10R10;
    }
    switch (attr.Type()) {
    case Maxwell::VertexAttribute::Type::UnusedEnumDoNotUseBecauseItWillGoAway:
        return Shader::AttributeType::Disabled;
    case Maxwell::VertexAttribute::Type::SNorm:
    case Maxwell::VertexAttribute::Type::UNorm:
    case Maxwell::VertexAttribute::Type::Float:
        return Shader::AttributeType::Float;
    case Maxwell::VertexAttribute::Type::SInt:
        return Shader::AttributeType::SignedInt;
    case Maxwell::VertexAttribute::Type::UInt:
        return Shader::AttributeType::UnsignedInt;
    case Maxwell::VertexAttribute::Type::UScaled:
        return Shader::AttributeType::UnsignedScaled;
    case Maxwell::VertexAttribute::Type::SScaled:
        return Shader::AttributeType::SignedScaled;
    }
    return Shader::AttributeType::Float;
}

Shader::RuntimeInfo MakeRuntimeInfo(std::span<const Shader::IR::Program> programs,
                                    const GraphicsPipelineCacheKey& key,
                                    const Shader::IR::Program& program,
                                    const Shader::IR::Program* previous_program) {
    Shader::RuntimeInfo info;
    if (previous_program) {
        info.previous_stage_stores = previous_program->info.stores;
        info.previous_stage_legacy_stores_mapping = previous_program->info.legacy_stores_mapping;
        if (previous_program->is_geometry_passthrough) {
            info.previous_stage_stores.mask |= previous_program->info.passthrough.mask;
        }
    } else {
        info.previous_stage_stores.mask.set();
    }
    const Shader::Stage stage{program.stage};
    const bool has_geometry{key.unique_hashes[4] != 0 && !programs[4].is_geometry_passthrough};
    const bool gl_ndc{key.state.ndc_minus_one_to_one != 0};
    const float point_size{std::bit_cast<float>(key.state.point_size)};
    switch (stage) {
    case Shader::Stage::VertexB:
        if (!has_geometry) {
            if (key.state.topology == Maxwell::PrimitiveTopology::Points) {
                info.fixed_state_point_size = point_size;
            }
            info.convert_depth_mode = gl_ndc;
        }
        std::ranges::transform(key.state.attributes, info.generic_input_types.begin(),
                               &CastAttributeType);
        break;
    case Shader::Stage::TessellationEval:
        info.tess_clockwise = key.state.tessellation_clockwise != 0;
        info.tess_primitive = [&key] {
            switch (static_cast<Maxwell::Tessellation::DomainType>(
                key.state.tessellation_primitive.Value())) {
            case Maxwell::Tessellation::DomainType::Isolines:
                return Shader::TessPrimitive::Isolines;
            case Maxwell::Tessellation::DomainType::Triangles:
                return Shader::TessPrimitive::Triangles;
            case Maxwell::Tessellation::DomainType::Quads:
                return Shader::TessPrimitive::Quads;
            }
            return Shader::TessPrimitive::Triangles;
        }();
        info.tess_spacing = [&key] {
            switch (static_cast<Maxwell::Tessellation::Spacing>(
                key.state.tessellation_spacing.Value())) {
            case Maxwell::Tessellation::Spacing::Integer:
                return Shader::TessSpacing::Equal;
            case Maxwell::Tessellation::Spacing::FractionalOdd:
                return Shader::TessSpacing::FractionalOdd;
            case Maxwell::Tessellation::Spacing::FractionalEven:
                return Shader::TessSpacing::FractionalEven;
            }
            return Shader::TessSpacing::Equal;
        }();
        break;
    case Shader::Stage::Geometry:
        if (program.output_topology == Shader::OutputTopology::PointList) {
            info.fixed_state_point_size = point_size;
        }
        info.convert_depth_mode = gl_ndc;
        break;
    case Shader::Stage::Fragment:
        info.alpha_test_func = MaxwellToCompareFunction(
            key.state.UnpackComparisonOp(key.state.alpha_test_func.Value()));
        info.alpha_test_reference = std::bit_cast<float>(key.state.alpha_test_ref);
        info.dual_source_blend = key.state.attachment0_dual_source_blend != 0;
        break;
    default:
        break;
    }
    switch (key.state.topology) {
    case Maxwell::PrimitiveTopology::Points:
        info.input_topology = Shader::InputTopology::Points;
        break;
    case Maxwell::PrimitiveTopology::Lines:
    case Maxwell::PrimitiveTopology::LineLoop:
    case Maxwell::PrimitiveTopology::LineStrip:
        info.input_topology = Shader::InputTopology::Lines;
        break;
    case Maxwell::PrimitiveTopology::LinesAdjacency:
    case Maxwell::PrimitiveTopology::LineStripAdjacency:
        info.input_topology = Shader::InputTopology::LinesAdjacency;
        break;
    case Maxwell::PrimitiveTopology::TrianglesAdjacency:
    case Maxwell::PrimitiveTopology::TriangleStripAdjacency:
        info.input_topology = Shader::InputTopology::TrianglesAdjacency;
        break;
    default:
        info.input_topology = Shader::InputTopology::Triangles;
        break;
    }
    info.force_early_z = key.state.early_z != 0;
    info.y_negate = key.state.y_negate != 0;
    return info;
}

/// Pipeline workers. Capped: each one holds a recompiler and a translator's worth of memory, and
/// the console's game partition has 5 GB in total.
size_t GetTotalPipelineWorkers() {
    const size_t threads = std::max<size_t>(std::thread::hardware_concurrency(), 2) - 1;
    return std::min<size_t>(threads, 4);
}

/// Shader capabilities from D3D12's feature queries (logged at boot by Device::LogCapabilities).
struct ShaderCaps {
    bool wave_ops{};
    u32 wave_lane_count_max{};
    bool int64{};
    bool float64{};
    bool vp_and_rt_index_from_any_stage{};
    bool typed_uav_load_additional_formats{};
};

ShaderCaps QueryShaderCaps(const Device& device) {
    ShaderCaps caps;
    D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
    if (SUCCEEDED(device.Get()->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options,
                                                    sizeof(options)))) {
        caps.float64 = options.DoublePrecisionFloatShaderOps != FALSE;
        caps.vp_and_rt_index_from_any_stage =
            options.VPAndRTArrayIndexFromAnyShaderFeedingRasterizerSupportedWithoutGSEmulation !=
            FALSE;
        caps.typed_uav_load_additional_formats = options.TypedUAVLoadAdditionalFormats != FALSE;
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS1 options1{};
    if (SUCCEEDED(device.Get()->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &options1,
                                                    sizeof(options1)))) {
        caps.wave_ops = options1.WaveOps != FALSE;
        caps.wave_lane_count_max = options1.WaveLaneCountMax;
        caps.int64 = options1.Int64ShaderOps != FALSE;
    }
    return caps;
}

} // Anonymous namespace

PipelineCache::PipelineCache(Tegra::MaxwellDeviceMemoryManager& device_memory_,
                             const Device& device_, const ShaderCompiler& compiler_,
                             const TextureCacheRuntime& texture_runtime_,
                             VideoCore::ShaderNotify& shader_notify_)
    : VideoCommon::ShaderCache{device_memory_}, device{device_}, compiler{compiler_},
      texture_runtime{texture_runtime_}, shader_notify{shader_notify_},
      use_asynchronous_shaders{Settings::values.use_asynchronous_shaders.GetValue()},
      root_signatures{device_},
      workers(GetTotalPipelineWorkers(), "D3D12PipelineBuilder", {},
              Common::ThreadPlacement::Background),
      serialization_thread(1, "D3D12PipelineSerialization", {},
                           Common::ThreadPlacement::Background) {
    const ShaderCaps caps = QueryShaderCaps(device);
    profile = Shader::Profile{
        .supported_spirv = 0x00010300,
        .unified_descriptor_binding = true,
        .descriptor_arrays_use_count = true,
        // One SPIR-V variable per binding: DXIL rejects two resources on one register.
        .support_descriptor_aliasing = false,
        .support_int8 = false,
        .support_uniform_and_storage_buffer_8bit = false,
        .support_storage_buffer_8bit = false,
        .support_int16 = false,
        .support_uniform_and_storage_buffer_16bit = false,
        .support_storage_buffer_16bit = false,
        .support_int64 = caps.int64,
        .support_vertex_instance_id = false,
        // Without a denorm mode DXIL leaves it to the driver ("any"); Maxwell flushes, and the
        // FMZ emulation (x * 0 == 0) compares against zero, so flush everywhere.
        .support_float_controls = true,
        .support_fp32_denorm_flush = true,
        .force_fp32_denorm_flush = true,
        .support_explicit_workgroup_layout = false,
        .support_shader_quad_control = false,
        .support_quad_shuffles = false,
        .support_vote = caps.wave_ops,
        .supported_subgroup_stages = caps.wave_ops ? 0x7Fu : 0u,
        .support_viewport_index_layer_non_geometry = caps.vp_and_rt_index_from_any_stage,
        .support_viewport_mask = false,
        .support_typeless_image_loads = caps.typed_uav_load_additional_formats,
        .support_demote_to_helper_invocation = false,
        .support_int64_atomics = false,
        .support_shared_int64_atomics = false,
        .support_derivative_control = true,
        .support_geometry_shader_passthrough = false,
        .support_native_ndc = false,
        .support_scaled_attributes = false,
        .support_multi_viewport = true,
        .support_geometry_streams = false,
        .support_sampled_image_array_nonuniform_indexing = true,
        .support_storage_image_array_nonuniform_indexing = false,
        .support_uniform_texel_buffer_array_nonuniform_indexing = false,
        .support_storage_texel_buffer_array_nonuniform_indexing = false,
        // The Series GPU (RDNA 2) runs waves of 64 lanes; Maxwell warps are 32.
        .warp_size_potentially_larger_than_guest = caps.wave_lane_count_max > 32,
        .lower_left_origin_mode = false,
        .need_declared_frag_colors = true,
        .need_gather_subpixel_offset = true,
        .has_broken_spirv_clamp = false,
        .has_broken_spirv_position_input = false,
        .has_broken_unsigned_image_offsets = false,
        .has_broken_signed_operations = false,
        .has_broken_fp16_float_controls = false,
        .has_broken_fp32_denorm_flush = false,
        .ignore_nan_fp_comparisons = false,
        .has_broken_spirv_subgroup_mask_vector_extract_dynamic = false,
        .has_broken_robust = false,
        // D3D12_RAW_UAV_SRV_BYTE_ALIGNMENT: raw views start on 16 bytes.
        .min_ssbo_alignment = 16,
        .max_user_clip_distances = 8,
    };
    host_info = Shader::HostTranslateInfo{
        .min_ssbo_alignment = 16,
        .max_per_stage_descriptor_sampled_images = 128,
        .max_per_stage_resources = 128,
        .max_descriptor_set_samplers = 2048,
        .max_descriptor_set_uniform_buffers = 14,
        .max_descriptor_set_uniform_buffers_dynamic = 0,
        .max_descriptor_set_storage_buffers = 64,
        .max_descriptor_set_storage_buffers_dynamic = 0,
        .max_descriptor_set_sampled_images = 128,
        .max_descriptor_set_storage_images = 64,
        .max_descriptor_set_input_attachements = 0,
        .support_float64 = caps.float64,
        .support_float16 = false,
        .support_int64 = caps.int64,
        .needs_demote_reorder = true,
        .support_snorm_render_buffer = true,
        .support_viewport_index_layer = caps.vp_and_rt_index_from_any_stage,
        .support_geometry_shader_passthrough = false,
        .support_conditional_barrier = false,
    };
    host_info.ApplyDescriptorLimitPolicy();

    LOG_INFO(Render,
             "D3D12: pipeline cache ready ({} workers, waves up to {} lanes, int64 {}, "
             "VP/RT index outside GS {}, asynchronous shaders {})",
             GetTotalPipelineWorkers(), caps.wave_lane_count_max, caps.int64,
             caps.vp_and_rt_index_from_any_stage, use_asynchronous_shaders);
}

PipelineCache::~PipelineCache() = default;

GraphicsPipeline* PipelineCache::CurrentGraphicsPipeline() {
    if (!RefreshStages(graphics_key.unique_hashes)) {
        current_pipeline = nullptr;
        return nullptr;
    }
    // FixedPipelineState::Refresh only rereads these blocks when Vulkan's state tracker flagged
    // them; D3D12 has no tracker yet (phase 4.3), so they are always reread.
    auto& dirty = maxwell3d->dirty.flags;
    dirty[Vulkan::Dirty::VertexInput] = true;
    dirty[Vulkan::Dirty::Blending] = true;
    dirty[Vulkan::Dirty::ViewportSwizzles] = true;
    graphics_key.state.Refresh(*maxwell3d, dynamic_features);
    graphics_key.RefreshStatic(maxwell3d->regs);

    if (current_pipeline) {
        GraphicsPipeline* const next{current_pipeline->Next(graphics_key)};
        if (next) {
            current_pipeline = next;
            return BuiltPipeline(current_pipeline);
        }
    }
    return CurrentGraphicsPipelineSlowPath();
}

ComputePipeline* PipelineCache::CurrentComputePipeline() {
    const VideoCommon::ShaderInfo* const shader{ComputeShader()};
    if (!shader) {
        return nullptr;
    }
    const auto& qmd{kepler_compute->launch_description};
    const ComputePipelineCacheKey key{
        .unique_hash = shader->unique_hash,
        .shared_memory_size = qmd.shared_alloc,
        .workgroup_size{qmd.block_dim_x, qmd.block_dim_y, qmd.block_dim_z},
    };
    const auto [pair, is_new]{compute_cache.try_emplace(key)};
    auto& pipeline{pair->second};
    if (is_new) {
        VideoCore::Perf::ScopedTimer timer{VideoCore::Perf::Counter::PipelineStallUs};
        pipeline = CreateComputePipeline(key, shader);
    }
    if (!pipeline) {
        return nullptr;
    }
    if (!pipeline->IsBuilt()) {
        // Compute always waits, even with asynchronous shaders: a skipped dispatch leaves the
        // buffers or images it writes stale for the rest of the frame and beyond.
        VideoCore::Perf::ScopedTimer timer{VideoCore::Perf::Counter::PipelineStallUs,
                                           VideoCore::Perf::Counter::PipelineStalls};
        pipeline->WaitBuilt();
    }
    return pipeline.get();
}

void PipelineCache::LoadDiskResources(u64 title_id, std::stop_token stop_loading,
                                      const VideoCore::DiskResourceLoadCallback& callback) {
    if (title_id == 0) {
        return;
    }
    if (!pipeline_cache_filename.empty()) {
        serialization_thread.WaitForRequests();
    }
    const auto shader_dir{Common::FS::GetEdenPath(Common::FS::EdenPath::ShaderDir)};
    const auto base_dir{shader_dir / fmt::format("{:016x}", title_id)};
    if (!Common::FS::CreateDir(shader_dir) || !Common::FS::CreateDir(base_dir)) {
        LOG_ERROR(Common_Filesystem, "Failed to create pipeline cache directories");
        return;
    }
    pipeline_cache_filename = base_dir / "d3d12.bin";

    struct {
        std::mutex mutex;
        size_t total{};
        size_t built{};
        bool has_loaded{};
    } state;

    const auto load_compute{[&](std::ifstream& file, FileEnvironment env) {
        ComputePipelineCacheKey key;
        file.read(reinterpret_cast<char*>(&key), sizeof(key));

        workers.QueueWork([this, key, env_ = std::move(env), &state, &callback]() mutable {
            ShaderPools pools;
            auto pipeline{CreateComputePipeline(pools, key, env_, false)};
            std::scoped_lock lock{state.mutex};
            if (pipeline) {
                compute_cache.emplace(key, std::move(pipeline));
            }
            ++state.built;
            if (state.has_loaded) {
                callback(VideoCore::LoadCallbackStage::Build, state.built, state.total);
            }
        });
        ++state.total;
    }};
    const auto load_graphics{[&](std::ifstream& file, std::vector<FileEnvironment> envs) {
        GraphicsPipelineCacheKey key;
        file.read(reinterpret_cast<char*>(&key), sizeof(key));

        workers.QueueWork([this, key, envs_ = std::move(envs), &state, &callback]() mutable {
            ShaderPools pools;
            boost::container::static_vector<Shader::Environment*, 5> env_ptrs;
            for (auto& env : envs_) {
                env_ptrs.push_back(&env);
            }
            auto pipeline{CreateGraphicsPipeline(pools, key, MakeSpan(env_ptrs), false)};

            std::scoped_lock lock{state.mutex};
            if (pipeline) {
                graphics_cache.emplace(key, std::move(pipeline));
            }
            ++state.built;
            if (state.has_loaded) {
                callback(VideoCore::LoadCallbackStage::Build, state.built, state.total);
            }
        });
        ++state.total;
    }};
    VideoCommon::LoadPipelines(stop_loading, pipeline_cache_filename, CACHE_VERSION, load_compute,
                               load_graphics);

    LOG_INFO(Render, "D3D12: {} pipelines in the disk cache", state.total);

    std::unique_lock lock{state.mutex};
    callback(VideoCore::LoadCallbackStage::Build, 0, state.total);
    state.has_loaded = true;
    lock.unlock();

    workers.WaitForRequests(stop_loading);
}

GraphicsPipeline* PipelineCache::CurrentGraphicsPipelineSlowPath() {
    const auto [pair, is_new]{graphics_cache.try_emplace(graphics_key)};
    auto& pipeline{pair->second};
    if (is_new) {
        // Translating the shaders runs here, on the GPU thread, before the build is queued.
        VideoCore::Perf::ScopedTimer timer{VideoCore::Perf::Counter::PipelineStallUs};
        pipeline = CreateGraphicsPipeline();
    }
    if (!pipeline) {
        return nullptr;
    }
    if (current_pipeline) {
        current_pipeline->AddTransition(pipeline.get());
    }
    current_pipeline = pipeline.get();
    return BuiltPipeline(current_pipeline);
}

GraphicsPipeline* PipelineCache::BuiltPipeline(GraphicsPipeline* pipeline) const noexcept {
    if (pipeline->IsBuilt()) {
        return pipeline;
    }
    // Small draws are usually full-screen passes that build textures once: skipping them would
    // lose the texture, so wait for those like Vulkan does.
    const auto& draw_state = maxwell3d->draw_manager.draw_state;
    if (!use_asynchronous_shaders || draw_state.index_buffer.count <= 6 ||
        draw_state.vertex_buffer.count <= 6) {
        VideoCore::Perf::ScopedTimer timer{VideoCore::Perf::Counter::PipelineStallUs,
                                           VideoCore::Perf::Counter::PipelineStalls};
        pipeline->WaitBuilt();
        return pipeline;
    }
    return nullptr;
}

std::unique_ptr<GraphicsPipeline> PipelineCache::CreateGraphicsPipeline(
    ShaderPools& pools, const GraphicsPipelineCacheKey& key,
    std::span<Shader::Environment* const> envs, bool build_in_parallel) try {
    size_t env_index{0};
    std::array<Shader::IR::Program, Maxwell::MaxShaderProgram> programs;
    const bool uses_vertex_a{key.unique_hashes[0] != 0};
    const bool uses_vertex_b{key.unique_hashes[1] != 0};

    // Layer passthrough generation for devices that cannot write the layer outside a GS
    Shader::IR::Program* layer_source_program{};

    for (size_t index = 0; index < Maxwell::MaxShaderProgram; ++index) {
        const bool is_emulated_stage = layer_source_program != nullptr &&
                                       index == static_cast<u32>(Maxwell::ShaderType::Geometry);
        if (key.unique_hashes[index] == 0 && is_emulated_stage) {
            auto topology = MaxwellToOutputTopology(key.state.topology);
            programs[index] = GenerateGeometryPassthrough(pools.inst, pools.block, host_info,
                                                          *layer_source_program, topology);
            continue;
        }
        if (key.unique_hashes[index] == 0) {
            continue;
        }
        Shader::Environment& env{*envs[env_index]};
        ++env_index;

        const u32 cfg_offset{static_cast<u32>(env.StartAddress() + sizeof(Shader::ProgramHeader))};
        Shader::Maxwell::Flow::CFG cfg(env, pools.flow_block, cfg_offset, index == 0);
        if (!uses_vertex_a || index != 1) {
            programs[index] = TranslateProgram(pools.inst, pools.block, env, cfg, host_info);
        } else {
            // VertexB path when VertexA is present.
            auto& program_va{programs[0]};
            auto program_vb{TranslateProgram(pools.inst, pools.block, env, cfg, host_info)};
            programs[index] = MergeDualVertexPrograms(program_va, program_vb, env);
        }
        if (Settings::values.dump_guest_shaders) {
            env.Dump(key.Hash(), key.unique_hashes[index]);
        }
        if (programs[index].info.requires_layer_emulation) {
            layer_source_program = &programs[index];
        }
    }

    std::array<const Shader::Info*, Maxwell::MaxShaderStage> infos{};
    std::array<std::vector<u32>, Maxwell::MaxShaderStage> spirv;
    boost::container::static_vector<ShaderCompiler::PipelineStage, Maxwell::MaxShaderStage> stages;
    boost::container::static_vector<size_t, Maxwell::MaxShaderStage> stage_indices;

    const Shader::IR::Program* previous_stage{};
    Shader::Backend::Bindings binding;
    for (size_t index = uses_vertex_a && uses_vertex_b ? 1 : 0; index < Maxwell::MaxShaderProgram;
         ++index) {
        const bool is_emulated_stage = layer_source_program != nullptr &&
                                       index == static_cast<u32>(Maxwell::ShaderType::Geometry);
        if (key.unique_hashes[index] == 0 && !is_emulated_stage) {
            continue;
        }
        if (index == 0) {
            LOG_ERROR(Render, "D3D12: VertexA without VertexB is not implemented");
            return nullptr;
        }
        Shader::IR::Program& program{programs[index]};
        const size_t stage_index{index - 1};
        infos[stage_index] = &program.info;

        const auto runtime_info{MakeRuntimeInfo(programs, key, program, previous_stage)};
        ConvertLegacyToGeneric(program, runtime_info);
        spirv[stage_index] = EmitSPIRV(profile, runtime_info, program, binding);
        // Stage indices 0-4 (VS, TCS, TES, GS, FS) match dxil_spirv_shader_stage's values.
        stages.push_back({.spirv = spirv[stage_index],
                          .stage = static_cast<dxil_spirv_shader_stage>(stage_index)});
        stage_indices.push_back(stage_index);
        previous_stage = &program;
    }

    // The guest viewport transform decides the flips per draw, through the runtime data
    // (RasterizerD3D12::UpdateViewports).
    const ShaderCompiler::PipelineOptions options{
        .yz_flip = DXIL_SPIRV_YZ_FLIP_CONDITIONAL,
        .first_vertex_and_base_instance = DXIL_SPIRV_SYSVAL_TYPE_RUNTIME_DATA,
    };
    auto compiled = compiler.CompilePipeline(std::span(stages.data(), stages.size()), options);
    std::array<std::vector<u8>, Maxwell::MaxShaderStage> dxil;
    for (size_t i = 0; i < compiled.size(); ++i) {
        dxil[stage_indices[i]] = std::move(compiled[i].dxil);
    }
    const PipelineLayout& layout = root_signatures.Get(infos, false);

    Common::ThreadWorker* const thread_worker{build_in_parallel ? &workers : nullptr};
    return std::make_unique<GraphicsPipeline>(device, texture_runtime, &shader_notify,
                                              thread_worker, key, std::move(dxil), infos, layout);

} catch (const Shader::Exception& exception) {
    LOG_ERROR(Render, "D3D12: recompiling VS {:016x} PS {:016x} failed: {}", key.unique_hashes[1],
              key.unique_hashes[5], exception.what());
    return nullptr;
} catch (const std::exception& exception) {
    LOG_ERROR(Render, "D3D12: building the pipeline for VS {:016x} PS {:016x} failed: {}",
              key.unique_hashes[1], key.unique_hashes[5], exception.what());
    return nullptr;
}

std::unique_ptr<GraphicsPipeline> PipelineCache::CreateGraphicsPipeline() {
    GraphicsEnvironments environments;
    GetGraphicsEnvironments(environments, graphics_key.unique_hashes);

    main_pools.ReleaseContents();
    auto pipeline{CreateGraphicsPipeline(main_pools, graphics_key, environments.Span(), true)};
    if (!pipeline || pipeline_cache_filename.empty()) {
        return pipeline;
    }
    serialization_thread.QueueWork([this, key = graphics_key, envs = std::move(environments.envs)] {
        boost::container::static_vector<const GenericEnvironment*, Maxwell::MaxShaderProgram>
            env_ptrs;
        for (size_t index = 0; index < Maxwell::MaxShaderProgram; ++index) {
            if (key.unique_hashes[index] != 0) {
                env_ptrs.push_back(&envs[index]);
            }
        }
        SerializePipeline(key, env_ptrs, pipeline_cache_filename, CACHE_VERSION);
    });
    return pipeline;
}

std::unique_ptr<ComputePipeline> PipelineCache::CreateComputePipeline(
    const ComputePipelineCacheKey& key, const VideoCommon::ShaderInfo* shader) {
    const GPUVAddr program_base{kepler_compute->regs.code_loc.Address()};
    const auto& qmd{kepler_compute->launch_description};
    ComputeEnvironment env{*kepler_compute, *gpu_memory, program_base, qmd.program_start};
    env.SetCachedSize(shader->size_bytes);

    main_pools.ReleaseContents();
    auto pipeline{CreateComputePipeline(main_pools, key, env, true)};
    if (!pipeline || pipeline_cache_filename.empty()) {
        return pipeline;
    }
    serialization_thread.QueueWork([this, key, env_ = std::move(env)] {
        SerializePipeline(key, std::array<const GenericEnvironment*, 1>{&env_},
                          pipeline_cache_filename, CACHE_VERSION);
    });
    return pipeline;
}

std::unique_ptr<ComputePipeline> PipelineCache::CreateComputePipeline(
    ShaderPools& pools, const ComputePipelineCacheKey& key, Shader::Environment& env,
    bool build_in_parallel) try {
    Shader::Maxwell::Flow::CFG cfg{env, pools.flow_block, env.StartAddress()};
    if (Settings::values.dump_guest_shaders) {
        env.Dump(key.Hash(), key.unique_hash);
    }
    auto program{TranslateProgram(pools.inst, pools.block, env, cfg, host_info)};
    const std::vector<u32> code{EmitSPIRV(profile, program)};

    const std::array<ShaderCompiler::PipelineStage, 1> stages{{
        {.spirv = code, .stage = DXIL_SPIRV_SHADER_COMPUTE},
    }};
    auto compiled = compiler.CompilePipeline(stages, {});
    const std::array<const Shader::Info*, 1> infos{&program.info};
    const PipelineLayout& layout = root_signatures.Get(infos, true);

    Common::ThreadWorker* const thread_worker{build_in_parallel ? &workers : nullptr};
    return std::make_unique<ComputePipeline>(device, &shader_notify, thread_worker,
                                             key.unique_hash, std::move(compiled[0].dxil),
                                             program.info, layout);

} catch (const Shader::Exception& exception) {
    LOG_ERROR(Render, "D3D12: recompiling compute {:016x} failed: {}", key.unique_hash,
              exception.what());
    return nullptr;
} catch (const std::exception& exception) {
    LOG_ERROR(Render, "D3D12: building compute pipeline {:016x} failed: {}", key.unique_hash,
              exception.what());
    return nullptr;
}

} // namespace D3D12
