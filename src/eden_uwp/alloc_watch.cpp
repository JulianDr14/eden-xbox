// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <atomic>
#include <cstdlib>
#include <new>

#include <windows.h>

#include "eden_uwp/alloc_watch.h"

namespace EdenXbox::AllocWatch {
namespace {

std::array<std::atomic<std::size_t>, MAX_SIZES> g_sizes{};
std::atomic<std::size_t> g_count{};
/// Captures per watched size, so a frequent size cannot use up the slots of a rare one.
std::array<std::atomic<unsigned>, MAX_SIZES> g_per_size{};
constexpr unsigned CAPTURES_PER_SIZE = 2;

std::array<Capture, MAX_CAPTURES> g_captures{};
std::atomic<std::size_t> g_reserved{}; ///< Slots handed out.
std::array<std::atomic<bool>, MAX_CAPTURES> g_ready{};
std::size_t g_taken{}; ///< Slots already returned by TakeNew (one reader).

// Lock-free and allocation-free: it runs inside operator new.
void Check(std::size_t size) noexcept {
    const std::size_t count = g_count.load(std::memory_order_relaxed);
    for (std::size_t i = 0; i < count; ++i) {
        if (g_sizes[i].load(std::memory_order_relaxed) != size) {
            continue;
        }
        if (g_per_size[i].fetch_add(1, std::memory_order_relaxed) >= CAPTURES_PER_SIZE) {
            return;
        }
        const std::size_t slot = g_reserved.fetch_add(1, std::memory_order_relaxed);
        if (slot >= MAX_CAPTURES) {
            return;
        }
        Capture& capture = g_captures[slot];
        capture.size = size;
        capture.frame_count = RtlCaptureStackBackTrace(1, static_cast<DWORD>(MAX_FRAMES),
                                                       capture.frames, nullptr);
        g_ready[slot].store(true, std::memory_order_release);
        return;
    }
}

} // Anonymous namespace

void Watch(std::span<const std::size_t> sizes) noexcept {
    const std::size_t count = sizes.size() < MAX_SIZES ? sizes.size() : MAX_SIZES;
    for (std::size_t i = 0; i < count; ++i) {
        g_sizes[i].store(sizes[i], std::memory_order_relaxed);
    }
    g_count.store(count, std::memory_order_release);
}

std::span<const Capture> TakeNew() noexcept {
    const std::size_t begin = g_taken;
    std::size_t end = begin;
    while (end < MAX_CAPTURES && g_ready[end].load(std::memory_order_acquire)) {
        ++end;
    }
    g_taken = end;
    return {g_captures.data() + begin, end - begin};
}

} // namespace EdenXbox::AllocWatch

// The replaceable global allocation functions, so every operator new in the executable passes the
// watch. Same behaviour as the MSVC defaults (malloc/free).
void* operator new(std::size_t size) {
    EdenXbox::AllocWatch::Check(size);
    if (void* const pointer = std::malloc(size != 0 ? size : 1)) {
        return pointer;
    }
    throw std::bad_alloc{};
}

void* operator new[](std::size_t size) {
    return operator new(size);
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    EdenXbox::AllocWatch::Check(size);
    return std::malloc(size != 0 ? size : 1);
}

void* operator new[](std::size_t size, const std::nothrow_t& tag) noexcept {
    return operator new(size, tag);
}

void operator delete(void* pointer) noexcept {
    std::free(pointer);
}

void operator delete[](void* pointer) noexcept {
    std::free(pointer);
}

void operator delete(void* pointer, std::size_t) noexcept {
    std::free(pointer);
}

void operator delete[](void* pointer, std::size_t) noexcept {
    std::free(pointer);
}

void operator delete(void* pointer, const std::nothrow_t&) noexcept {
    std::free(pointer);
}

void operator delete[](void* pointer, const std::nothrow_t&) noexcept {
    std::free(pointer);
}
