// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <exception>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <fmt/format.h>

#include "common/logging.h"
#include "common/settings.h"
#include "video_core/frame_trace.h"
#include "video_core/gpu.h"
#include "video_core/perf_counters.h"
#include "video_core/renderer_d3d12/d3d12_resource_utils.h"
#include "video_core/renderer_d3d12/renderer_d3d12.h"
#include "video_core/shader_notify.h"

// Overlays drawn over the presented image: performance HUD, trace and shader indicators, load
// progress and the game menu (ClearRenderTargetView rects, no pipeline).

namespace D3D12 {

namespace {

/// Shared 3x5 overlay font, drawn as batched ClearRenderTargetView rects.
/// Digits 0-9 then '/'; rows top to bottom, bit 2 is the left column.
constexpr std::array<std::array<u8, 5>, 11> GLYPHS{{
    {7, 5, 5, 5, 7}, {2, 6, 2, 2, 7}, {7, 1, 7, 4, 7}, {7, 1, 7, 1, 7}, {5, 5, 7, 1, 1},
    {7, 4, 7, 1, 7}, {7, 4, 7, 5, 7}, {7, 1, 1, 1, 1}, {7, 5, 7, 5, 7}, {7, 5, 7, 1, 7},
    {1, 1, 2, 4, 4},
}};

constexpr std::array<std::array<u8, 5>, 26> LETTER_GLYPHS{{
    {2,5,7,5,5}, {6,5,6,5,6}, {3,4,4,4,3}, {6,5,5,5,6}, {7,4,6,4,7},
    {7,4,6,4,4}, {3,4,5,5,3}, {5,5,7,5,5}, {7,2,2,2,7}, {1,1,1,5,2},
    {5,5,6,5,5}, {4,4,4,4,7}, {5,7,7,5,5}, {5,7,7,7,5}, {2,5,5,5,2},
    {6,5,6,4,4}, {2,5,5,3,1}, {6,5,6,5,5}, {3,4,2,1,6}, {7,2,2,2,2},
    {5,5,5,5,7}, {5,5,5,5,2}, {5,5,7,7,5}, {5,5,2,5,5}, {5,5,2,2,2},
    {7,1,2,4,7},
}};

std::array<u8, 5> Glyph(char c) {
    if (c >= '0' && c <= '9') {
        return GLYPHS[c - '0'];
    }
    if (c >= 'A' && c <= 'Z') {
        return LETTER_GLYPHS[c - 'A'];
    }
    switch (c) {
    case '/': return GLYPHS[10];
    case '.': return {0,0,0,0,2};
    case '%': return {5,1,2,4,5};
    case '-': return {0,0,7,0,0};
    case ':': return {0,2,0,2,0};
    case '<': return {1,2,4,2,1};
    case '>': return {4,2,1,2,4};
    default: return {};
    }
}

/// Width of `chars` glyphs of `cell` pixels, with a one-cell gap between them.
LONG TextWidth(size_t chars, LONG cell) {
    return chars == 0 ? 0 : static_cast<LONG>(chars) * 4 * cell - cell;
}

/// Shared bitmap text for shader progress and performance panels. Spaces advance normally.
void AppendText(std::vector<D3D12_RECT>& rects, std::string_view text, LONG x, LONG y, LONG cell) {
    for (const char c : text) {
        const auto glyph = Glyph(c);
        for (LONG row = 0; row < 5; ++row) {
            LONG column = 0;
            while (column < 3) {
                if (((glyph[row] >> (2 - column)) & 1) == 0) {
                    ++column;
                    continue;
                }
                const LONG start = column++;
                while (column < 3 && ((glyph[row] >> (2 - column)) & 1)) {
                    ++column;
                }
                rects.push_back({x + start * cell, y + row * cell, x + column * cell,
                                 y + (row + 1) * cell});
            }
        }
        x += 4 * cell;
    }
}

void ClearRects(ID3D12GraphicsCommandList* cmd, D3D12_CPU_DESCRIPTOR_HANDLE rtv,
                const std::array<float, 4>& color, std::span<const D3D12_RECT> rects) {
    if (!rects.empty()) {
        cmd->ClearRenderTargetView(rtv, color.data(), static_cast<UINT>(rects.size()),
                                   rects.data());
    }
}

// Overlay colors: a near-black panel, dim and amber activity dots, light gray text.
constexpr std::array<float, 4> OVERLAY_PANEL{0.04f, 0.04f, 0.05f, 1.0f};
constexpr std::array<float, 4> OVERLAY_DIM{0.25f, 0.25f, 0.28f, 1.0f};
constexpr std::array<float, 4> OVERLAY_ACCENT{1.0f, 0.72f, 0.2f, 1.0f};
constexpr std::array<float, 4> OVERLAY_TEXT{0.82f, 0.82f, 0.84f, 1.0f};

} // Anonymous namespace

void ShowLoadProgress(VideoCore::RendererBase& renderer, size_t done, size_t total) {
    // No RTTI in this build: the configured backend says which renderer this is.
    if (Settings::values.renderer_backend.GetValue() == Settings::RendererBackend::Direct3D12) {
        static_cast<RendererD3D12&>(renderer).ShowLoadProgress(done, total);
    }
}

void RendererD3D12::DrawPerformanceOverlay(ID3D12GraphicsCommandList* cmd,
                                          D3D12_CPU_DESCRIPTOR_HANDLE rtv) {
    auto& hud = performance_overlay;
    const auto now = std::chrono::steady_clock::now();
    const bool first = hud.sample_time == std::chrono::steady_clock::time_point{};
    const double elapsed_ms = std::chrono::duration<double, std::milli>(now - hud.sample_time).count();
    if (first || elapsed_ms >= 500.0) {
        FILETIME created{}, exited{}, kernel{}, user{};
        const bool cpu_valid = GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
        const auto ticks = [](FILETIME time) {
            return (static_cast<u64>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
        };
        const u64 cpu_ticks = ticks(kernel) + ticks(user);
        const u64 gpu_us = VideoCore::Perf::counters[
            static_cast<size_t>(VideoCore::Perf::Counter::GpuBusyUs)].load(std::memory_order_relaxed);
        std::array<std::string, 4> lines{"FPS --", "FRAME -- MS MAX --", "CPU --", "GPUQ --"};
        if (!first && hud.frames != 0) {
            const double frames = static_cast<double>(hud.frames);
            lines[0] = fmt::format("FPS {:.1f}", frames * 1000.0 / elapsed_ms);
            lines[1] = fmt::format("FRAME {:.1f} MS MAX {:.1f}", hud.last_frame_ms, hud.max_frame_ms);
            // Process CPU includes all threads; show equivalent cores rather than an ambiguous %.
            if (cpu_valid && hud.cpu_valid && cpu_ticks >= hud.cpu_ticks) {
                const double cpu_ms = static_cast<double>(cpu_ticks - hud.cpu_ticks) / 10000.0;
                lines[2] = fmt::format("CPU {:.2f} CORES {:.1f} MS/F", cpu_ms / elapsed_ms,
                                       cpu_ms / frames);
            }
            if (scheduler.HasGpuTimestamps()) {
                const double gpu_ms = static_cast<double>(gpu_us - hud.gpu_us) / 1000.0;
                // Completed direct-queue time, delayed by frames in flight; not whole-GPU utilization.
                lines[3] = fmt::format("GPUQ {:.0f}% {:.1f} MS/F", gpu_ms * 100.0 / elapsed_ms,
                                       gpu_ms / frames);
            }
        }
        const LONG cell = std::max<LONG>(2, static_cast<LONG>(swapchain.Height()) / 360);
        const LONG pad = 3 * cell;
        const LONG right = static_cast<LONG>(swapchain.Width()) - 8 * cell;
        const LONG top = 8 * cell;
        size_t chars = 0;
        for (const auto& line : lines) {
            chars = std::max(chars, line.size());
        }
        const LONG left = right - TextWidth(chars, cell) - 2 * pad;
        hud.panel = {left, top, right, top + 2 * pad + 4 * 7 * cell - 2 * cell};
        hud.text.clear();
        hud.text.reserve(1536);
        for (size_t row = 0; row < lines.size(); ++row) {
            AppendText(hud.text, lines[row], left + pad,
                       top + pad + static_cast<LONG>(row) * 7 * cell, cell);
        }
        hud.sample_time = now;
        hud.cpu_ticks = cpu_ticks;
        hud.cpu_valid = cpu_valid;
        hud.gpu_us = gpu_us;
        hud.frames = 0;
        hud.max_frame_ms = 0;
    }
    ClearRects(cmd, rtv, OVERLAY_PANEL, {&hud.panel, 1});
    ClearRects(cmd, rtv, OVERLAY_TEXT, hud.text);
    DrawTraceIndicator(cmd, rtv);
}

void RendererD3D12::DrawTraceIndicator(ID3D12GraphicsCommandList* cmd,
                                      D3D12_CPU_DESCRIPTOR_HANDLE rtv) {
    using VideoCore::FrameTrace::CaptureState;
    const auto status = VideoCore::FrameTrace::GetCaptureStatus();
    if (status.state == CaptureState::Idle) {
        return;
    }
    auto& hud = performance_overlay;
    const u32 tenths = (status.vsyncs_remaining + 5) / 6;
    const u64 key = (static_cast<u64>(status.id) << 32) |
                    (static_cast<u64>(status.state) << 24) | tenths;
    const LONG cell = std::max<LONG>(2, static_cast<LONG>(swapchain.Height()) / 360);
    const LONG top = hud.panel.bottom + 2 * cell;
    if (key != hud.trace_key || top != hud.trace_top) {
        std::string label;
        switch (status.state) {
        case CaptureState::Recording:
            label = fmt::format("T {} CAPTURANDO {:.1f} S", status.id, tenths / 10.0);
            break;
        case CaptureState::Saving: label = fmt::format("T {} GUARDANDO", status.id); break;
        case CaptureState::Saved: label = fmt::format("T {} GUARDADA", status.id); break;
        case CaptureState::Truncated: label = fmt::format("T {} TRUNCADA", status.id); break;
        default: return;
        }
        const LONG pad = 3 * cell;
        const LONG right = hud.panel.right;
        const LONG left = right - TextWidth(label.size(), cell) - 2 * pad;
        hud.trace_panel = {left, top, right, top + 2 * pad + 5 * cell};
        hud.trace_text.clear();
        hud.trace_text.reserve(256);
        AppendText(hud.trace_text, label, left + pad, top + pad, cell);
        hud.trace_key = key;
        hud.trace_top = top;
    }
    constexpr std::array<float, 4> saved_color{0.3f, 1.0f, 0.45f, 1.0f};
    ClearRects(cmd, rtv, OVERLAY_PANEL, {&hud.trace_panel, 1});
    ClearRects(cmd, rtv, status.state == CaptureState::Saved ? saved_color : OVERLAY_ACCENT,
               hud.trace_text);
}

void RendererD3D12::DrawShaderIndicator(ID3D12GraphicsCommandList* cmd,
                                        D3D12_CPU_DESCRIPTOR_HANDLE rtv) {
    const int building = gpu.ShaderNotify().ShadersBuilding();
    if (building <= 0) {
        indicator_frames = 0;
        return;
    }
    ++indicator_frames;
    // 3-pixel cells at 1080p: the panel is 27 pixels tall, out of the way in the corner.
    const LONG cell = std::max<LONG>(2, static_cast<LONG>(swapchain.Height()) / 360);
    const std::string count = std::to_string(building);
    const LONG pad = 3 * cell;
    const LONG dots_width = 5 * cell;
    const LONG width = pad + dots_width + 3 * cell + TextWidth(count.size(), cell) + pad;
    const LONG height = pad + 5 * cell + pad;
    const LONG right = static_cast<LONG>(swapchain.Width()) - 8 * cell;
    const LONG bottom = static_cast<LONG>(swapchain.Height()) - 8 * cell;
    const LONG left = right - width;
    const LONG top = bottom - height;
    const D3D12_RECT panel{left, top, right, bottom};
    ClearRects(cmd, rtv, OVERLAY_PANEL, {&panel, 1});

    // Three dots on the middle row; the lit one moves every 8 presented frames.
    std::vector<D3D12_RECT> dim;
    std::vector<D3D12_RECT> lit;
    const LONG dot_y = top + pad + 2 * cell;
    for (LONG dot = 0; dot < 3; ++dot) {
        const LONG x = left + pad + dot * 2 * cell;
        (dot == static_cast<LONG>((indicator_frames / 8) % 3) ? lit : dim)
            .push_back({x, dot_y, x + cell, dot_y + cell});
    }
    ClearRects(cmd, rtv, OVERLAY_DIM, dim);
    ClearRects(cmd, rtv, OVERLAY_ACCENT, lit);

    std::vector<D3D12_RECT> text;
    AppendText(text, count, left + pad + dots_width + 3 * cell, top + pad, cell);
    ClearRects(cmd, rtv, OVERLAY_TEXT, text);
}

void ShowGameMenu(VideoCore::RendererBase& renderer, std::string_view title,
                  std::span<const std::string> items, size_t selected, std::string_view hint) {
    if (Settings::values.renderer_backend.GetValue() == Settings::RendererBackend::Direct3D12) {
        static_cast<RendererD3D12&>(renderer).ShowGameMenu(RendererD3D12::GameMenuOverlay{
            std::string{title}, {items.begin(), items.end()}, selected, std::string{hint}});
    }
}

void HideGameMenu(VideoCore::RendererBase& renderer) {
    if (Settings::values.renderer_backend.GetValue() == Settings::RendererBackend::Direct3D12) {
        static_cast<RendererD3D12&>(renderer).ShowGameMenu(std::nullopt);
    }
}

void ShowCpuLoadProgress(VideoCore::RendererBase& renderer, size_t done, size_t total) {
    if (Settings::values.renderer_backend.GetValue() == Settings::RendererBackend::Direct3D12) {
        static_cast<RendererD3D12&>(renderer).ShowLoadProgress(done, total, "CPU JIT");
    }
}

void RendererD3D12::ShowLoadProgress(size_t done, size_t total, std::string_view phase) {
    if (present_failed || total == 0) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (done != total && now - last_load_present < std::chrono::milliseconds{33}) {
        return;
    }
    last_load_present = now;
    try {
        FrameLease frame = present_manager.AcquireFrame();
        ID3D12GraphicsCommandList* const cmd = scheduler.CommandList();
        frame->Transition(cmd, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv = frame->rtv;
        constexpr float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        cmd->ClearRenderTargetView(rtv, black, 0, nullptr);

        // A bar across the middle 40% of the screen and "done/total" centered under it.
        const LONG screen_width = static_cast<LONG>(swapchain.Width());
        const LONG screen_height = static_cast<LONG>(swapchain.Height());
        const LONG cell = std::max<LONG>(2, screen_height / 270);
        const LONG bar_width = screen_width * 2 / 5;
        const LONG bar_left = (screen_width - bar_width) / 2;
        const LONG bar_top = screen_height / 2 - cell;
        const LONG filled = static_cast<LONG>(static_cast<double>(bar_width) *
                                              static_cast<double>(std::min(done, total)) /
                                              static_cast<double>(total));
        const D3D12_RECT bar{bar_left, bar_top, bar_left + bar_width, bar_top + 2 * cell};
        const D3D12_RECT fill{bar_left, bar_top, bar_left + filled, bar_top + 2 * cell};
        ClearRects(cmd, rtv, OVERLAY_DIM, {&bar, 1});
        if (filled > 0) {
            ClearRects(cmd, rtv, OVERLAY_ACCENT, {&fill, 1});
        }
        const std::string label = fmt::format("{} {}/{}", phase, done, total);
        std::vector<D3D12_RECT> text;
        AppendText(text, label, (screen_width - TextWidth(label.size(), cell)) / 2,
                   bar_top + 6 * cell, cell);
        ClearRects(cmd, rtv, OVERLAY_TEXT, text);
        Present(std::move(frame));
    } catch (const std::exception& e) {
        LOG_ERROR(Render, "D3D12: shader cache progress not shown: {}", e.what());
    }
}

void RendererD3D12::ShowGameMenu(std::optional<GameMenuOverlay> menu) {
    game_menu = std::move(menu);
    if (present_failed) {
        return;
    }
    try {
        FrameLease frame = present_manager.AcquireFrame();
        // The guest is paused: blit its last frame again (the menu is drawn with it), or show the
        // menu on black when that frame is not a GPU image.
        if (!blit_ready || !last_framebuffer || !CompositeAccelerated(*last_framebuffer, *frame)) {
            ID3D12GraphicsCommandList* const cmd = scheduler.CommandList();
            frame->Transition(cmd, D3D12_RESOURCE_STATE_RENDER_TARGET);
            constexpr float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            cmd->ClearRenderTargetView(frame->rtv, black, 0, nullptr);
            DrawGameMenu(cmd, frame->rtv);
        }
        Present(std::move(frame));
    } catch (const std::exception& e) {
        LOG_ERROR(Render, "D3D12: game menu not shown: {}", e.what());
    }
}

void RendererD3D12::DrawGameMenu(ID3D12GraphicsCommandList* cmd,
                                 D3D12_CPU_DESCRIPTOR_HANDLE rtv) {
    if (!game_menu) {
        return;
    }
    const LONG screen_width = static_cast<LONG>(swapchain.Width());
    const LONG screen_height = static_cast<LONG>(swapchain.Height());
    size_t longest = std::max(game_menu->title.size(), game_menu->hint.size());
    for (const std::string& item : game_menu->items) {
        longest = std::max(longest, item.size());
    }
    // Glyphs 5 cells high on rows of 10; shrink the cell until the longest line fits.
    LONG cell = std::max<LONG>(2, screen_height / 200);
    while (cell > 2 && TextWidth(longest, cell) + 16 * cell > screen_width * 9 / 10) {
        --cell;
    }
    const LONG row = 10 * cell;
    const LONG pad = 4 * cell;
    const LONG width = TextWidth(longest, cell) + 2 * pad;
    const LONG height = static_cast<LONG>(game_menu->items.size() + 4) * row + 2 * pad;
    const LONG left = (screen_width - width) / 2;
    const LONG top = (screen_height - height) / 2;
    const D3D12_RECT border{left - cell, top - cell, left + width + cell, top + height + cell};
    const D3D12_RECT panel{left, top, left + width, top + height};
    ClearRects(cmd, rtv, OVERLAY_ACCENT, {&border, 1});
    ClearRects(cmd, rtv, OVERLAY_PANEL, {&panel, 1});

    std::vector<D3D12_RECT> title;
    AppendText(title, game_menu->title, left + pad, top + pad + 2 * cell, cell);
    ClearRects(cmd, rtv, OVERLAY_ACCENT, title);
    std::vector<D3D12_RECT> text;
    std::vector<D3D12_RECT> lit;
    for (size_t i = 0; i < game_menu->items.size(); ++i) {
        const LONG y = top + pad + static_cast<LONG>(i + 2) * row;
        if (i == game_menu->selected) {
            const D3D12_RECT bar{left + cell, y - 2 * cell, left + width - cell, y + 7 * cell};
            ClearRects(cmd, rtv, OVERLAY_DIM, {&bar, 1});
        }
        AppendText(i == game_menu->selected ? lit : text, game_menu->items[i], left + pad, y,
                   cell);
    }
    std::vector<D3D12_RECT> hint;
    AppendText(hint, game_menu->hint, left + pad,
               top + pad + static_cast<LONG>(game_menu->items.size() + 3) * row, cell);
    ClearRects(cmd, rtv, OVERLAY_TEXT, text);
    ClearRects(cmd, rtv, OVERLAY_ACCENT, lit);
    ClearRects(cmd, rtv, OVERLAY_TEXT, hint);
}

} // namespace D3D12
