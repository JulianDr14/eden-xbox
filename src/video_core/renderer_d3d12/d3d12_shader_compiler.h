// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <mutex>
#include <span>
#include <vector>

#include <eden_spirv_to_dxil.h>
#include <spirv_to_dxil.h>

#include "common/dynamic_library.h"
#include "video_core/renderer_d3d12/d3d12_device.h"

struct IDxcValidator;

namespace D3D12 {

/// Register spaces the translated shaders use for data Vulkan passes outside descriptor sets.
/// Guest descriptors keep the Vulkan model: space = set, register = binding.
constexpr u32 PUSH_CONSTANT_SPACE = 30;
constexpr u32 RUNTIME_DATA_SPACE = 31;

/// SPIR-V -> signed DXIL, via Mesa's spirv_to_dxil and the DXIL validator in dxil.dll.
///
/// Both DLLs ship in the package and are loaded at runtime, so a console that rejects either one
/// loses the shader path (IsAvailable() == false) instead of failing to activate the app.
class ShaderCompiler {
public:
    ShaderCompiler();
    ~ShaderCompiler();

    ShaderCompiler(const ShaderCompiler&) = delete;
    ShaderCompiler& operator=(const ShaderCompiler&) = delete;

    bool IsAvailable() const {
        return available;
    }

    /// Whether the DLL links pipeline stages (eden_spirv_to_dxil_pipeline). Without it,
    /// CompilePipeline() translates each stage alone and the signatures may not match.
    bool CanLinkStages() const {
        return translate_pipeline != nullptr;
    }

    /// Whether linked pipelines turn integer texture samples into texel loads, reading the sampler
    /// from PipelineLayout::IntegerSamplerIndex (eden_spirv_to_dxil_pipeline_v2). Without it,
    /// DXIL validation rejects those shaders.
    bool LowersIntegerSampling() const {
        return lowers_integer_sampling;
    }

    /// Translates and signs one stage. Throws std::runtime_error with the translator's or the
    /// validator's message on failure. Vertex-pipeline stages get Vulkan's Y-down clip space flipped
    /// to D3D's when flip_y is set.
    std::vector<u8> Compile(std::span<const u32> spirv, dxil_spirv_shader_stage stage,
                            bool flip_y = false) const;

    struct PipelineStage {
        std::span<const u32> spirv;
        dxil_spirv_shader_stage stage;
    };
    struct CompiledStage {
        std::vector<u8> dxil;
        dxil_spirv_metadata metadata;
    };
    struct PipelineOptions {
        /// Applied to the last pre-rasterization stage only.
        dxil_spirv_yz_flip_mode yz_flip = DXIL_SPIRV_YZ_FLIP_NONE;
        u16 y_flip_mask = 0;
        u16 z_flip_mask = 0;
        dxil_spirv_sysval_type first_vertex_and_base_instance = DXIL_SPIRV_SYSVAL_TYPE_ZERO;
    };

    /// Translates, links and signs the stages of one pipeline, given in pipeline order. Returns
    /// one entry per input stage. Throws std::runtime_error on failure.
    std::vector<CompiledStage> CompilePipeline(std::span<const PipelineStage> stages,
                                               const PipelineOptions& options, u64 trace_pipeline = 0) const;

private:
    void Sign(std::vector<u8>& dxil, u64 trace_pipeline = 0) const;
    dxil_spirv_runtime_conf MakeConf() const;

    using PFN_spirv_to_dxil = decltype(&::spirv_to_dxil);
    using PFN_spirv_to_dxil_free = decltype(&::spirv_to_dxil_free);
    using PFN_eden_spirv_to_dxil_pipeline = decltype(&::eden_spirv_to_dxil_pipeline);

    Common::DynamicLibrary spirv_to_dxil_library;
    Common::DynamicLibrary dxil_library;
    PFN_spirv_to_dxil translate{};
    PFN_spirv_to_dxil_free free_dxil{};
    PFN_eden_spirv_to_dxil_pipeline translate_pipeline{};
    ComPtr<IDxcValidator> validator;
    /// The validator is shared by the pipeline workers.
    mutable std::mutex validator_mutex;
    bool lowers_integer_sampling{};
    bool available{};
};

} // namespace D3D12
