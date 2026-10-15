// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <array>

namespace D3D12 {

// Only single-texel probe formats support bounded ROI readback (DXGI values).
constexpr std::uint32_t MaterialProbeRegionBytes(std::uint32_t format) {
    return format == 2 ? 16 : format == 26 ? 4 : 0;
}

// Explicit shader/count selections may need all four 1080p G-buffer inputs.
// This remains subordinate to the capture-wide readback cap and present reserve.
class MaterialSampledInputBudget {
public:
    static constexpr std::uint64_t MAX_RESOURCE_BYTES = 8ULL << 20;
    static constexpr std::uint64_t MAX_BYTES = 32ULL << 20;
    bool Fits(std::uint64_t count) const {
        return count && count <= MAX_RESOURCE_BYTES && count <= MAX_BYTES - bytes;
    }
    bool Commit(std::uint64_t count) {
        if (!Fits(count)) return false;
        bytes += count;
        return true;
    }
private:
    std::uint64_t bytes{};
};

// At most two selected draws per capture; their MRT snapshots share a fixed budget.
// Configuration is
// generic (shader hashes supplied by the diagnostic package), never a game workaround.
class MaterialSnapshotSelection {
public:
    static constexpr std::uint64_t MAX_BYTES = 32ULL << 20;
    void Begin(std::array<std::uint64_t, 2> requested,
               std::array<std::uint32_t, 2> counts = {}) {
        shaders = requested;
        vertices = counts;
        bytes = 0;
    }
    bool Matches(std::uint64_t shader, std::uint32_t count) const {
        if (!shader) return false;
        for (std::size_t i = 0; i < shaders.size(); ++i)
            if (shaders[i] == shader && (!vertices[i] || vertices[i] == count)) return true;
        return false;
    }
    bool Targeted(std::uint64_t shader, std::uint32_t count) const {
        if (!shader) return false;
        for (std::size_t i = 0; i < shaders.size(); ++i)
            if (shaders[i] == shader && vertices[i] && vertices[i] == count) return true;
        return false;
    }
    bool Consume(std::uint64_t shader, std::uint32_t count = 0) {
        bool selected = false;
        for (std::size_t i = 0; i < shaders.size(); ++i) {
            if (shader && shaders[i] == shader && (!vertices[i] || vertices[i] == count)) {
                shaders[i] = 0;
                selected = true;
            }
        }
        return selected;
    }
    bool AllowBytes(std::uint64_t count) {
        if (!count || count > MAX_BYTES - bytes) return false;
        bytes += count;
        return true;
    }
private:
    std::array<std::uint64_t, 2> shaders{};
    std::array<std::uint32_t, 2> vertices{};
    std::uint64_t bytes{};
};

// Native block bytes, including typeless and sRGB variants. Zero means unsupported.
constexpr std::uint32_t MaterialBlockBytes(std::uint32_t format) {
    if ((format >= 70 && format <= 72) || (format >= 79 && format <= 81)) return 8;
    if ((format >= 73 && format <= 78) || (format >= 82 && format <= 84) ||
        (format >= 94 && format <= 99)) return 16;
    return 0;
}

// Metadata only. A request lasts one presented frame; neither budget affects rendering.
class MaterialTraceBudget {
public:
    static constexpr std::size_t MAX_DRAWS = 4096;
    static constexpr std::size_t MAX_BYTES = 2 * 1024 * 1024;

    void Begin(std::uint64_t pixel_shader = 0) {
        *this = {};
        active = true;
        shader = pixel_shader;
    }
    void End() { active = false; }
    bool Active() const { return active; }
    bool Select(std::uint64_t pixel_shader) {
        if (!active || (shader != 0 && shader != pixel_shader)) return false;
        ++matched;
        if (selected == MAX_DRAWS || bytes == MAX_BYTES) return false;
        ++selected;
        return true;
    }
    bool AllowBytes(std::size_t count) {
        if (!active || count > MAX_BYTES - bytes) {
            if (active) {
                bytes = MAX_BYTES;
                ++dropped_lines;
            }
            return false;
        }
        bytes += count;
        return true;
    }
    std::size_t matched{}, selected{}, bytes{}, dropped_lines{};

private:
    bool active{};
    std::uint64_t shader{};
};

} // namespace D3D12
