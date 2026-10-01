// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cassert>
#include <limits>
#include <iostream>
#include "video_core/renderer_d3d12/d3d12_cache_policy.h"
using namespace D3D12;
constexpr u64 MiB = 1024*1024;
int main() {
    CachePressureController c;
    auto sample = [](u64 free) { return CacheMemorySnapshot{5120*MiB-free,5120*MiB,530*MiB,3447*MiB}; };
    // A large CPU/JIT allocation is not GPU usage: 600 MiB actual headroom is normal.
    assert(c.Update(sample(600*MiB)) == CachePressure::Normal);
    assert(c.Update(sample(512*MiB)) == CachePressure::Pressure);
    assert(c.Update(sample(600*MiB)) == CachePressure::Pressure);
    assert(c.Update(sample(640*MiB)) == CachePressure::Normal);
    assert(c.Update(sample(256*MiB)) == CachePressure::Critical);
    assert(c.Update(sample(350*MiB)) == CachePressure::Critical);
    assert(c.Update(sample(384*MiB)) == CachePressure::Pressure);
    assert(c.Update(sample(128*MiB)) == CachePressure::Emergency);
    assert(c.Update(sample(180*MiB)) == CachePressure::Emergency);
    assert(c.Update(sample(192*MiB)) == CachePressure::Critical);
    auto exhausted = sample(0); exhausted.app_used += MiB;
    assert(exhausted.AppFree() == 0);
    assert(c.Update(exhausted) == CachePressure::Emergency);
    assert(c.Update({}) == CachePressure::Emergency); // failed queries do not relax pressure
    CachePressureController gpu;
    assert(gpu.Update({0,0,970*MiB,1000*MiB}) == CachePressure::Emergency);
    assert(gpu.Update({0,0,950*MiB,1000*MiB}) == CachePressure::Emergency);
    assert(gpu.Update({0,0,920*MiB,1000*MiB}) == CachePressure::Critical);
    assert(gpu.Update({0,0,850*MiB,1000*MiB}) == CachePressure::Pressure);
    assert(gpu.Update({0,0,750*MiB,1000*MiB}) == CachePressure::Normal);
    // OS budget shrink must trigger even when the app has ample CPU RAM.
    assert(gpu.Update({1000*MiB,5120*MiB,530*MiB,540*MiB}) == CachePressure::Emergency);
    CachePressureController large;
    const auto maximum = std::numeric_limits<u64>::max();
    assert(large.Update({0,0,maximum,maximum}) == CachePressure::Emergency);
    auto critical = c.Policy(CachePressure::Critical,true);
    auto emergency = c.Policy(CachePressure::Emergency,true);
    assert(critical.min_age == 60 && critical.max_downloads == 1 && critical.time_budget_us == 1000);
    assert(emergency.min_age == 10 && emergency.max_downloads == 40 && emergency.time_budget_us == 0);
    assert(!c.Policy(CachePressure::Critical,false).aggressive);
    assert(c.Policy(CachePressure::Normal,false).min_age >= critical.min_age);
    std::cout << "PASS: independent domains, app/GPU budget exhaustion, hysteresis, query failure, overflow and emergency recovery policy\n";
}
