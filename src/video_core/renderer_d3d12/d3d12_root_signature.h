// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <memory>
#include <mutex>
#include <span>
#include <unordered_map>
#include <vector>

#include "common/common_types.h"
#include "shader_recompiler/shader_info.h"
#include "video_core/renderer_d3d12/d3d12_device.h"

namespace D3D12 {

/// Root constants for Vulkan push constants (space 30). spirv_to_dxil sizes the CBV to the highest
/// offset read, 16-byte aligned: RescalingLayout needs 8 dwords, RenderAreaLayout 4.
constexpr u32 PUSH_CONSTANT_WORDS = 8;

/// Root signature of a guest pipeline.
///
/// Layout (root parameters, in this order; the tables only exist when non-empty):
///  - root constants, push constants: b0 space 30
///  - root constants, spirv_to_dxil runtime data (first vertex, y-flip mask, group count...): b0
///    space 31
///  - CBV_SRV_UAV descriptor table
///  - sampler descriptor table
///
/// Descriptor tables follow the binding stream of the SPIR-V backend with
/// unified_descriptor_binding: one counter over the pipeline's stages in order and, inside each
/// stage, constant buffers, storage buffers, texture buffers, image buffers, textures, images.
/// spirv_to_dxil keeps set 0 as space 0 and uses the binding as the register of each class, so a
/// descriptor at binding B lands on b<B>, t<B> or u<B> (a combined sampler also on s<B>).
///
/// Table slots, in that order, per guest descriptor (count = descriptor array length):
///  - constant buffer: count CBVs
///  - storage buffer, image buffer, image: count UAVs, then count SRVs. spirv_to_dxil turns the
///    ones it proves read-only into SRVs (declared_read_only_images_as_srvs, NonWritable), so both
///    are declared on the same register and both get written
///  - texture buffer: 1 SRV
///  - texture: count SRVs, plus count samplers in the sampler table
class PipelineLayout {
public:
    ID3D12RootSignature* Handle() const noexcept {
        return root_signature.Get();
    }
    u32 RuntimeDataWords() const noexcept {
        return runtime_data_words;
    }
    /// Root parameter index of each table, or NO_TABLE.
    u32 ResourceTableIndex() const noexcept {
        return resource_table_index;
    }
    u32 SamplerTableIndex() const noexcept {
        return sampler_table_index;
    }
    u32 NumResourceDescriptors() const noexcept {
        return num_resources;
    }
    u32 NumSamplerDescriptors() const noexcept {
        return num_samplers;
    }

    static constexpr u32 PUSH_CONSTANTS_INDEX = 0;
    static constexpr u32 RUNTIME_DATA_INDEX = 1;
    static constexpr u32 NO_TABLE = ~0u;

private:
    friend class RootSignatureCache;

    ComPtr<ID3D12RootSignature> root_signature;
    u32 runtime_data_words{};
    u32 resource_table_index{NO_TABLE};
    u32 sampler_table_index{NO_TABLE};
    u32 num_resources{};
    u32 num_samplers{};
};

/// Builds and deduplicates guest root signatures. Thread-safe: pipelines are built on workers.
class RootSignatureCache {
public:
    explicit RootSignatureCache(const Device& device);
    ~RootSignatureCache();

    /// Layout for the stages of one pipeline, in the order their SPIR-V was emitted. Null entries
    /// (absent stages) are skipped. Throws std::runtime_error if D3D12 rejects the signature.
    const PipelineLayout& Get(std::span<const Shader::Info* const> infos, bool is_compute);

private:
    const Device& device;
    std::mutex mutex;
    std::unordered_map<u64, std::vector<std::pair<std::vector<u32>, std::unique_ptr<PipelineLayout>>>>
        layouts;
};

} // namespace D3D12
