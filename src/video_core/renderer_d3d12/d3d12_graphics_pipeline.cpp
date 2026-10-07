// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <atomic>
#include <bit>
#include <optional>
#include <cstddef>
#include <cstring>

#include <boost/container/small_vector.hpp>

#include "common/alignment.h"
#include "common/bug_tracker.h"
#include "common/cityhash.h"
#include "common/logging.h"
#include "video_core/gpu_thread.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "video_core/frame_trace.h"
#include "video_core/memory_manager.h"
#include "video_core/perf_counters.h"
#include "video_core/renderer_d3d12/d3d12_barrier_batch.h"
#include "video_core/renderer_d3d12/d3d12_descriptor_heap.h"
#include "video_core/renderer_d3d12/d3d12_graphics_pipeline.h"
#include "video_core/renderer_d3d12/d3d12_log.h"
#include "video_core/renderer_d3d12/d3d12_maxwell_to_d3d12.h"
#include "video_core/renderer_d3d12/d3d12_pipeline_helper.h"
#include "video_core/renderer_d3d12/d3d12_root_signature.h"
#include "video_core/renderer_d3d12/d3d12_texture_cache.h"
#include "video_core/renderer_d3d12/diagnostics/d3d12_pipeline_diagnostics.h"
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

/// Warns once per process about state D3D12 cannot express; the bug tracker counts every
/// occurrence (keyed by the flag, one per feature).
void WarnOnce(std::atomic_bool& flag, const char* what) {
    BUG_TRACK_KEY(UnsupportedState, reinterpret_cast<std::uintptr_t>(&flag),
                  "{} is not supported and is ignored", what);
    WarnOnceLog(flag, "{} is not supported and is ignored", what);
}
std::atomic_bool warned_logic_op;
std::atomic_bool warned_depth_bounds;
std::atomic_bool warned_point_fill;
std::atomic_bool warned_vertex_format;
std::atomic_bool warned_depth_feedback;
std::atomic_bool warned_conservative;

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
                                   std::function<SharedDxilStages(u64)> compile_dxil,
                                   const std::array<const Shader::Info*, NUM_STAGES>& infos,
                                   const PipelineLayout& layout_)
    : device{device_}, key{key_}, layout{layout_} {
    if (shader_notify) {
        shader_notify->MarkShaderBuilding();
    }
    for (size_t stage = 0; stage < NUM_STAGES; ++stage) {
        const Shader::Info* const info = infos[stage];
        if (!info) {
            continue;
        }
        has_stage[stage] = true;
        stage_infos[stage] = *info;
        enabled_uniform_buffer_masks[stage] = info->constant_buffer_mask;
        std::ranges::copy(info->constant_buffer_used_sizes, uniform_buffer_sizes[stage].begin());
        has_images |= !info->image_descriptors.empty();
    }
    namespace FT = VideoCore::FrameTrace;
    const u64 trace_pipeline = reinterpret_cast<uintptr_t>(this);
    FT::Mark(FT::Event::PipelineBuildRequested, trace_pipeline, key.Hash());
    auto func{[this, &texture_runtime, shader_notify, compile_dxil = std::move(compile_dxil),
               trace_pipeline] {
        FT::Mark(FT::Event::PipelineWorkerBegin, trace_pipeline);
        FT::ScopedSpan worker_span{FT::Event::PipelineWorkerLong, trace_pipeline};
        bool compiled = false;
        try {
            FT::ScopedSpan dxil_span{FT::Event::PipelineDxilLong, trace_pipeline};
            dxil = compile_dxil(trace_pipeline);
            compiled = true;
        } catch (const std::exception& exception) {
            // Handle() stays null: draws with this pipeline are skipped.
            BUG_TRACK_KEY(ShaderCompile, key.Hash(),
                          "DXIL for VS {:016x} PS {:016x} failed: {}", key.unique_hashes[1],
                          key.unique_hashes[5], exception.what());
            const Common::BugTracker::TapMute bug_tracker_mute;
            LOG_ERROR(Render, "D3D12: building the pipeline for VS {:016x} PS {:016x} failed: {}",
                      key.unique_hashes[1], key.unique_hashes[5], exception.what());
        }
        if (compiled) {
            Build(texture_runtime);
        }
        {
            std::scoped_lock lock{build_mutex};
            // Publish DXIL/PSO to IsBuilt's lock-free fast path as well as WaitBuilt.
            is_built.store(true, std::memory_order::release);
        }
        FT::Mark(FT::Event::PipelineBuildDone, trace_pipeline, pipeline_state.Get() != nullptr);
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

D3D12_GPU_VIRTUAL_ADDRESS IntegerSamplerTable::Upload(BufferCache& buffer_cache) const {
    if (rows.empty()) {
        return 0;
    }
    // The staging alignment (512) covers the root CBV's 256-byte placement.
    const size_t size = Common::AlignUp(num_bindings * sizeof(eden_integer_sampler_state),
                                        D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
    const StagingBufferRef ref = buffer_cache.runtime.UploadStagingBuffer(size);
    for (const auto& [binding, state] : rows) {
        std::memcpy(ref.mapped_span.data() + binding * sizeof(state), &state, sizeof(state));
    }
    return ref.buffer->GetGPUVirtualAddress() + ref.offset;
}

void GraphicsPipeline::WaitBuilt() {
    VideoCore::FrameTrace::ScopedSpan wait_span{
        VideoCore::FrameTrace::Event::PipelineWaitLong, reinterpret_cast<uintptr_t>(this)};
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

    ImageViewList views;
    SamplerIdList samplers;
    VideoCore::Perf::LapTimer lap;

    texture_cache.SynchronizeDescriptors(false);
    buffer_cache.SetUniformBuffersState(enabled_uniform_buffer_masks, &uniform_buffer_sizes);

    const bool via_header_index = regs.sampler_binding == Maxwell::SamplerBinding::ViaHeaderBinding;
    for (size_t stage = 0; stage < NUM_STAGES; ++stage) {
        if (!HasStage(stage)) {
            continue;
        }
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
            return ReadTextureHandle(gpu_memory, desc, index, via_header_index,
                                     [&](u32 cbuf) { return cbufs[cbuf].address; });
        };
        GatherStageViews(info, texture_cache, false, read_handle, views, samplers);
    }
    texture_cache.FillImageViews(std::span(views.data(), views.size()), false, has_images);
    lap.Lap(VideoCore::Perf::Counter::DrawTexturesNs);

    const ImageViewInOut* texture_buffer_it = views.data();
    for (size_t stage = 0; stage < NUM_STAGES; ++stage) {
        if (!HasStage(stage)) {
            continue;
        }
        const Shader::Info& info = stage_infos[stage];
        buffer_cache.UnbindGraphicsTextureBuffers(stage);
        BindStageTextureBuffers(info, texture_cache, texture_buffer_it,
                                [&](size_t index, ImageView& view,
                                    VideoCore::Surface::PixelFormat format, bool is_written,
                                    bool is_image) {
                                    buffer_cache.BindGraphicsTextureBuffer(
                                        stage, index, view.GpuAddr(), view.BufferSize(), format,
                                        is_written, is_image);
                                });
        texture_buffer_it += Shader::NumDescriptors(info.texture_descriptors);
        texture_buffer_it += Shader::NumDescriptors(info.image_descriptors);
    }

    buffer_cache.UpdateGraphicsBuffers(is_indexed);
    buffer_cache.BindHostGeometryBuffers(is_indexed);
    lap.Lap(VideoCore::Perf::Counter::DrawBuffersNs);

    // Resolve attachments before writing SRVs: a writable depth attachment must be sampled
    // through a GPU snapshot, not through its read-only DSV (which would suppress guest writes).
    VideoCommon::ImageId depth_image;
    {
        VideoCore::Perf::LapTimer target_lap;
        texture_cache.UpdateRenderTargets(false);
        target_lap.Lap(VideoCore::Perf::Counter::DrawUpdateTargetsNs);
        depth_image = texture_cache.GetFramebuffer()->DepthImageId();
        target_lap.Lap(VideoCore::Perf::Counter::DrawFramebufferNs);
    }
    lap.Lap(VideoCore::Perf::Counter::DrawTargetsNs);
    const auto& dynamic = key.state.dynamic_state;
    const bool writable_depth =
        (dynamic.depth_test_enable != 0 && dynamic.depth_write_enable != 0) ||
        (dynamic.stencil_enable != 0 && key.stencil_write_mask != 0);
    bool depth_feedback_attempted = false;
    bool depth_feedback_ready = false;

    // The table is written in root-signature order while the caches bind, stage by stage:
    // uniform, storage and texel buffers from the buffer cache, then textures and images.
    GuestDescriptorQueue& queue = context.descriptor_queue;
    queue.Acquire(layout.NumResourceDescriptors());
    SamplerTableBuilder sampler_table;
    ImageTransitionList image_transitions;
    bool uses_render_area = false;
    const bool has_integer_samplers = layout.IntegerSamplerIndex() != PipelineLayout::NO_TABLE;
    IntegerSamplerTable integer_samplers;
    u32 stage_binding = 0;
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
        u32 texture_binding = FirstTextureBinding(info, stage_binding);
        stage_binding += NumStageBindings(info);
        for (const auto& desc : info.texture_descriptors) {
            for (u32 index = 0; index < desc.count; ++index, ++texture_binding) {
                const VideoCommon::ImageViewId view_id = (views_it++)->id;
                const ImageView& image_view = texture_cache.GetImageView(view_id);
                const bool aliases_depth = depth_image != VideoCommon::ImageId{} &&
                                           image_view.image_id == depth_image;
                if (aliases_depth && writable_depth && !depth_feedback_attempted) {
                    depth_feedback_attempted = true;
                    depth_feedback_ready = image_view.SourceImage()->PrepareDepthFeedback() != nullptr;
                }
                if (aliases_depth && depth_feedback_ready) {
                    queue.AddCopy(image_view.DepthFeedbackHandle(desc.type));
                } else {
                    image_view.PrepareRead(desc.type);
                    queue.AddCopy(image_view.Handle(desc.type));
                }
                const Sampler& sampler = texture_cache.GetSampler(*(samplers_it++));
                sampler_table.Add(sampler);
                if (has_integer_samplers && desc.is_integer) {
                    integer_samplers.Add(texture_binding, sampler, image_view);
                }
                image_transitions.emplace_back(view_id, false);
                if (out.trace_views) {
                    out.trace_views->push_back(view_id);
                }
                if (out.trace_filters) {
                    out.trace_filters->push_back(sampler.Describe());
                }
                if (out.trace_types) {
                    out.trace_types->push_back(static_cast<u32>(desc.type));
                }
            }
        }
        PushStorageImages(info, texture_cache, queue, views_it, image_transitions);
        uses_render_area |= info.uses_render_area;
    }
    queue.FlushCopies();
    if (buffer_cache.any_buffer_uploaded) {
        buffer_cache.runtime.PostCopyBarrier();
        buffer_cache.any_buffer_uploaded = false;
    }
    lap.Lap(VideoCore::Perf::Counter::DrawDescriptorsNs);
    VideoCore::Perf::LapTimer target_lap;
    texture_cache.CheckFeedbackLoop(std::span<const ImageViewInOut>{views.data(), views.size()});
    target_lap.Lap(VideoCore::Perf::Counter::DrawFeedbackNs);

    // Depth-based effects sample the bound depth buffer: both uses then share a read-only state
    // (DEPTH_SAMPLED_STATE) and the draw binds the read-only DSV.
    BarrierBatch barriers{context.scheduler};
    for (const auto& [view_id, is_storage] : image_transitions) {
        ImageView& image_view = texture_cache.GetImageView(view_id);
        if (!is_storage && depth_image != VideoCommon::ImageId{} &&
            image_view.image_id == depth_image) {
            if (!depth_feedback_ready) {
                out.depth_sampled = true;
                image_view.TransitionImage(DEPTH_SAMPLED_STATE, &barriers);
            }
            continue;
        }
        image_view.TransitionImage(is_storage ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
                                              : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                   &barriers);
    }
    barriers.Flush();
    target_lap.Lap(VideoCore::Perf::Counter::DrawImageTransitionsNs);
    if (out.depth_sampled && writable_depth) {
        WarnOnce(warned_depth_feedback, "depth feedback snapshot allocation failed; depth writes");
    }
    lap.Lap(VideoCore::Perf::Counter::DrawTargetsNs);

    out.resource_table = queue.Table();
    out.sampler_table = sampler_table.Table(context.sampler_heap);
    out.integer_samplers = integer_samplers.Upload(buffer_cache);
    lap.Lap(VideoCore::Perf::Counter::DrawSamplersNs);

    // Push constants: no resolution scaling yet (all rescaling bits clear, down factor 1), then
    // the render area, which shares the first words exactly as in the Vulkan backend.
    ResetPushConstants(out.push_constants);
    if (uses_render_area) {
        out.push_constants[0] = std::bit_cast<u32>(static_cast<f32>(regs.surface_clip.width));
        out.push_constants[1] = std::bit_cast<u32>(static_cast<f32>(regs.surface_clip.height));
    }
}

void GraphicsPipeline::Build(const TextureCacheRuntime& texture_runtime) {
    using VideoCore::Surface::PixelFormatFromDepthFormat;
    using VideoCore::Surface::PixelFormatFromRenderTargetFormat;

    const auto& stages = *dxil;
    const FixedPipelineState& state = key.state;
    const FixedPipelineState::DynamicState& dynamic = state.dynamic_state;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = layout.Handle();
    desc.VS = Bytecode(stages[0]);
    desc.HS = Bytecode(stages[1]);
    desc.DS = Bytecode(stages[2]);
    desc.GS = Bytecode(stages[3]);
    desc.PS = Bytecode(stages[4]);

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
            BUG_TRACK_KEY(UnsupportedFormat,
                          static_cast<u64>(attribute.Type()) << 8 |
                              static_cast<u64>(attribute.Size()),
                          "vertex attribute type {} size {} has no DXGI format; read as RGBA32F",
                          static_cast<u32>(attribute.Type()), static_cast<u32>(attribute.Size()));
            WarnOnceLog(warned_vertex_format, "{} is not supported and is ignored",
                        "a vertex attribute format (read as RGBA32F)");
            format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        }
        const u32 slot = attribute.buffer;
        const u32 divisor = state.binding_divisors[slot];
        // Part k of an attribute fetched in parts is generic N + 32k (MakeRuntimeInfo).
        const MaxwellToD3D12::AttributeFetch fetch = MaxwellToD3D12::SplitAttributeFetch(
            attribute.Type(), attribute.Size(), attribute.offset, state.vertex_strides[slot]);
        if (fetch.parts > 1) {
            format = fetch.part_format;
        }
        for (u32 part = 0; part < fetch.parts; ++part) {
            elements.push_back({
                .SemanticName = "TEXCOORD",
                .SemanticIndex = static_cast<UINT>(index + part * Shader::IR::NUM_GENERICS),
                .Format = format,
                .InputSlot = slot,
                .AlignedByteOffset = attribute.offset + part * fetch.part_bytes,
                .InputSlotClass = divisor != 0 ? D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA
                                               : D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
                .InstanceDataStepRate = divisor,
            });
        }
    }
    desc.InputLayout = {.pInputElementDescs = elements.empty() ? nullptr : elements.data(),
                        .NumElements = static_cast<UINT>(elements.size())};
    desc.IBStripCutValue = static_cast<D3D12_INDEX_BUFFER_STRIP_CUT_VALUE>(key.strip_cut);

    const bool has_tessellation = !stages[1].empty() || !stages[2].empty();
    desc.PrimitiveTopologyType = MaxwellToD3D12::PrimitiveTopologyType(state.topology);
    if (has_tessellation) {
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
    } else if (desc.PrimitiveTopologyType == D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH) {
        BUG_TRACK(UnsupportedState, "patch topology without tessellation drawn as points");
        const Common::BugTracker::TapMute bug_tracker_mute;
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

    VideoCore::FrameTrace::ScopedSpan pso_span{
        VideoCore::FrameTrace::Event::PipelinePsoLong, reinterpret_cast<uintptr_t>(this)};
    const HRESULT hr =
        device.Get()->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline_state));
    pso_span.Finish();
    CheckRemovedAfter(device.Get(), [&] {
        return fmt::format("graphics PSO for VS {:016x} PS {:016x} (HRESULT 0x{:08X})",
                           key.unique_hashes[1], key.unique_hashes[5], static_cast<u32>(hr));
    });
    if (FAILED(hr)) {
        BUG_TRACK_KEY(PipelineRejected, key.Hash(),
                      "CreateGraphicsPipelineState failed (HRESULT 0x{:08X}) for VS {:016x} PS "
                      "{:016x}: {} attributes, {} RTs (RT0 {}), DSV {}",
                      static_cast<u32>(hr), key.unique_hashes[1], key.unique_hashes[5],
                      elements.size(), num_attachments, static_cast<u32>(desc.RTVFormats[0]),
                      static_cast<u32>(desc.DSVFormat));
        const Common::BugTracker::TapMute bug_tracker_mute;
        LOG_ERROR(Render,
                  "D3D12: CreateGraphicsPipelineState failed (HRESULT 0x{:08X}) for VS {:016x} "
                  "PS {:016x}: {} attributes, {} RTs (RT0 {}), DSV {}, features VS {:x} PS {:x}",
                  static_cast<u32>(hr), key.unique_hashes[1], key.unique_hashes[5],
                  elements.size(), num_attachments, static_cast<u32>(desc.RTVFormats[0]),
                  static_cast<u32>(desc.DSVFormat), ShaderFeatureFlags(stages[0]),
                  ShaderFeatureFlags(stages[4]));
        if (hr == E_OUTOFMEMORY) {
            VideoCommon::GPUThread::ReportException("D3D12 graphics PSO allocation exhausted memory");
        } else {
            DiagnoseFailedPipeline(device.Get(), desc);
        }
        pipeline_state.Reset();
        return;
    }
    LOG_INFO(Render,
             "D3D12: pipeline built for VS {:016x} PS {:016x} ({} attributes, {} RTs, RT0 {}, "
             "DSV {}, {} + {} descriptors, features VS {:x} PS {:x})",
             key.unique_hashes[1], key.unique_hashes[5], elements.size(), num_attachments,
             static_cast<u32>(desc.RTVFormats[0]), static_cast<u32>(desc.DSVFormat),
             layout.NumResourceDescriptors(), layout.NumSamplerDescriptors(),
             ShaderFeatureFlags(stages[0]), ShaderFeatureFlags(stages[4]));
}

} // namespace D3D12
