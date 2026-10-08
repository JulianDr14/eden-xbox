// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>

// Pure policy for the D3D12 memory guard: no device, clock or allocation, so every decision is
// covered by tools/xbox/tests/memory-guard.cpp.
//
// What the platform does (Windows.System.MemoryManager): AppMemoryUsage is the app's commit and is
// exactly what the Resource Manager compares with AppMemoryUsageLimit. A commit beyond the limit
// fails (CreateCommittedResource returns E_OUTOFMEMORY), and on Xbox an app left over its limit is
// suspended after two seconds. So the guard keeps headroom ahead of allocations instead of reacting
// after them, and a refused commit is answered by reclaiming memory and retrying once.
namespace D3D12 {

namespace MemoryGuard {
inline constexpr std::uint64_t MiB = 1024 * 1024;
} // namespace MemoryGuard

/// Staging references identify dedicated buffers by unique ID, never by their vector position.
/// Swap removal bounds CPU work even when a size bucket contains thousands of busy buffers.
/// max_checks bounds the per-frame sweep; recovery from a refused allocation passes no bound.
template <class Entries, class IsRetired>
bool ReclaimRetiredStaging(Entries& entries, std::size_t& cursor, IsRetired&& retired,
                           std::size_t max_checks = 16) {
    bool changed = false;
    if (cursor >= entries.size()) {
        cursor = 0;
    }
    for (std::size_t checked = 0; checked < max_checks && cursor < entries.size(); ++checked) {
        if (retired(entries[cursor])) {
            if (cursor + 1 != entries.size()) {
                entries[cursor] = std::move(entries.back());
            }
            entries.pop_back();
            changed = true;
        } else {
            ++cursor;
        }
    }
    if (cursor >= entries.size()) {
        cursor = 0;
    }
    return changed;
}

/// Bytes a dedicated staging buffer is created with. Up to 1 MiB, a power of two (D3D12 commits
/// whole 64 KiB pages anyway). Above it, sixteenths of the next power of two: less than 12.5 % over
/// the request instead of up to 100 % (a 17 MiB upload used to commit 32 MiB, now 18). Buckets
/// stay indexed by Log2Ceil(request), so a bucket holds at most sixteen distinct sizes.
[[nodiscard]] constexpr std::uint64_t DedicatedStagingSize(std::uint64_t size) {
    size = (std::max<std::uint64_t>)(size, 1);
    const std::uint64_t pow2 = std::bit_ceil(size);
    if (pow2 <= MemoryGuard::MiB) {
        return pow2;
    }
    const std::uint64_t step = pow2 / 16;
    return (size + step - 1) / step * step;
}

/// Target size of the staging stream ring. It shrinks immediately with the app's headroom and grows
/// back one step at a time (32 -> 64 -> 128 -> 256 MiB) only after sustained headroom. Each shrink
/// that follows a growth doubles the time required for the next one, so a game living near the
/// limit settles instead of oscillating.
///
/// It never goes below FLOOR_RING. Without a ring every upload, however small, becomes a dedicated
/// committed buffer of at least 64 KiB: more commit than the ring saved, and about 0.6 ms each.
/// A game at the limit then created 1400 of them a frame and ran at one frame a second.
class MemoryGuardPolicy {
public:
    static constexpr std::uint64_t MiB = MemoryGuard::MiB;
    static constexpr std::uint64_t EMERGENCY_FREE = 32 * MiB; ///< Last resort: drain and release.
    static constexpr std::uint64_t FLOOR_RING = 32 * MiB;     ///< Kept even in an emergency.
    static constexpr std::uint64_t TIGHT_FREE = 64 * MiB;
    static constexpr std::uint64_t LOW_FREE = 128 * MiB; ///< Also where optional caches trim.
    static constexpr std::uint64_t GROW_HEADROOM = 256 * MiB; ///< Left free after a growth step.
    static constexpr unsigned GROW_FRAMES = 120;              ///< Two seconds at 60 FPS.
    static constexpr unsigned MAX_GROW_FRAMES = 7200;         ///< Two minutes.

    struct Decision {
        std::uint64_t stream_bytes;
        bool trim;
        bool emergency;
    };

    explicit MemoryGuardPolicy(std::uint64_t max_ring_ = 256 * MiB)
        : max_ring{max_ring_}, floor_ring{(std::min)(FLOOR_RING, max_ring_)}, target{max_ring_} {}

    Decision Update(std::uint64_t used, std::uint64_t limit) {
        if (!limit) {
            // A failed measurement never implies headroom: keep the target, restart the count.
            healthy_frames = 0;
            return {target, false, false};
        }
        const std::uint64_t free = used >= limit ? 0 : limit - used;
        const bool emergency = free <= EMERGENCY_FREE;
        const std::uint64_t cap = emergency           ? floor_ring
                                  : free <= TIGHT_FREE ? TIGHT_FREE
                                  : free <= LOW_FREE   ? LOW_FREE
                                                       : max_ring;
        if (cap < target) {
            target = cap;
            healthy_frames = 0;
            if (grew_last) {
                grow_frames = (std::min)(grow_frames * 2, MAX_GROW_FRAMES);
                grew_last = false;
            }
        } else if (target < max_ring) {
            const std::uint64_t next = (std::min)(target * 2, max_ring);
            if (free >= next + GROW_HEADROOM) {
                if (++healthy_frames >= grow_frames) {
                    target = next;
                    healthy_frames = 0;
                    grew_last = true;
                }
            } else {
                healthy_frames = 0;
            }
        }
        return {target, free <= LOW_FREE, emergency};
    }

    [[nodiscard]] unsigned GrowFrames() const noexcept {
        return grow_frames;
    }

    [[nodiscard]] std::uint64_t FloorRing() const noexcept {
        return floor_ring;
    }

private:
    std::uint64_t max_ring;
    std::uint64_t floor_ring;
    std::uint64_t target;
    unsigned healthy_frames{};
    unsigned grow_frames{GROW_FRAMES};
    bool grew_last{};
};

/// Frame-boundary state machine of the staging ring, driven by StagingBufferPool::GuardMemory:
///   1. BeginFrame: new target; whether the GPU must be drained (once per emergency) and whether
///      retired dedicated buffers must be trimmed.
///   2. While Retiring(), the pool hands out no ring memory and releases the ring once the GPU has
///      passed every tick that used it (immediately after a drain), then calls OnRetired().
///   3. WantedRing: bytes of the replacement ring, or 0. Old and new rings never coexist, and the
///      replacement needs its size plus RING_MARGIN free, except a floor ring: uploads without one
///      commit more than it does. A refused allocation backs off.
class StagingRingController {
public:
    static constexpr std::uint64_t RING_MARGIN = 64 * MemoryGuard::MiB;
    static constexpr unsigned RETRY_FRAMES = 120;

    struct Plan {
        std::uint64_t previous_target;
        std::uint64_t target;
        bool finish;
        bool trim;
        bool emergency;
    };

    explicit StagingRingController(std::uint64_t max_ring = 256 * MemoryGuard::MiB)
        : policy{max_ring}, target{max_ring} {}

    Plan BeginFrame(std::uint64_t used, std::uint64_t limit, std::uint64_t ring_bytes) {
        if (retry_frames) {
            --retry_frames;
        }
        const auto decision = policy.Update(used, limit);
        Plan plan{
            .previous_target = target,
            .target = decision.stream_bytes,
            // Draining only speeds up releasing a ring larger than the floor.
            .finish = decision.emergency && ring_bytes > decision.stream_bytes &&
                      !emergency_finished,
            .trim = decision.trim,
            .emergency = decision.emergency,
        };
        target = decision.stream_bytes;
        retiring = ring_bytes != 0 && ring_bytes != target;
        if (plan.finish) {
            emergency_finished = true;
        }
        if (!decision.emergency) {
            emergency_finished = false;
        }
        return plan;
    }

    [[nodiscard]] bool Retiring() const noexcept {
        return retiring;
    }

    void OnRetired() noexcept {
        retiring = false;
    }

    [[nodiscard]] std::uint64_t WantedRing(std::uint64_t ring_bytes, std::uint64_t used,
                                           std::uint64_t limit) const noexcept {
        if (ring_bytes != 0 || target == 0 || retry_frames != 0 || limit == 0) {
            return 0;
        }
        if (target <= policy.FloorRing()) {
            return target;
        }
        const std::uint64_t free = used >= limit ? 0 : limit - used;
        return free >= target + RING_MARGIN ? target : 0;
    }

    void OnAllocationFailed() noexcept {
        retry_frames = RETRY_FRAMES;
    }

    [[nodiscard]] std::uint64_t Target() const noexcept {
        return target;
    }

    [[nodiscard]] const MemoryGuardPolicy& Policy() const noexcept {
        return policy;
    }

private:
    MemoryGuardPolicy policy;
    std::uint64_t target;
    unsigned retry_frames{};
    bool retiring{};
    bool emergency_finished{};
};

/// Allocation-time headroom. The frame-boundary snapshot is decremented by every buffer created
/// since, so a live (WinRT) measurement is paid only when the estimate nears RESERVE: never on a
/// healthy frame, and under pressure once per allocation of MEASURE_STEP or more (or per
/// MEASURE_STEP of small ones). Frees are not credited, so the estimate errs on the safe side.
class HeadroomTracker {
public:
    static constexpr std::uint64_t RESERVE = 64 * MemoryGuard::MiB; ///< Never knowingly go below.
    static constexpr std::uint64_t SLACK = 192 * MemoryGuard::MiB;  ///< Re-measure below R + S.
    static constexpr std::uint64_t MEASURE_STEP = MemoryGuard::MiB;

    void Measured(std::uint64_t used, std::uint64_t limit) noexcept {
        known = limit != 0;
        free = used >= limit ? 0 : limit - used;
        created = 0;
    }

    void Created(std::uint64_t bytes) noexcept {
        created += bytes;
    }

    /// Expected free memory after creating bytes more; unbounded when the limit is unknown.
    [[nodiscard]] std::uint64_t Projected(std::uint64_t bytes) const noexcept {
        if (!known) {
            return (std::numeric_limits<std::uint64_t>::max)();
        }
        const std::uint64_t spent = created + bytes;
        return free > spent ? free - spent : 0;
    }

    [[nodiscard]] bool NeedsMeasure(std::uint64_t bytes) const noexcept {
        if (!known) {
            return false;
        }
        const std::uint64_t projected = Projected(bytes);
        if (projected >= RESERVE + SLACK) {
            return false;
        }
        return projected < RESERVE || bytes >= MEASURE_STEP || created >= MEASURE_STEP;
    }

    [[nodiscard]] bool Fits(std::uint64_t bytes) const noexcept {
        return Projected(bytes) >= RESERVE;
    }

private:
    std::uint64_t free{};
    std::uint64_t created{};
    bool known{};
};

/// Optional caches (the linked shader cache): trimmed at LOW_FREE, restored only after
/// RESTORE_FRAMES consecutive frames with RESTORE_FREE, so they never flap with the headroom.
class OptionalCacheGate {
public:
    static constexpr std::uint64_t TRIM_FREE = MemoryGuardPolicy::LOW_FREE;
    static constexpr std::uint64_t RESTORE_FREE = 768 * MemoryGuard::MiB;
    static constexpr unsigned RESTORE_FRAMES = 600; ///< Ten seconds at 60 FPS.

    enum class Action { None, Trim, Restore };

    Action Update(std::uint64_t used, std::uint64_t limit) noexcept {
        if (!limit) {
            healthy_frames = 0;
            return Action::None;
        }
        const std::uint64_t free = used >= limit ? 0 : limit - used;
        if (!trimmed) {
            if (free <= TRIM_FREE) {
                trimmed = true;
                healthy_frames = 0;
                return Action::Trim;
            }
            return Action::None;
        }
        if (free < RESTORE_FREE) {
            healthy_frames = 0;
            return Action::None;
        }
        if (++healthy_frames < RESTORE_FRAMES) {
            return Action::None;
        }
        trimmed = false;
        healthy_frames = 0;
        return Action::Restore;
    }

private:
    unsigned healthy_frames{};
    bool trimmed{};
};

} // namespace D3D12
