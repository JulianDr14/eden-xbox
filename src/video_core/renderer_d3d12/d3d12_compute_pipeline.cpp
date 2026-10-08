// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <bit>
#include <cstring>
#include <optional>

#include <boost/container/small_vector.hpp>

#include "common/bug_tracker.h"
#include "common/cityhash.h"
#include "common/logging.h"
#include "video_core/gpu_thread.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "video_core/engines/kepler_compute.h"
#include "video_core/memory_manager.h"
#include "video_core/renderer_d3d12/d3d12_barrier_batch.h"
#include "video_core/renderer_d3d12/d3d12_compute_pipeline.h"
#include "video_core/renderer_d3d12/d3d12_descriptor_heap.h"
#include "video_core/renderer_d3d12/d3d12_pipeline_helper.h"
#include "video_core/renderer_d3d12/d3d12_root_signature.h"
#include "video_core/shader_notify.h"
#include "video_core/surface.h"
#include "video_core/textures/texture.h"

namespace D3D12 {

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
      dxil_charge{Common::MemoryAccount::ShaderBytecode, dxil.capacity()}, info{info_} {
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

    ImageViewList views;
    SamplerIdList samplers;

    const auto& cbufs = qmd.const_buffer_config;
    const bool via_header_index = qmd.linked_tsc != 0;
    const auto read_handle = [&](const auto& desc, u32 index) {
        return ReadTextureHandle(gpu_memory, desc, index, via_header_index,
                                 [&](u32 cbuf) { return cbufs[cbuf].Address(); });
    };
    GatherStageViews(info, texture_cache, true, read_handle, views, samplers);
    texture_cache.FillImageViews(std::span(views.data(), views.size()), true);

    buffer_cache.UnbindComputeTextureBuffers();
    const ImageViewInOut* texture_buffer_it = views.data();
    BindStageTextureBuffers(info, texture_cache, texture_buffer_it,
                            [&](size_t index, ImageView& view,
                                VideoCore::Surface::PixelFormat format, bool is_written,
                                bool is_image) {
                                buffer_cache.BindComputeTextureBuffer(
                                    index, view.GpuAddr(), view.BufferSize(), format, is_written,
                                    is_image);
                            });

    buffer_cache.UpdateComputeBuffers();

    // The table is written in root-signature order: uniform, storage and texel buffers while the
    // buffer cache binds them, then textures and images.
    GuestDescriptorQueue& queue = context.descriptor_queue;
    queue.Acquire(layout.NumResourceDescriptors());
    buffer_cache.BindHostComputeBuffers();

    SamplerTableBuilder sampler_table;
    ImageTransitionList image_transitions;
    const SamplerId* samplers_it = samplers.data();
    const ImageViewInOut* views_it = views.data();
    views_it += Shader::NumDescriptors(info.texture_buffer_descriptors);
    views_it += Shader::NumDescriptors(info.image_buffer_descriptors);
    const bool has_integer_samplers = layout.IntegerSamplerIndex() != PipelineLayout::NO_TABLE;
    IntegerSamplerTable integer_samplers;
    u32 texture_binding = FirstTextureBinding(info, 0);
    for (const auto& desc : info.texture_descriptors) {
        for (u32 index = 0; index < desc.count; ++index, ++texture_binding) {
            const VideoCommon::ImageViewId view_id = (views_it++)->id;
            const ImageView& image_view = texture_cache.GetImageView(view_id);
            image_view.PrepareRead(desc.type);
            queue.AddCopy(image_view.Handle(desc.type));
            const Sampler& sampler = texture_cache.GetSampler(*(samplers_it++));
            sampler_table.Add(sampler);
            if (has_integer_samplers && desc.is_integer) {
                integer_samplers.Add(texture_binding, sampler, image_view);
            }
            image_transitions.emplace_back(view_id, false);
        }
    }
    PushStorageImages(info, texture_cache, queue, views_it, image_transitions);
    queue.FlushCopies();

    BarrierBatch barriers{context.scheduler};
    for (const auto& [view_id, is_storage] : image_transitions) {
        texture_cache.GetImageView(view_id).TransitionImage(
            is_storage ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
                       : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            &barriers);
    }
    barriers.Flush();

    out.resource_table = queue.Table();
    out.sampler_table = sampler_table.Table(context.sampler_heap);
    out.integer_samplers = integer_samplers.Upload(buffer_cache);

    // No resolution scaling yet.
    ResetPushConstants(out.push_constants);
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
        BUG_TRACK_KEY(PipelineRejected, unique_hash,
                      "CreateComputePipelineState failed (HRESULT 0x{:08X}) for {:016x}",
                      static_cast<u32>(hr), unique_hash);
        if (hr == E_OUTOFMEMORY)
            VideoCommon::GPUThread::ReportException("D3D12 compute PSO allocation exhausted memory");
        const Common::BugTracker::TapMute bug_tracker_mute;
        LOG_ERROR(Render, "D3D12: CreateComputePipelineState failed (HRESULT 0x{:08X}) for {:016x}",
                  static_cast<u32>(hr), unique_hash);
        pipeline_state.Reset();
        return;
    }
    pso_charge = Common::MemoryCharge(Common::MemoryAccount::PipelineStates, 0);
    LOG_INFO(Render, "D3D12: compute pipeline built for {:016x} ({} + {} descriptors)",
             unique_hash, layout.NumResourceDescriptors(), layout.NumSamplerDescriptors());
}

} // namespace D3D12
