// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video_core/renderer_d3d12/d3d12_fence_manager.h"
#include "video_core/renderer_d3d12/d3d12_scheduler.h"

namespace D3D12 {

InnerFence::InnerFence(Scheduler& scheduler_, bool is_stubbed_)
    : FenceBase{is_stubbed_}, scheduler{scheduler_} {}

void InnerFence::Queue() {
    if (is_stubbed) return;
    wait_tick = scheduler.CurrentTick();
    scheduler.Flush();
}

bool InnerFence::IsSignaled() const {
    return is_stubbed || scheduler.IsFree(wait_tick);
}

void InnerFence::Wait() {
    if (!is_stubbed) scheduler.Wait(wait_tick);
}

FenceManager::FenceManager(VideoCore::RasterizerInterface& rasterizer, Tegra::GPU& gpu,
                           TextureCache& texture_cache, BufferCache& buffer_cache,
                           QueryCache& query_cache, Scheduler& scheduler_)
    : VideoCommon::FenceManager<FenceManagerParams>{rasterizer, gpu, texture_cache, buffer_cache,
                                                    query_cache},
      scheduler{scheduler_} {}

Fence FenceManager::CreateFence(bool is_stubbed) {
    return std::make_shared<InnerFence>(scheduler, is_stubbed);
}
void FenceManager::QueueFence(Fence& fence) { fence->Queue(); }
bool FenceManager::IsFenceSignaled(Fence& fence) const { return fence->IsSignaled(); }
void FenceManager::WaitFence(Fence& fence) { fence->Wait(); }

} // namespace D3D12
