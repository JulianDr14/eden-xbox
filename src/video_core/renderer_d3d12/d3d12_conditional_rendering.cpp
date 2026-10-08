// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <span>

#include "common/logging.h"
#include "video_core/memory_manager.h"
#include "video_core/renderer_d3d12/d3d12_conditional_rendering.h"
#include "video_core/renderer_d3d12/d3d12_query_cache.h"
#include "video_core/renderer_d3d12/d3d12_resource_utils.h"
#include "video_core/renderer_d3d12/d3d12_scheduler.h"

namespace D3D12 {
namespace {

using RenderEnable = Tegra::Engines::Maxwell3D::Regs::RenderEnable;

/// Holds the largest ResolveQueryData result (pipeline statistics); 256 bytes is the smallest
/// buffer placement anyway.
constexpr u64 PREDICATE_SIZE = 256;

/// What "lhs == rhs" comes down to.
struct Equality {
    enum class Kind {
        Unknown,   ///< needs more than one slice (left to the CPU)
        True,      ///< known on the CPU
        False,     ///< known on the CPU
        SliceZero, ///< equal exactly when slice counted zero
    };
    Kind kind;
    const HostCounter* slice{};
};

/// The slices of report that other does not count too, newest first.
std::span<const std::shared_ptr<HostCounter>> OwnSlices(const PendingReport& report,
                                                        const PendingReport& other) {
    const auto first = report.slices.begin();
    const auto last = first + report.num_slices;
    const auto other_last = other.slices.begin() + other.num_slices;
    // Slices form a chain: once one is shared, so are the ones below it.
    const auto shared = std::find_if(first, last, [&](const std::shared_ptr<HostCounter>& slice) {
        return std::find(other.slices.begin(), other_last, slice) != other_last;
    });
    return {first, shared};
}

Equality CompareEqual(const PendingReport& lhs, const PendingReport& rhs) {
    using enum Equality::Kind;
    if (!lhs.complete || !rhs.complete) {
        return {Unknown};
    }
    // A later report of a counter that was not reset counts on from the earlier one: the
    // slices both count cancel out (the before/after pattern around a bounding volume).
    const auto lhs_own = OwnSlices(lhs, rhs);
    const auto rhs_own = OwnSlices(rhs, lhs);
    if (lhs_own.empty() && rhs_own.empty()) {
        return {lhs.known == rhs.known ? True : False};
    }
    if (lhs_own.size() + rhs_own.size() != 1) {
        return {Unknown};
    }
    const bool lhs_counts = !lhs_own.empty();
    const PendingReport& counting = lhs_counts ? lhs : rhs;
    const PendingReport& fixed = lhs_counts ? rhs : lhs;
    // Counters never decrease: the counting report is at least what is known of it.
    if (counting.known > fixed.known) {
        return {False};
    }
    if (counting.known < fixed.known) {
        return {Unknown};
    }
    return {SliceZero, (lhs_counts ? lhs_own : rhs_own).front().get()};
}

} // namespace

ConditionalRendering::ConditionalRendering(const Device& device, Scheduler& scheduler_,
                                           QueryCache& query_cache_)
    : scheduler{scheduler_}, query_cache{query_cache_},
      predicate{CreateCommittedBuffer(device.Get(), PREDICATE_SIZE, D3D12_HEAP_TYPE_DEFAULT,
                                      D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_NONE,
                                      "CreateCommittedResource (predicate)")} {}

bool ConditionalRendering::Accelerate(const Tegra::Engines::Maxwell3D::Regs& regs,
                                      Tegra::MemoryManager& gpu_memory) {
    mode = Mode::Render;
    // The fixed modes do not read memory: the CPU evaluates them without waiting.
    if (regs.render_enable_override != RenderEnable::Override::UseRenderEnable) {
        return false;
    }
    const RenderEnable::Mode guest_mode = regs.render_enable.mode;
    if (guest_mode != RenderEnable::Mode::Conditional &&
        guest_mode != RenderEnable::Mode::IfEqual && guest_mode != RenderEnable::Mode::IfNotEqual) {
        return false;
    }
    const GPUVAddr address = regs.render_enable.Address();
    // Conditional renders when the 64-bit report is not zero (as Ryujinx); the others compare
    // the reports at address and address + 16.
    const PendingReport lhs = Report(address, gpu_memory);
    const PendingReport rhs = guest_mode == RenderEnable::Mode::Conditional
                                  ? PendingReport{}
                                  : Report(address + 16, gpu_memory);
    const Equality equality = CompareEqual(lhs, rhs);
    const bool render_if_equal = guest_mode == RenderEnable::Mode::IfEqual;
    switch (equality.kind) {
    case Equality::Kind::Unknown:
        if (!logged_cpu) {
            LOG_WARNING(Render,
                        "D3D12: a render condition (mode {}) needs more than one query slice; "
                        "the CPU waits for its result",
                        static_cast<u32>(guest_mode));
            logged_cpu = true;
        }
        return false;
    case Equality::Kind::True:
    case Equality::Kind::False:
        mode = (equality.kind == Equality::Kind::True) == render_if_equal ? Mode::Render
                                                                           : Mode::Skip;
        return true;
    case Equality::Kind::SliceZero:
        Predicate(*equality.slice, render_if_equal);
        if (!logged_gpu) {
            LOG_INFO(Render, "D3D12: render conditions evaluated on the GPU (mode {})",
                     static_cast<u32>(guest_mode));
            logged_gpu = true;
        }
        return true;
    }
    return false;
}

PendingReport ConditionalRendering::Report(GPUVAddr address, Tegra::MemoryManager& gpu_memory) {
    if (const std::optional<DAddr> cpu_address = gpu_memory.GpuToCpuAddress(address)) {
        if (std::optional<PendingReport> cached = query_cache.PeekReport(*cpu_address)) {
            return std::move(*cached);
        }
    }
    // Not a query the host counts: read it as the CPU path does.
    PendingReport report;
    gpu_memory.ReadBlock(address, &report.known, sizeof(report.known));
    return report;
}

void ConditionalRendering::Predicate(const HostCounter& slice, bool render_if_zero) {
    ID3D12GraphicsCommandList* const cmd = scheduler.CommandList();
    const u64 tick = scheduler.CurrentTick();
    // Earlier predicated draws have read the buffer when this resolve overwrites it: SetPredication
    // takes the value when it executes, and the barrier orders the write after those reads.
    const D3D12_RESOURCE_STATES current =
        predicate_tick == tick ? D3D12_RESOURCE_STATE_PREDICATION : D3D12_RESOURCE_STATE_COMMON;
    TransitionResource(cmd, predicate.Get(), current, D3D12_RESOURCE_STATE_COPY_DEST);
    const QueryPool::Slot& slot = slice.Slot();
    cmd->ResolveQueryData(slot.heap, slice.HostType(), slot.index, 1, predicate.Get(), 0);
    TransitionResource(cmd, predicate.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                       D3D12_RESOURCE_STATE_PREDICATION);
    predicate_tick = tick;
    value_offset = slice.ValueOffset();
    // The op names when draws are skipped.
    op = render_if_zero ? D3D12_PREDICATION_OP_NOT_EQUAL_ZERO : D3D12_PREDICATION_OP_EQUAL_ZERO;
    mode = Mode::Predicated;
}

void ConditionalRendering::Bind(ID3D12GraphicsCommandList* cmd) {
    const u64 tick = scheduler.CurrentTick();
    if (predicate_tick != tick) {
        TransitionResource(cmd, predicate.Get(), D3D12_RESOURCE_STATE_COMMON,
                           D3D12_RESOURCE_STATE_PREDICATION);
        predicate_tick = tick;
    }
    cmd->SetPredication(predicate.Get(), value_offset, op);
}

} // namespace D3D12
