// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstring>

#include "common/cityhash.h"
#include "common/logging.h"
#include "video_core/renderer_d3d12/d3d12_compute_pipeline.h"
#include "video_core/renderer_d3d12/d3d12_root_signature.h"
#include "video_core/shader_notify.h"

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
      info{info_} {
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

void ComputePipeline::Build() {
    const D3D12_COMPUTE_PIPELINE_STATE_DESC desc{
        .pRootSignature = layout.Handle(),
        .CS = {.pShaderBytecode = dxil.data(), .BytecodeLength = dxil.size()},
        .NodeMask = 0,
        .CachedPSO = {},
        .Flags = D3D12_PIPELINE_STATE_FLAG_NONE,
    };
    const HRESULT hr = device.Get()->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pipeline_state));
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
