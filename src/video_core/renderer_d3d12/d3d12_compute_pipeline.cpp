// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <bit>
#include <cstring>
#include <optional>

#include <boost/container/small_vector.hpp>

#include "common/cityhash.h"
#include "common/logging.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "video_core/engines/kepler_compute.h"
#include "video_core/memory_manager.h"
#include "video_core/renderer_d3d12/d3d12_compute_pipeline.h"
#include "video_core/renderer_d3d12/d3d12_descriptor_heap.h"
#include "video_core/renderer_d3d12/d3d12_root_signature.h"
#include "video_core/shader_notify.h"
#include "video_core/surface.h"
#include "video_core/textures/texture.h"

namespace D3D12 {

namespace {

/// Explicit format of a typed image buffer (as pipeline_helper.h in the Vulkan backend).
std::optional<VideoCore::Surface::PixelFormat> PixelFormatFromImageFormat(
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

} // Anonymous namespace

size_t ComputePipelineCacheKey::Hash() const noexcept {
    return static_cast<size_t>(
        Common::CityHash64(reinterpret_cast<const char*>(this), sizeof *this));
}

bool ComputePipelineCacheKey::operator==(const ComputePipelineCacheKey& rhs) const noexcept {
    return std::memcmp(&rhs, this, sizeof *this) == 0;
}

ComputePipeline::ComputePipeline(const Device& device_, VideoCore::ShaderNotify* shader_notify,
                                 Common::ThreadWorker* worker_thread, u64 unique_hash_,
                                 std::vector<u8> dxil_, const Shader::Info& info_,
                                 const PipelineLayout& layout_)
    : device{device_}, unique_hash{unique_hash_}, layout{layout_}, dxil{std::move(dxil_)},
      info{info_} {
    std::ranges::copy(info.constant_buffer_used_sizes, uniform_buffer_sizes.begin());
    if (shader_notify) {
        shader_notify->MarkShaderBuilding();
    }
    auto func{[this, shader_notify] {
        Build();
        {
            std::scoped_lock lock{build_mutex};
            is_built = true;
        }
        build_condvar.notify_all();
        if (shader_notify) {
            shader_notify->MarkShaderComplete();
        }
    }};
    if (worker_thread) {
        worker_thread->QueueWork(std::move(func));
    } else {
        func();
    }
}

ComputePipeline::~ComputePipeline() = default;

void ComputePipeline::WaitBuilt() {
    std::unique_lock lock{build_mutex};
    build_condvar.wait(lock, [this] { return is_built.load(std::memory_order::relaxed); });
}

void ComputePipeline::Configure(const ComputeBindContext& context, PipelineBindings& out) {
    using VideoCommon::ImageViewInOut;
    using VideoCommon::SamplerId;

    BufferCache& buffer_cache = context.buffer_cache;
    TextureCache& texture_cache = context.texture_cache;
    Tegra::MemoryManager& gpu_memory = context.gpu_memory;
    const auto& qmd = context.kepler_compute.launch_description;

    buffer_cache.SetComputeUniformBufferState(info.constant_buffer_mask, &uniform_buffer_sizes);
    buffer_cache.UnbindComputeStorageBuffers();
    size_t ssbo_index = 0;
    for (const auto& desc : info.storage_buffers_descriptors) {
        buffer_cache.BindComputeStorageBuffer(ssbo_index, desc.cbuf_index, desc.cbuf_offset,
                                              desc.is_written);
        ++ssbo_index;
    }

    texture_cache.SynchronizeDescriptors(true);

    boost::container::small_vector<ImageViewInOut, 64> views;
    boost::container::small_vector<SamplerId, 64> samplers;

    const auto& cbufs = qmd.const_buffer_config;
    const bool via_header_index = qmd.linked_tsc != 0;
    const auto read_handle = [&](const auto& desc, u32 index) {
        const u32 index_offset = index << desc.size_shift;
        const u32 offset = desc.cbuf_offset + index_offset;
        const GPUVAddr addr = cbufs[desc.cbuf_index].Address() + offset;
        if constexpr (std::is_same_v<decltype(desc), const Shader::TextureDescriptor&> ||
                      std::is_same_v<decltype(desc), const Shader::TextureBufferDescriptor&>) {
            if (desc.has_secondary) {
                const u32 second_offset = desc.secondary_cbuf_offset + index_offset;
                const GPUVAddr separate_addr =
                    cbufs[desc.secondary_cbuf_index].Address() + second_offset;
                const u32 lhs_raw = gpu_memory.Read<u32>(addr) << desc.shift_left;
                const u32 rhs_raw = gpu_memory.Read<u32>(separate_addr)
                                    << desc.secondary_shift_left;
                return Tegra::Texture::TexturePair(lhs_raw | rhs_raw, via_header_index);
            }
        }
        return Tegra::Texture::TexturePair(gpu_memory.Read<u32>(addr), via_header_index);
    };
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
            samplers.push_back(texture_cache.GetSamplerId(handle.second, true));
        }
    }
    for (const auto& desc : info.image_descriptors) {
        add_image(desc, desc.is_written);
    }
    texture_cache.FillImageViews(std::span(views.data(), views.size()), true);

    buffer_cache.UnbindComputeTextureBuffers();
    size_t buffer_index = 0;
    const auto add_buffer = [&](const auto& desc) {
        constexpr bool is_image =
            std::is_same_v<decltype(desc), const Shader::ImageBufferDescriptor&>;
        for (u32 i = 0; i < desc.count; ++i) {
            bool is_written = false;
            ImageView& image_view = texture_cache.GetImageView(views[buffer_index].id);
            VideoCore::Surface::PixelFormat format = image_view.format;
            if constexpr (is_image) {
                is_written = desc.is_written;
                if (const auto explicit_format = PixelFormatFromImageFormat(desc.format)) {
                    format = *explicit_format;
                }
            }
            buffer_cache.BindComputeTextureBuffer(buffer_index, image_view.GpuAddr(),
                                                  image_view.BufferSize(), format, is_written,
                                                  is_image);
            ++buffer_index;
        }
    };
    std::ranges::for_each(info.texture_buffer_descriptors, add_buffer);
    std::ranges::for_each(info.image_buffer_descriptors, add_buffer);

    buffer_cache.UpdateComputeBuffers();

    // The table is written in root-signature order: uniform, storage and texel buffers while the
    // buffer cache binds them, then textures and images.
    GuestDescriptorQueue& queue = context.descriptor_queue;
    queue.Acquire(layout.NumResourceDescriptors());
    buffer_cache.BindHostComputeBuffers();

    boost::container::small_vector<D3D12_CPU_DESCRIPTOR_HANDLE, 32> sampler_handles;
    boost::container::small_vector<u64, 32> sampler_keys;
    boost::container::small_vector<std::pair<VideoCommon::ImageViewId, bool>, 32> image_transitions;
    const SamplerId* samplers_it = samplers.data();
    const ImageViewInOut* views_it = views.data();
    views_it += Shader::NumDescriptors(info.texture_buffer_descriptors);
    views_it += Shader::NumDescriptors(info.image_buffer_descriptors);
    for (const auto& desc : info.texture_descriptors) {
        for (u32 index = 0; index < desc.count; ++index) {
            const VideoCommon::ImageViewId view_id = (views_it++)->id;
            const ImageView& image_view = texture_cache.GetImageView(view_id);
            image_view.PrepareRead(desc.type);
            queue.AddCopy(image_view.Handle(desc.type));
            const Sampler& sampler = texture_cache.GetSampler(*(samplers_it++));
            sampler_handles.push_back(sampler.Handle());
            sampler_keys.push_back(sampler.Key());
            image_transitions.emplace_back(view_id, false);
        }
    }
    for (const auto& desc : info.image_descriptors) {
        const VideoCommon::ImageViewId view_id = views_it->id;
        views_it += desc.count;
        ImageView& image_view = texture_cache.GetImageView(view_id);
        if (desc.is_written) {
            texture_cache.MarkModification(image_view.image_id);
        }
        queue.AddCopy(image_view.StorageView(desc.type, desc.format));
        queue.AddCopy(image_view.Handle(desc.type));
        image_transitions.emplace_back(view_id, true);
    }

    for (const auto& [view_id, is_storage] : image_transitions) {
        texture_cache.GetImageView(view_id).TransitionImage(
            is_storage ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
                       : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

    out.resource_table = queue.Table();
    out.sampler_table = {};
    if (!sampler_handles.empty()) {
        out.sampler_table = context.sampler_heap.GetTable(
            std::span<const u64>(sampler_keys.data(), sampler_keys.size()),
            std::span<const D3D12_CPU_DESCRIPTOR_HANDLE>(sampler_handles.data(),
                                                         sampler_handles.size()));
    }

    // Push constants: no resolution scaling yet (all rescaling bits clear, down factor 1).
    out.push_constants.fill(0);
    constexpr u32 down_factor_word =
        Shader::Backend::SPIRV::RESCALING_LAYOUT_DOWN_FACTOR_OFFSET / sizeof(u32);
    out.push_constants[down_factor_word] = std::bit_cast<u32>(1.0f);
}

void ComputePipeline::Build() {
    const D3D12_COMPUTE_PIPELINE_STATE_DESC desc{
        .pRootSignature = layout.Handle(),
        .CS = {.pShaderBytecode = dxil.data(), .BytecodeLength = dxil.size()},
        .NodeMask = 0,
        .CachedPSO = {},
        .Flags = D3D12_PIPELINE_STATE_FLAG_NONE,
    };
    const HRESULT hr = device.Get()->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pipeline_state));
    CheckRemovedAfter(device.Get(), [&] {
        return fmt::format("compute PSO for {:016x} (HRESULT 0x{:08X})", unique_hash,
                           static_cast<u32>(hr));
    });
    if (FAILED(hr)) {
        LOG_ERROR(Render, "D3D12: CreateComputePipelineState failed (HRESULT 0x{:08X}) for {:016x}",
                  static_cast<u32>(hr), unique_hash);
        pipeline_state.Reset();
        return;
    }
    LOG_INFO(Render, "D3D12: compute pipeline built for {:016x} ({} + {} descriptors)",
             unique_hash, layout.NumResourceDescriptors(), layout.NumSamplerDescriptors());
}

} // namespace D3D12
