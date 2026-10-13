// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>

namespace D3D12 {

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
