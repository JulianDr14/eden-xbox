// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <atomic>
#include <bit>
#include <optional>
#include <cstddef>
#include <cstring>

#include <boost/container/small_vector.hpp>

#include "common/cityhash.h"
#include "common/logging.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "video_core/memory_manager.h"
#include "video_core/renderer_d3d12/d3d12_descriptor_heap.h"
#include "video_core/renderer_d3d12/d3d12_graphics_pipeline.h"
#include "video_core/renderer_d3d12/d3d12_maxwell_to_d3d12.h"
#include "video_core/renderer_d3d12/d3d12_root_signature.h"
#include "video_core/renderer_d3d12/d3d12_texture_cache.h"
#include "video_core/shader_notify.h"
#include "video_core/surface.h"
#include "video_core/textures/texture.h"

namespace D3D12 {

namespace {

using Vulkan::FixedPipelineState;

constexpr size_t KEY_EXTRA_OFFSET = offsetof(GraphicsPipelineCacheKey, depth_bias);
constexpr size_t KEY_EXTRA_SIZE = sizeof(GraphicsPipelineCacheKey) - KEY_EXTRA_OFFSET;

/// Bytes of the key that count: Vulkan's state is only valid up to FixedPipelineState::Size().
size_t PrefixSize(const GraphicsPipelineCacheKey& key) {
    return sizeof(key.unique_hashes) + key.state.Size();
}

size_t NumAttachments(const FixedPipelineState& state) {
    size_t num{};
    for (size_t index = 0; index < Maxwell::NumRenderTargets; ++index) {
        const auto format{static_cast<Tegra::RenderTargetFormat>(state.color_formats[index])};
        if (format != Tegra::RenderTargetFormat::NONE) {
            num = index + 1;
        }
    }
    return num;
}

D3D12_SHADER_BYTECODE Bytecode(const std::vector<u8>& code) {
    return {.pShaderBytecode = code.empty() ? nullptr : code.data(),
            .BytecodeLength = code.size()};
}

D3D12_DEPTH_STENCILOP_DESC StencilFace(const auto& face) {
    return {
        .StencilFailOp = MaxwellToD3D12::StencilOp(face.ActionStencilFail()),
        .StencilDepthFailOp = MaxwellToD3D12::StencilOp(face.ActionDepthFail()),
        .StencilPassOp = MaxwellToD3D12::StencilOp(face.ActionDepthPass()),
        .StencilFunc = MaxwellToD3D12::ComparisonFunc(face.TestFunc()),
    };
}

D3D12_RENDER_TARGET_BLEND_DESC BlendTarget(const FixedPipelineState::BlendingAttachment& blend) {
    const std::array mask = blend.Mask();
    const UINT8 write_mask = static_cast<UINT8>(
        (mask[0] ? D3D12_COLOR_WRITE_ENABLE_RED : 0) |
        (mask[1] ? D3D12_COLOR_WRITE_ENABLE_GREEN : 0) |
        (mask[2] ? D3D12_COLOR_WRITE_ENABLE_BLUE : 0) |
        (mask[3] ? D3D12_COLOR_WRITE_ENABLE_ALPHA : 0));
    if (blend.enable == 0) {
        return {
            .BlendEnable = FALSE,
            .LogicOpEnable = FALSE,
            .SrcBlend = D3D12_BLEND_ONE,
            .DestBlend = D3D12_BLEND_ZERO,
            .BlendOp = D3D12_BLEND_OP_ADD,
            .SrcBlendAlpha = D3D12_BLEND_ONE,
            .DestBlendAlpha = D3D12_BLEND_ZERO,
            .BlendOpAlpha = D3D12_BLEND_OP_ADD,
            .LogicOp = D3D12_LOGIC_OP_NOOP,
            .RenderTargetWriteMask = write_mask,
        };
    }
    return {
        .BlendEnable = TRUE,
        .LogicOpEnable = FALSE,
        .SrcBlend = MaxwellToD3D12::BlendFactor(blend.SourceRGBFactor(), false),
        .DestBlend = MaxwellToD3D12::BlendFactor(blend.DestRGBFactor(), false),
        .BlendOp = MaxwellToD3D12::BlendOp(blend.EquationRGB()),
        .SrcBlendAlpha = MaxwellToD3D12::BlendFactor(blend.SourceAlphaFactor(), true),
        .DestBlendAlpha = MaxwellToD3D12::BlendFactor(blend.DestAlphaFactor(), true),
        .BlendOpAlpha = MaxwellToD3D12::BlendOp(blend.EquationAlpha()),
        .LogicOp = D3D12_LOGIC_OP_NOOP,
        .RenderTargetWriteMask = write_mask,
    };
}

/// Warns once per process about state D3D12 cannot express.
void WarnOnce(std::atomic_bool& flag, const char* what) {
    if (!flag.exchange(true, std::memory_order_relaxed)) {
        LOG_WARNING(Render, "D3D12: {} is not supported and is ignored", what);
    }
}
std::atomic_bool warned_logic_op;
std::atomic_bool warned_depth_bounds;
std::atomic_bool warned_point_fill;
std::atomic_bool warned_vertex_format;
std::atomic_bool warned_conservative;

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

size_t GraphicsPipelineCacheKey::Hash() const noexcept {
    const u64 prefix = Common::CityHash64(reinterpret_cast<const char*>(this), PrefixSize(*this));
    const u64 extra = Common::CityHash64(reinterpret_cast<const char*>(this) + KEY_EXTRA_OFFSET,
                                         KEY_EXTRA_SIZE);
    return static_cast<size_t>(prefix ^ (extra + 0x9e3779b97f4a7c15ULL + (prefix << 6) +
                                         (prefix >> 2)));
}

bool GraphicsPipelineCacheKey::operator==(const GraphicsPipelineCacheKey& rhs) const noexcept {
    const size_t prefix = PrefixSize(*this);
    return prefix == PrefixSize(rhs) && std::memcmp(this, &rhs, prefix) == 0 &&
           std::memcmp(reinterpret_cast<const char*>(this) + KEY_EXTRA_OFFSET,
                       reinterpret_cast<const char*>(&rhs) + KEY_EXTRA_OFFSET,
                       KEY_EXTRA_SIZE) == 0;
}

void GraphicsPipelineCacheKey::RefreshStatic(const Maxwell& regs) noexcept {
    const auto& dynamic = state.dynamic_state;
    const bool bias = dynamic.depth_bias_enable != 0;
    depth_bias = bias ? std::bit_cast<u32>(regs.depth_bias) : 0;
    depth_bias_clamp = bias ? std::bit_cast<u32>(regs.depth_bias_clamp) : 0;
    slope_scaled_depth_bias = bias ? std::bit_cast<u32>(regs.slope_scale_depth_bias) : 0;
    const bool stencil = dynamic.stencil_enable != 0;
    stencil_read_mask = stencil ? static_cast<u8>(regs.stencil_front_func_mask) : 0;
    stencil_write_mask = stencil ? static_cast<u8>(regs.stencil_front_mask) : 0;
    strip_cut = 0;
    if (dynamic.primitive_restart_enable != 0) {
        strip_cut = static_cast<u8>(regs.index_buffer.format == Maxwell::IndexFormat::UnsignedInt
                                        ? D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF
                                        : D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF);
    }
    padding = 0;
}

GraphicsPipeline::GraphicsPipeline(const Device& device_,
                                   const TextureCacheRuntime& texture_runtime,
                                   VideoCore::ShaderNotify* shader_notify,
                                   Common::ThreadWorker* worker_thread,
                                   const GraphicsPipelineCacheKey& key_,
                                   std::array<std::vector<u8>, NUM_STAGES> dxil_,
                                   const std::array<const Shader::Info*, NUM_STAGES>& infos,
                                   const PipelineLayout& layout_)
    : device{device_}, key{key_}, layout{layout_}, dxil{std::move(dxil_)} {
    if (shader_notify) {
        shader_notify->MarkShaderBuilding();
    }
    for (size_t stage = 0; stage < NUM_STAGES; ++stage) {
        const Shader::Info* const info = infos[stage];
        if (!info) {
            continue;
        }
        stage_infos[stage] = *info;
        enabled_uniform_buffer_masks[stage] = info->constant_buffer_mask;
        std::ranges::copy(info->constant_buffer_used_sizes, uniform_buffer_sizes[stage].begin());
        has_images |= !info->image_descriptors.empty();
    }
    auto func{[this, &texture_runtime, shader_notify] {
        Build(texture_runtime);
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

GraphicsPipeline::~GraphicsPipeline() = default;

void GraphicsPipeline::AddTransition(GraphicsPipeline* transition) {
    transition_keys.push_back(transition->key);
    transitions.push_back(transition);
}

void GraphicsPipeline::WaitBuilt() {
    std::unique_lock lock{build_mutex};
    build_condvar.wait(lock, [this] { return is_built.load(std::memory_order::relaxed); });
}

void GraphicsPipeline::Configure(bool is_indexed, const PipelineBindContext& context,
                                 PipelineBindings& out) {
    using VideoCommon::ImageViewInOut;
    using VideoCommon::SamplerId;

    BufferCache& buffer_cache = context.buffer_cache;
    TextureCache& texture_cache = context.texture_cache;
    Tegra::MemoryManager& gpu_memory = context.gpu_memory;
    const auto& maxwell3d = context.maxwell3d;
    const auto& regs = maxwell3d.regs;

    boost::container::small_vector<ImageViewInOut, 64> views;
    boost::container::small_vector<SamplerId, 64> samplers;

    texture_cache.SynchronizeDescriptors(false);
    buffer_cache.SetUniformBuffersState(enabled_uniform_buffer_masks, &uniform_buffer_sizes);

    const bool via_header_index = regs.sampler_binding == Maxwell::SamplerBinding::ViaHeaderBinding;
    const auto config_stage = [&](size_t stage) {
        const Shader::Info& info = stage_infos[stage];
        buffer_cache.UnbindGraphicsStorageBuffers(stage);
        size_t ssbo_index = 0;
        for (const auto& desc : info.storage_buffers_descriptors) {
            buffer_cache.BindGraphicsStorageBuffer(stage, ssbo_index, desc.cbuf_index,
                                                   desc.cbuf_offset, desc.is_written);
            ++ssbo_index;
        }
        const auto& cbufs = maxwell3d.state.shader_stages[stage].const_buffers;
        const auto read_handle = [&](const auto& desc, u32 index) {
            const u32 index_offset = index << desc.size_shift;
            const u32 offset = desc.cbuf_offset + index_offset;
            const GPUVAddr addr = cbufs[desc.cbuf_index].address + offset;
            if constexpr (std::is_same_v<decltype(desc), const Shader::TextureDescriptor&> ||
                          std::is_same_v<decltype(desc),
                                         const Shader::TextureBufferDescriptor&>) {
                if (desc.has_secondary) {
                    const u32 second_offset = desc.secondary_cbuf_offset + index_offset;
                    const GPUVAddr separate_addr =
                        cbufs[desc.secondary_cbuf_index].address + second_offset;
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
                samplers.push_back(texture_cache.GetSamplerId(handle.second, false));
            }
        }
        for (const auto& desc : info.image_descriptors) {
            add_image(desc, desc.is_written);
        }
    };
    for (size_t stage = 0; stage < NUM_STAGES; ++stage) {
        if (HasStage(stage)) {
            config_stage(stage);
        }
    }
    texture_cache.FillImageViews(std::span(views.data(), views.size()), false, has_images);

    ImageViewInOut* texture_buffer_it = views.data();
    const auto bind_stage_info = [&](size_t stage) {
        size_t index = 0;
        const auto add_buffer = [&](const auto& desc) {
            constexpr bool is_image =
                std::is_same_v<decltype(desc), const Shader::ImageBufferDescriptor&>;
            for (u32 i = 0; i < desc.count; ++i) {
                bool is_written = false;
                ImageView& image_view = texture_cache.GetImageView(texture_buffer_it->id);
                VideoCore::Surface::PixelFormat format = image_view.format;
                if constexpr (is_image) {
                    is_written = desc.is_written;
                    if (const auto explicit_format =
                            PixelFormatFromImageFormat(desc.format)) {
                        format = *explicit_format;
                    }
                }
                buffer_cache.BindGraphicsTextureBuffer(stage, index, image_view.GpuAddr(),
                                                       image_view.BufferSize(), format,
                                                       is_written, is_image);
                ++index;
                ++texture_buffer_it;
            }
        };
        buffer_cache.UnbindGraphicsTextureBuffers(stage);
        const Shader::Info& info = stage_infos[stage];
        for (const auto& desc : info.texture_buffer_descriptors) {
            add_buffer(desc);
        }
        for (const auto& desc : info.image_buffer_descriptors) {
            add_buffer(desc);
        }
        texture_buffer_it += Shader::NumDescriptors(info.texture_descriptors);
        texture_buffer_it += Shader::NumDescriptors(info.image_descriptors);
    };
    for (size_t stage = 0; stage < NUM_STAGES; ++stage) {
        if (HasStage(stage)) {
            bind_stage_info(stage);
        }
    }

    buffer_cache.UpdateGraphicsBuffers(is_indexed);
    buffer_cache.BindHostGeometryBuffers(is_indexed);

    // The table is written in root-signature order while the caches bind, stage by stage:
    // uniform, storage and texel buffers from the buffer cache, then textures and images.
    GuestDescriptorQueue& queue = context.descriptor_queue;
    queue.Acquire(layout.NumResourceDescriptors());
    boost::container::small_vector<D3D12_CPU_DESCRIPTOR_HANDLE, 32> sampler_handles;
    boost::container::small_vector<u64, 32> sampler_keys;
    // Views whose images get transitioned once every copy of this draw has been recorded.
    boost::container::small_vector<std::pair<VideoCommon::ImageViewId, bool>, 32> image_transitions;
    bool uses_render_area = false;
    const SamplerId* samplers_it = samplers.data();
    const ImageViewInOut* views_it = views.data();
    for (size_t stage = 0; stage < NUM_STAGES; ++stage) {
        if (!HasStage(stage)) {
            continue;
        }
        buffer_cache.BindHostStageBuffers(stage);
        const Shader::Info& info = stage_infos[stage];
        views_it += Shader::NumDescriptors(info.texture_buffer_descriptors);
        views_it += Shader::NumDescriptors(info.image_buffer_descriptors);
        for (const auto& desc : info.texture_descriptors) {
            for (u32 index = 0; index < desc.count; ++index) {
                const VideoCommon::ImageViewId view_id = (views_it++)->id;
                const ImageView& image_view = texture_cache.GetImageView(view_id);
                queue.AddCopy(image_view.Handle(desc.type));
                const Sampler& sampler = texture_cache.GetSampler(*(samplers_it++));
                sampler_handles.push_back(sampler.Handle());
                sampler_keys.push_back(sampler.Key());
                image_transitions.emplace_back(view_id, false);
                if (out.trace_views) {
                    out.trace_views->push_back(view_id);
                }
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
        uses_render_area |= info.uses_render_area;
    }
    if (buffer_cache.any_buffer_uploaded) {
        buffer_cache.runtime.PostCopyBarrier();
        buffer_cache.any_buffer_uploaded = false;
    }
    texture_cache.UpdateRenderTargets(false);
    texture_cache.CheckFeedbackLoop(std::span<const ImageViewInOut>{views.data(), views.size()});

    for (const auto& [view_id, is_storage] : image_transitions) {
        texture_cache.GetImageView(view_id).TransitionImage(
            is_storage ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
                       : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

    out.resource_table = queue.Table();
    out.sampler_table = {};
    if (!sampler_handles.empty()) {
        out.sampler_table = context.sampler_heap.GetTable(
            std::span<const u64>(sampler_keys.data(), sampler_keys.size()),
            std::span<const D3D12_CPU_DESCRIPTOR_HANDLE>(sampler_handles.data(),
                                                         sampler_handles.size()));
    }

    // Push constants: no resolution scaling yet (all rescaling bits clear, down factor 1), then
    // the render area, which shares the first words exactly as in the Vulkan backend.
    out.push_constants.fill(0);
    constexpr u32 down_factor_word =
        Shader::Backend::SPIRV::RESCALING_LAYOUT_DOWN_FACTOR_OFFSET / sizeof(u32);
    out.push_constants[down_factor_word] = std::bit_cast<u32>(1.0f);
    if (uses_render_area) {
        out.push_constants[0] = std::bit_cast<u32>(static_cast<f32>(regs.surface_clip.width));
        out.push_constants[1] = std::bit_cast<u32>(static_cast<f32>(regs.surface_clip.height));
    }
}

void GraphicsPipeline::Build(const TextureCacheRuntime& texture_runtime) {
    using VideoCore::Surface::PixelFormatFromDepthFormat;
    using VideoCore::Surface::PixelFormatFromRenderTargetFormat;

    const FixedPipelineState& state = key.state;
    const FixedPipelineState::DynamicState& dynamic = state.dynamic_state;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = layout.Handle();
    desc.VS = Bytecode(dxil[0]);
    desc.HS = Bytecode(dxil[1]);
    desc.DS = Bytecode(dxil[2]);
    desc.GS = Bytecode(dxil[3]);
    desc.PS = Bytecode(dxil[4]);

    // Vertex input: nir_to_dxil names generic attribute N "TEXCOORD" N. Strides are dynamic (they
    // go in the vertex buffer views), so the layout only needs slots and offsets.
    std::vector<D3D12_INPUT_ELEMENT_DESC> elements;
    for (size_t index = 0; index < state.attributes.size(); ++index) {
        const auto& attribute = state.attributes[index];
        if (attribute.enabled == 0 || !stage_infos[0].loads.Generic(index)) {
            continue;
        }
        DXGI_FORMAT format = MaxwellToD3D12::VertexFormat(attribute.Type(), attribute.Size());
        if (format == DXGI_FORMAT_UNKNOWN) {
            WarnOnce(warned_vertex_format, "a vertex attribute format (read as RGBA32F)");
            format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        }
        const u32 slot = attribute.buffer;
        const u32 divisor = state.binding_divisors[slot];
        elements.push_back({
            .SemanticName = "TEXCOORD",
            .SemanticIndex = static_cast<UINT>(index),
            .Format = format,
            .InputSlot = slot,
            .AlignedByteOffset = attribute.offset,
            .InputSlotClass = divisor != 0 ? D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA
                                           : D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
            .InstanceDataStepRate = divisor,
        });
    }
    desc.InputLayout = {.pInputElementDescs = elements.empty() ? nullptr : elements.data(),
                        .NumElements = static_cast<UINT>(elements.size())};
    desc.IBStripCutValue = static_cast<D3D12_INDEX_BUFFER_STRIP_CUT_VALUE>(key.strip_cut);

    const bool has_tessellation = !dxil[1].empty() || !dxil[2].empty();
    desc.PrimitiveTopologyType = MaxwellToD3D12::PrimitiveTopologyType(state.topology);
    if (has_tessellation) {
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
    } else if (desc.PrimitiveTopologyType == D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH) {
        LOG_WARNING(Render, "D3D12: patch topology without tessellation, using points");
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
    }

    const auto polygon_mode = FixedPipelineState::UnpackPolygonMode(state.polygon_mode);
    if (polygon_mode == Maxwell::PolygonMode::Point) {
        WarnOnce(warned_point_fill, "point fill mode");
    }
    if (state.conservative_raster_enable != 0) {
        WarnOnce(warned_conservative, "conservative rasterization");
    }
    // The y-flip happens in the shader (spirv_to_dxil yz_flip), so primitives keep the winding they
    // have in Vulkan's framebuffer space and the front face maps directly.
    desc.RasterizerState = {
        .FillMode = MaxwellToD3D12::FillMode(polygon_mode),
        .CullMode = MaxwellToD3D12::CullMode(dynamic.cull_enable != 0, dynamic.CullFace()),
        .FrontCounterClockwise = dynamic.FrontFace() == Maxwell::FrontFace::CounterClockWise,
        .DepthBias = static_cast<INT>(std::bit_cast<f32>(key.depth_bias) / 2.0f),
        .DepthBiasClamp = std::bit_cast<f32>(key.depth_bias_clamp),
        .SlopeScaledDepthBias = std::bit_cast<f32>(key.slope_scaled_depth_bias),
        .DepthClipEnable = dynamic.depth_clamp_disabled != 0,
        .MultisampleEnable = FALSE,
        .AntialiasedLineEnable = state.smooth_lines != 0,
        .ForcedSampleCount = 0,
        .ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF,
    };

    if (dynamic.logic_op_enable != 0) {
        WarnOnce(warned_logic_op, "logic op");
    }
    const size_t num_attachments = NumAttachments(state);
    desc.BlendState.AlphaToCoverageEnable = state.alpha_to_coverage_enabled != 0;
    desc.BlendState.IndependentBlendEnable = TRUE;
    desc.NumRenderTargets = static_cast<UINT>(num_attachments);
    for (size_t index = 0; index < num_attachments; ++index) {
        desc.BlendState.RenderTarget[index] = BlendTarget(state.attachments[index]);
        const auto format = static_cast<Tegra::RenderTargetFormat>(state.color_formats[index]);
        desc.RTVFormats[index] =
            format == Tegra::RenderTargetFormat::NONE
                ? DXGI_FORMAT_UNKNOWN
                : texture_runtime.Format(PixelFormatFromRenderTargetFormat(format)).view;
    }

    if (state.depth_enabled != 0) {
        const auto depth_format = static_cast<Tegra::DepthFormat>(state.depth_format.Value());
        desc.DSVFormat = texture_runtime.Format(PixelFormatFromDepthFormat(depth_format)).dsv;
    }
    const bool has_depth = desc.DSVFormat != DXGI_FORMAT_UNKNOWN;
    if (dynamic.depth_bounds_enable != 0) {
        WarnOnce(warned_depth_bounds, "the depth bounds test");
    }
    desc.DepthStencilState = {
        .DepthEnable = has_depth && dynamic.depth_test_enable != 0,
        .DepthWriteMask = has_depth && dynamic.depth_write_enable != 0
                              ? D3D12_DEPTH_WRITE_MASK_ALL
                              : D3D12_DEPTH_WRITE_MASK_ZERO,
        .DepthFunc = dynamic.depth_test_enable != 0
                         ? MaxwellToD3D12::ComparisonFunc(dynamic.DepthTestFunc())
                         : D3D12_COMPARISON_FUNC_ALWAYS,
        .StencilEnable = has_depth && dynamic.stencil_enable != 0,
        .StencilReadMask = key.stencil_read_mask,
        .StencilWriteMask = key.stencil_write_mask,
        .FrontFace = StencilFace(dynamic.front),
        .BackFace = StencilFace(dynamic.back),
    };

    desc.SampleMask = UINT_MAX;
    desc.SampleDesc = {.Count = MaxwellToD3D12::SampleCount(state.msaa_mode), .Quality = 0};

    const HRESULT hr =
        device.Get()->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline_state));
    CheckRemovedAfter(device.Get(), [&] {
        return fmt::format("graphics PSO for VS {:016x} PS {:016x} (HRESULT 0x{:08X})",
                           key.unique_hashes[1], key.unique_hashes[5], static_cast<u32>(hr));
    });
    if (FAILED(hr)) {
        LOG_ERROR(Render,
                  "D3D12: CreateGraphicsPipelineState failed (HRESULT 0x{:08X}) for VS {:016x} "
                  "PS {:016x}: {} attributes, {} RTs (RT0 {}), DSV {}",
                  static_cast<u32>(hr), key.unique_hashes[1], key.unique_hashes[5],
                  elements.size(), num_attachments, static_cast<u32>(desc.RTVFormats[0]),
                  static_cast<u32>(desc.DSVFormat));
        pipeline_state.Reset();
        return;
    }
    LOG_INFO(Render,
             "D3D12: pipeline built for VS {:016x} PS {:016x} ({} attributes, {} RTs, RT0 {}, "
             "DSV {}, {} + {} descriptors)",
             key.unique_hashes[1], key.unique_hashes[5], elements.size(), num_attachments,
             static_cast<u32>(desc.RTVFormats[0]), static_cast<u32>(desc.DSVFormat),
             layout.NumResourceDescriptors(), layout.NumSamplerDescriptors());
}

} // namespace D3D12
