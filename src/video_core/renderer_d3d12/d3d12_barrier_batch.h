// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <boost/container/small_vector.hpp>

#include "video_core/renderer_d3d12/d3d12_resource_utils.h"
#include "video_core/renderer_d3d12/d3d12_scheduler.h"

namespace D3D12 {

/// Collects resource barriers and records them with one ResourceBarrier call, which lets the
/// driver resolve them together instead of one by one. Flush before recording any command that
/// touches a resource with a pending barrier.
class BarrierBatch {
public:
    explicit BarrierBatch(Scheduler& scheduler_) : scheduler{scheduler_} {}
    ~BarrierBatch() {
        Flush();
    }

    BarrierBatch(const BarrierBatch&) = delete;
    BarrierBatch& operator=(const BarrierBatch&) = delete;

    void Add(const D3D12_RESOURCE_BARRIER& barrier) {
        barriers.push_back(barrier);
    }

    void Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                    D3D12_RESOURCE_STATES after,
                    UINT subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES) {
        if (before != after) {
            barriers.push_back(TransitionBarrier(resource, before, after, subresource));
        }
    }

    void Uav(ID3D12Resource* resource) {
        barriers.push_back(UavBarrier(resource));
    }

    void Flush() {
        if (!barriers.empty()) {
            scheduler.CommandList()->ResourceBarrier(static_cast<UINT>(barriers.size()),
                                                     barriers.data());
            barriers.clear();
        }
    }

private:
    Scheduler& scheduler;
    boost::container::small_vector<D3D12_RESOURCE_BARRIER, 16> barriers;
};

} // namespace D3D12
