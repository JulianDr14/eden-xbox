// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "video_core/renderer_d3d12/d3d12_memory_guard.h"

// Pure policy for which graphics PSOs stay resident: no device, file or clock, so every decision is
// covered by tools/xbox/tests/pipeline-residency.cpp.
//
// A compiled PSO is the driver's: about 100 KiB each, against some 12 KiB for the pipeline's own
// bookkeeping and a share of its DXIL. A game that built every pipeline of its disk cache at boot
// (17,000 for one of them) spent 1.3 GiB on PSOs before its first frame. So a pipeline keeps its
// DXIL and only has a PSO while it is drawn: the ones used in recent sessions are built at boot,
// the rest when first drawn, and a PSO idle for long enough is released and built again from the
// kept DXIL if it is drawn again.
namespace D3D12 {

/// How long an idle PSO is kept, in frames, for the app's free memory: minutes while there is
/// room, seconds as the memory guard's thresholds near.
struct PipelineResidencyPolicy {
    static constexpr std::uint64_t IDLE_FRAMES = 3 * 60 * 60;      ///< Three minutes at 60 FPS.
    static constexpr std::uint64_t LOW_IDLE_FRAMES = 10 * 60;      ///< Ten seconds.
    static constexpr std::uint64_t EMERGENCY_IDLE_FRAMES = 60;     ///< One second.
    static constexpr std::size_t SWEEP_PER_FRAME = 256;            ///< Pipelines checked a frame.

    /// limit 0 is an unknown limit: as with the memory guard, it never implies pressure.
    [[nodiscard]] static constexpr std::uint64_t IdleFrames(std::uint64_t used,
                                                            std::uint64_t limit) noexcept {
        if (limit == 0) {
            return IDLE_FRAMES;
        }
        const std::uint64_t free = used >= limit ? 0 : limit - used;
        if (free <= MemoryGuardPolicy::EMERGENCY_FREE) {
            return EMERGENCY_IDLE_FRAMES;
        }
        if (free <= MemoryGuardPolicy::LOW_FREE) {
            return LOW_IDLE_FRAMES;
        }
        return IDLE_FRAMES;
    }
};

/// The pipelines drawn in a game's recent sessions, by key hash, persisted next to its disk
/// cache. Each session marks what it draws; a pipeline not drawn for KEEP_SESSIONS sessions is
/// forgotten, so the set follows where the player is instead of growing with the whole game.
class HotPipelineSet {
public:
    static constexpr std::uint32_t KEEP_SESSIONS = 4;
    /// Pipelines built at boot at most, most recently drawn first.
    static constexpr std::size_t MAX_PREWARM = 4096;
    /// With no history yet (first session, or a file of an older format), the first pipelines
    /// of the disk cache, which is in the order they were first seen: the start of the game.
    static constexpr std::size_t FIRST_SESSION_PREWARM = 2048;

    /// Starts a session from a saved file; an empty, damaged or foreign one starts with no history.
    void Load(std::span<const std::uint8_t> bytes) {
        last_used.clear();
        session = 1;
        if (bytes.size() < HEADER_SIZE || std::memcmp(bytes.data(), MAGIC, sizeof(MAGIC)) != 0 ||
            Read32(bytes, 4) != VERSION) {
            return;
        }
        const std::uint32_t saved_session = Read32(bytes, 8);
        const std::uint32_t count = Read32(bytes, 12);
        if (bytes.size() != HEADER_SIZE + std::size_t{count} * ENTRY_SIZE) {
            return;
        }
        session = saved_session + 1;
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::size_t offset = HEADER_SIZE + std::size_t{i} * ENTRY_SIZE;
            const std::uint32_t entry_session = Read32(bytes, offset + 8);
            if (entry_session < session && session - entry_session <= KEEP_SESSIONS) {
                last_used.emplace(Read64(bytes, offset), entry_session);
            }
        }
        BuildPrewarm();
    }

    [[nodiscard]] bool HasHistory() const noexcept {
        return !last_used.empty();
    }

    /// Whether the pipeline should get its PSO at boot. file_index is its position in the disk
    /// cache, for the first session's fallback.
    [[nodiscard]] bool Prewarm(std::uint64_t hash, std::size_t file_index) const {
        if (!HasHistory()) {
            return file_index < FIRST_SESSION_PREWARM;
        }
        return prewarm.contains(hash);
    }

    /// Marks a pipeline drawn this session; true the first time, when the set needs saving.
    bool MarkUsed(std::uint64_t hash) {
        auto [it, inserted] = last_used.try_emplace(hash, session);
        if (!inserted && it->second == session) {
            return false;
        }
        it->second = session;
        return true;
    }

    [[nodiscard]] std::size_t Size() const noexcept {
        return last_used.size();
    }

    [[nodiscard]] std::size_t PrewarmSize() const noexcept {
        return prewarm.size();
    }

    [[nodiscard]] std::vector<std::uint8_t> Serialize() const {
        std::vector<std::uint8_t> bytes(HEADER_SIZE + last_used.size() * ENTRY_SIZE);
        std::memcpy(bytes.data(), MAGIC, sizeof(MAGIC));
        Write32(bytes, 4, VERSION);
        Write32(bytes, 8, session);
        Write32(bytes, 12, static_cast<std::uint32_t>(last_used.size()));
        std::size_t offset = HEADER_SIZE;
        for (const auto& [hash, entry_session] : last_used) {
            Write64(bytes, offset, hash);
            Write32(bytes, offset + 8, entry_session);
            offset += ENTRY_SIZE;
        }
        return bytes;
    }

private:
    static constexpr char MAGIC[4] = {'D', '3', 'H', 'P'};
    static constexpr std::uint32_t VERSION = 1;
    static constexpr std::size_t HEADER_SIZE = 16;
    static constexpr std::size_t ENTRY_SIZE = 12;

    void BuildPrewarm() {
        std::vector<std::pair<std::uint32_t, std::uint64_t>> by_recency;
        by_recency.reserve(last_used.size());
        for (const auto& [hash, entry_session] : last_used) {
            by_recency.emplace_back(entry_session, hash);
        }
        // Most recent session first; ties by hash, so the choice does not depend on map order.
        std::sort(by_recency.begin(), by_recency.end(),
                  [](const auto& a, const auto& b) { return a.first != b.first ? a.first > b.first
                                                                                : a.second < b.second; });
        by_recency.resize((std::min)(by_recency.size(), MAX_PREWARM));
        prewarm.clear();
        for (const auto& entry : by_recency) {
            prewarm.insert(entry.second);
        }
    }

    static std::uint32_t Read32(std::span<const std::uint8_t> bytes, std::size_t offset) {
        std::uint32_t value;
        std::memcpy(&value, bytes.data() + offset, sizeof(value));
        return value;
    }
    static std::uint64_t Read64(std::span<const std::uint8_t> bytes, std::size_t offset) {
        std::uint64_t value;
        std::memcpy(&value, bytes.data() + offset, sizeof(value));
        return value;
    }
    static void Write32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
        std::memcpy(bytes.data() + offset, &value, sizeof(value));
    }
    static void Write64(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint64_t value) {
        std::memcpy(bytes.data() + offset, &value, sizeof(value));
    }

    std::uint32_t session{1};
    std::unordered_map<std::uint64_t, std::uint32_t> last_used; ///< hash -> last session drawn
    std::unordered_set<std::uint64_t> prewarm;                  ///< chosen at Load
};

} // namespace D3D12
