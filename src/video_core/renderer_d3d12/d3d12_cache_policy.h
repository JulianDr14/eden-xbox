// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include "video_core/texture_cache/gc_policy.h"
namespace D3D12 {
struct CacheMemorySnapshot {
    u64 app_used{}, app_limit{}, gpu_used{}, gpu_budget{};
    [[nodiscard]] bool Known() const { return app_limit != 0 || gpu_budget != 0; }
    [[nodiscard]] u64 AppFree() const { return app_limit > app_used ? app_limit - app_used : 0; }
};
enum class CachePressure : u8 { Normal, Pressure, Critical, Emergency };
/// GPU-thread-owned controller. Keep app and GPU domains separate; apply the worse pressure.
/// Missing queries retain the preceding level instead of silently relaxing an emergency.
class CachePressureController {
public:
    CachePressure Update(const CacheMemorySnapshot& s) {
        constexpr u64 MiB = 1024 * 1024;
        if (s.app_limit) {
            // A loading screen of a large game added 500 MiB of textures, buffers and DRAM in
            // ten seconds and exhausted 410 MiB of headroom before the old 256 MiB critical level
            // could evict the previous area's textures. Each level now starts 50% earlier.
            const u64 free = s.AppFree();
            auto next = free <= 192*MiB ? CachePressure::Emergency :
                        free <= 384*MiB ? CachePressure::Critical :
                        free <= 768*MiB ? CachePressure::Pressure : CachePressure::Normal;
            const u64 exit = app == CachePressure::Emergency ? 256*MiB :
                             app == CachePressure::Critical ? 512*MiB : 896*MiB;
            if (next >= app || free >= exit) app = next;
        }
        if (s.gpu_budget) {
            // Subtraction avoids overflow when comparing large budgets.
            const u64 budget = s.gpu_budget;
            auto next = s.gpu_used >= budget - (budget/100)*3 ? CachePressure::Emergency :
                        s.gpu_used >= budget - budget/10 ? CachePressure::Critical :
                        s.gpu_used >= budget - budget/5 ? CachePressure::Pressure : CachePressure::Normal;
            const u64 exit = gpu == CachePressure::Emergency ? budget - (budget/100)*8 :
                             gpu == CachePressure::Critical ? budget - (budget/100)*15 :
                             budget - budget/4;
            if (next >= gpu || s.gpu_used <= exit) gpu = next;
        }
        return std::max(app, gpu);
    }
    [[nodiscard]] static bool RequiresImmediateRecovery(const CacheMemorySnapshot& s) {
        return (s.app_limit != 0 && s.AppFree() < 64ULL * 1024 * 1024) ||
               (s.gpu_budget != 0 && s.gpu_used >= s.gpu_budget);
    }
    [[nodiscard]] static VideoCommon::TextureGcPolicy Policy(CachePressure level, bool second_pass,
                                                            bool immediate_recovery = false) {
        if (second_pass && level == CachePressure::Emergency) {
            // Hysteresis may retain Emergency with ample headroom. Only actual
            // exhaustion justifies unbounded recovery on the gameplay thread.
            return immediate_recovery ? VideoCommon::TextureGcPolicy{true, true, 10, 40, 0, 40}
                                      : VideoCommon::TextureGcPolicy{true, true, 10, 40, 1000, 1};
        }
        if (second_pass && level == CachePressure::Critical)
            return {true, true, 60, 16, 1000, 1};
        if (level != CachePressure::Normal)
            return {true, false, 120, 8, 1000, 0};
        return {false, false, 120, 4, 1000, 0};
    }
private:
    CachePressure app{}, gpu{};
};
}
