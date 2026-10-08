// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <span>

// Diagnostic: the call stacks of the first operator new calls of chosen sizes ("alloc_watch=" in
// boot.cfg). The heap report gives the sizes of what fills the heaps but not their owners; a size
// seen there, watched here, names the code that allocates it. The check on every allocation is one
// relaxed load while nothing is watched.
namespace EdenXbox::AllocWatch {

inline constexpr std::size_t MAX_SIZES = 8;
inline constexpr std::size_t MAX_CAPTURES = 16;
inline constexpr std::size_t MAX_FRAMES = 28;

/// Sets the sizes to watch (at most MAX_SIZES); call before the allocations of interest.
void Watch(std::span<const std::size_t> sizes) noexcept;

struct Capture {
    std::size_t size;
    unsigned short frame_count;
    void* frames[MAX_FRAMES];
};

/// Captures recorded since the last call, each returned once.
std::span<const Capture> TakeNew() noexcept;

} // namespace EdenXbox::AllocWatch
