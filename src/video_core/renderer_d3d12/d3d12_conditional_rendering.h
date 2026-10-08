// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "video_core/engines/maxwell_3d.h"
#include "video_core/renderer_d3d12/d3d12_device.h"

namespace Tegra {
class MemoryManager;
}

namespace D3D12 {

class HostCounter;
class QueryCache;
class Scheduler;
struct PendingReport;

/// Guest conditional rendering (render enable) evaluated by the GPU with SetPredication.
///
/// Without it, Maxwell3D reads the guest's query reports back on the CPU, which waits for the GPU
/// to finish the query (QueryCacheLegacy flushes it) every time a condition is set. Here the
/// condition is reduced to whether one host query slice counted zero, or to a result already known
/// on the CPU; the slice is resolved into a predicate buffer where the guest sets the condition,
/// and the guest's draws and clears are predicated on it. Conditions that need more than one
/// slice are left to the CPU.
///
/// D3D12 also predicates copies, clears and dispatches, so predication is only set around guest
/// draws and clears (Scope), never across the texture and buffer caches' own work.
class ConditionalRendering {
public:
    explicit ConditionalRendering(const Device& device, Scheduler& scheduler,
                                  QueryCache& query_cache);

    /// Evaluates the guest's render enable (RasterizerInterface::AccelerateConditionalRendering).
    /// False when the CPU has to evaluate it.
    bool Accelerate(const Tegra::Engines::Maxwell3D::Regs& regs, Tegra::MemoryManager& gpu_memory);

    /// Whether guest draws and clears are known to be disabled: they are dropped unrecorded.
    [[nodiscard]] bool SkipsDraws() const noexcept {
        return mode == Mode::Skip;
    }

    /// Predicates the guest draw or clear recorded while it lives.
    class [[nodiscard]] Scope {
    public:
        Scope(ConditionalRendering& owner, ID3D12GraphicsCommandList* cmd)
            : list{owner.mode == Mode::Predicated ? cmd : nullptr} {
            if (list) {
                owner.Bind(list);
            }
        }
        ~Scope() {
            if (list) {
                list->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
            }
        }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

    private:
        ID3D12GraphicsCommandList* list;
    };

private:
    enum class Mode { Render, Skip, Predicated };

    /// Guest reports at the condition's address: cached queries or values in guest memory.
    PendingReport Report(GPUVAddr address, Tegra::MemoryManager& gpu_memory);
    /// Draws render only when slice counted zero (or only when it did not).
    void Predicate(const HostCounter& slice, bool render_if_zero);
    void Bind(ID3D12GraphicsCommandList* cmd);

    Scheduler& scheduler;
    QueryCache& query_cache;
    /// The slice's resolved data. In PREDICATION while predicate_tick is the list being
    /// recorded, else COMMON (buffers decay to it after each submission).
    ComPtr<ID3D12Resource> predicate;
    u64 predicate_tick{};
    u64 value_offset{};
    D3D12_PREDICATION_OP op{};
    Mode mode{Mode::Render};
    bool logged_gpu{};
    bool logged_cpu{};
};

} // namespace D3D12
