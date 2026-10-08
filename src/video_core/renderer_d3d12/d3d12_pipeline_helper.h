// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <bit>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>

#include <boost/container/small_vector.hpp>

#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/shader_info.h"
#include "video_core/memory_manager.h"
#include "video_core/renderer_d3d12/d3d12_descriptor_heap.h"
#include "video_core/renderer_d3d12/d3d12_root_signature.h"
#include "video_core/renderer_d3d12/d3d12_texture_cache.h"
#include "video_core/surface.h"
#include "video_core/textures/texture.h"

// What the graphics and compute pipelines share when they bind guest resources (the D3D12
// counterpart of renderer_vulkan/pipeline_helper.h).

namespace D3D12 {

using ImageViewList = boost::container::small_vector<VideoCommon::ImageViewInOut, 64>;
using SamplerIdList = boost::container::small_vector<VideoCommon::SamplerId, 64>;
/// Views whose images get transitioned once every copy of the draw has been recorded, and whether
/// each one is a storage image.
using ImageTransitionList =
    boost::container::small_vector<std::pair<VideoCommon::ImageViewId, bool>, 32>;

/// Explicit format of a typed image buffer.
[[nodiscard]] inline std::optional<VideoCore::Surface::PixelFormat> PixelFormatFromImageFormat(
    Shader::ImageFormat format) {
    using VideoCore::Surface::PixelFormat;
    switch (format) {
    case Shader::ImageFormat::Typeless:
        return std::nullopt;
    case Shader::ImageFormat::R8_UINT:
        return PixelFormat::R8_UINT;
    case Shader::ImageFormat::R8_SINT:
        return PixelFormat::R8_SINT;
    case Shader::ImageFormat::R16_UINT:
        return PixelFormat::R16_UINT;
    case Shader::ImageFormat::R16_SINT:
        return PixelFormat::R16_SINT;
    case Shader::ImageFormat::R32_UINT:
        return PixelFormat::R32_UINT;
    case Shader::ImageFormat::R32G32_UINT:
        return PixelFormat::R32G32_UINT;
    case Shader::ImageFormat::R32G32B32A32_UINT:
        return PixelFormat::R32G32B32A32_UINT;
    }
    return std::nullopt;
}

/// Reads the texture handle of element index of desc from guest constant buffers.
/// cbuf_address(i) returns the GPU address of constant buffer i.
template <typename Descriptor, typename CbufAddress>
[[nodiscard]] auto ReadTextureHandle(Tegra::MemoryManager& gpu_memory, const Descriptor& desc,
                                     u32 index, bool via_header_index,
                                     CbufAddress&& cbuf_address) {
    const u32 index_offset = index << desc.size_shift;
    const u32 offset = desc.cbuf_offset + index_offset;
    const GPUVAddr addr = cbuf_address(desc.cbuf_index) + offset;
    if constexpr (std::is_same_v<Descriptor, Shader::TextureDescriptor> ||
                  std::is_same_v<Descriptor, Shader::TextureBufferDescriptor>) {
        if (desc.has_secondary) {
            const u32 second_offset = desc.secondary_cbuf_offset + index_offset;
            const GPUVAddr separate_addr = cbuf_address(desc.secondary_cbuf_index) + second_offset;
            const u32 lhs_raw = gpu_memory.Read<u32>(addr) << desc.shift_left;
            const u32 rhs_raw = gpu_memory.Read<u32>(separate_addr) << desc.secondary_shift_left;
            return Tegra::Texture::TexturePair(lhs_raw | rhs_raw, via_header_index);
        }
    }
    return Tegra::Texture::TexturePair(gpu_memory.Read<u32>(addr), via_header_index);
}

/// Appends the image views (and the samplers of the textures) a stage reads, in the order the
/// descriptor table expects: texel buffers, image buffers, textures, images.
/// Info is a Shader::Info or the StageBindings a graphics pipeline keeps of one.
template <typename Info, typename ReadHandle>
void GatherStageViews(const Info& info, TextureCache& texture_cache, bool is_compute,
                      ReadHandle&& read_handle, ImageViewList& views, SamplerIdList& samplers) {
    const auto add_image = [&](const auto& desc, bool blacklist) {
        for (u32 index = 0; index < desc.count; ++index) {
            const auto handle = read_handle(desc, index);
            views.push_back({.index = handle.first, .blacklist = blacklist, .id = {}});
        }
    };
    for (const auto& desc : info.texture_buffer_descriptors) {
        add_image(desc, false);
    }
    for (const auto& desc : info.image_buffer_descriptors) {
        add_image(desc, false);
    }
    for (const auto& desc : info.texture_descriptors) {
        for (u32 index = 0; index < desc.count; ++index) {
            const auto handle = read_handle(desc, index);
            views.push_back({handle.first});
            samplers.push_back(texture_cache.GetSamplerId(handle.second, is_compute));
        }
    }
    for (const auto& desc : info.image_descriptors) {
        add_image(desc, desc.is_written);
    }
}

/// Binds the texel and image buffers of a stage through the buffer cache. bind(index, view,
/// format, is_written, is_image) forwards to Bind{Graphics,Compute}TextureBuffer. views_it
/// advances past them.
template <typename Info, typename Bind>
void BindStageTextureBuffers(const Info& info, TextureCache& texture_cache,
                             const VideoCommon::ImageViewInOut*& views_it, Bind&& bind) {
    size_t index = 0;
    const auto add_buffer = [&](const auto& desc) {
        constexpr bool is_image =
            std::is_same_v<std::remove_cvref_t<decltype(desc)>, Shader::ImageBufferDescriptor>;
        for (u32 i = 0; i < desc.count; ++i) {
            bool is_written = false;
            ImageView& image_view = texture_cache.GetImageView((views_it++)->id);
            VideoCore::Surface::PixelFormat format = image_view.format;
            if constexpr (is_image) {
                is_written = desc.is_written;
                if (const auto explicit_format = PixelFormatFromImageFormat(desc.format)) {
                    format = *explicit_format;
                }
            }
            bind(index, image_view, format, is_written, is_image);
            ++index;
        }
    };
    for (const auto& desc : info.texture_buffer_descriptors) {
        add_buffer(desc);
    }
    for (const auto& desc : info.image_buffer_descriptors) {
        add_buffer(desc);
    }
}

/// Writes the UAV and SRV of every storage image of a stage to the descriptor table.
template <typename Info>
void PushStorageImages(const Info& info, TextureCache& texture_cache,
                              GuestDescriptorQueue& queue,
                              const VideoCommon::ImageViewInOut*& views_it,
                              ImageTransitionList& transitions) {
    for (const auto& desc : info.image_descriptors) {
        const VideoCommon::ImageViewId view_id = views_it->id;
        views_it += desc.count;
        ImageView& image_view = texture_cache.GetImageView(view_id);
        if (desc.is_written) {
            texture_cache.MarkModification(image_view.image_id);
        }
        queue.AddCopy(image_view.StorageView(desc.type, desc.format));
        queue.AddCopy(image_view.Handle(desc.type));
        transitions.emplace_back(view_id, true);
    }
}

/// The samplers of one draw or dispatch, uploaded (or reused) as one sampler table.
class SamplerTableBuilder {
public:
    void Add(const Sampler& sampler) {
        handles.push_back(sampler.Handle());
        keys.push_back(sampler.Key());
    }

    [[nodiscard]] D3D12_GPU_DESCRIPTOR_HANDLE Table(SamplerHeap& heap) const {
        if (handles.empty()) {
            return {};
        }
        return heap.GetTable(std::span<const u64>(keys.data(), keys.size()),
                             std::span<const D3D12_CPU_DESCRIPTOR_HANDLE>(handles.data(),
                                                                          handles.size()));
    }

private:
    boost::container::small_vector<D3D12_CPU_DESCRIPTOR_HANDLE, 32> handles;
    boost::container::small_vector<u64, 32> keys;
};

/// Push constants with no resolution scaling (all rescaling bits clear, down factor 1).
inline void ResetPushConstants(std::array<u32, PUSH_CONSTANT_WORDS>& words) {
    words.fill(0);
    constexpr u32 down_factor_word =
        Shader::Backend::SPIRV::RESCALING_LAYOUT_DOWN_FACTOR_OFFSET / sizeof(u32);
    words[down_factor_word] = std::bit_cast<u32>(1.0f);
}

} // namespace D3D12
