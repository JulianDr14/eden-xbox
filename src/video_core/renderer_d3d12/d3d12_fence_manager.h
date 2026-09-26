// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <memory>

#include "video_core/fence_manager.h"
#include "video_core/renderer_d3d12/d3d12_buffer_cache.h"
#include "video_core/renderer_d3d12/d3d12_query_cache.h"
#include "video_core/renderer_d3d12/d3d12_texture_cache.h"

namespace D3D12 {

class Scheduler;

class InnerFence final : public VideoCommon::FenceBase {
public:
    InnerFence(Scheduler& scheduler, bool is_stubbed);
    void Queue();
    [[nodiscard]] bool IsSignaled() const;
    void Wait();

private:
    Scheduler& scheduler;
    u64 wait_tick{};
};

using Fence = std::shared_ptr<InnerFence>;

struct FenceManagerParams {
    using FenceType = Fence;
    using BufferCacheType = BufferCache;
    using TextureCacheType = TextureCache;
    using QueryCacheType = QueryCache;
    static constexpr bool HAS_ASYNC_CHECK = true;
};

class FenceManager final : public VideoCommon::FenceManager<FenceManagerParams> {
public:
    FenceManager(VideoCore::RasterizerInterface& rasterizer, Tegra::GPU& gpu,
                 TextureCache& texture_cache, BufferCache& buffer_cache, QueryCache& query_cache,
                 Scheduler& scheduler);

protected:
    Fence CreateFence(bool is_stubbed) override;
    void QueueFence(Fence& fence) override;
    bool IsFenceSignaled(Fence& fence) const override;
    void WaitFence(Fence& fence) override;

private:
    Scheduler& scheduler;
};

} // namespace D3D12
