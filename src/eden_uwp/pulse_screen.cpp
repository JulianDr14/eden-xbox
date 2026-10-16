// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#include "eden_uwp/pulse_screen.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <numbers>
#include <thread>

#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <winrt/base.h>

#include "common/logging.h"
#include "core/core.h"
#include "core/perf_stats.h"
#include "eden_uwp/localization.h"
#include "eden_uwp/ui_painter.h"
#include "video_core/gpu.h"
#include "video_core/renderer_d3d12/d3d12_menu_image.h"

namespace EdenXbox {
namespace {
using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;
using T = I18n::Text; // Text is also UiPainter::Text

/// The screen's layout units, centered on a 1280x720 screen.
struct Layout {
    static constexpr float Width = 640, Height = 460;
    static constexpr float RingX = 320, RingY = 180, Radius = 84, Stroke = 8;
    static constexpr float TitleY = 300, GameY = 340, StepY = 370, BarY = 410, HintY = 426;
    static constexpr float BarWidth = 320, BarHeight = 4;
};

constexpr auto FramePeriod = std::chrono::milliseconds{33};
/// How long the closing frame (the check) stays before drawing stops.
constexpr float CloseSeconds = 0.8f;
/// A resume's screen closes once the game has presented this many frames, or after the timeout.
constexpr unsigned long long GameFramesToClose = 3;
constexpr float StartingTimeoutSeconds = 20;
constexpr unsigned Glint = 0xd8fff2;
constexpr unsigned Comet = 0xa6f5da;

enum class Stage { Preparing, Capture, Write, Verify, Sync, Replace, Restore, Shaders, Starting };
enum class Outcome { None, Done, Failed };

/// The share of the bar a stage fills: it eases from `from` towards `to` (time constant `tau`
/// seconds), so the ring always moves without ever claiming a step that has not finished.
struct Range {
    float from, to, tau;
};
constexpr Range RangeOf(Stage stage) {
    switch (stage) {
    case Stage::Preparing: return {0.00f, 0.05f, 0.3f};
    case Stage::Capture: return {0.05f, 0.18f, 0.35f};
    case Stage::Write: return {0.18f, 0.58f, 1.0f};
    case Stage::Verify: return {0.58f, 0.84f, 0.7f};
    case Stage::Sync: return {0.84f, 0.97f, 0.4f};
    case Stage::Replace: return {0.97f, 0.99f, 0.2f};
    case Stage::Restore: return {0.03f, 0.45f, 2.2f};
    case Stage::Shaders: return {0.45f, 0.88f, 3.0f};
    case Stage::Starting: return {0.88f, 0.99f, 1.5f};
    }
    return {0, 1, 1};
}

float Seconds(Clock::duration duration) {
    return std::chrono::duration<float>(duration).count();
}

/// Two beats a cycle, as a heart: 0 at rest, about 1 on the first beat.
float Heartbeat(float t) {
    const float phase = std::fmod(t, 1.2f);
    const auto beat = [](float x) { return std::exp(-(x * x) / (0.07f * 0.07f)); };
    return beat(phase) + 0.6f * beat(phase - 0.24f) + beat(phase - 1.2f);
}

/// Overshoots a little before settling at 1, for the check's pop.
float EaseOutBack(float x) {
    x = std::clamp(x, 0.0f, 1.0f) - 1;
    constexpr float c = 1.7f;
    return 1 + (c + 1) * x * x * x + c * x * x;
}

/// What a frame shows.
struct View {
    Stage stage;
    Outcome outcome;
    float t;        ///< Seconds since the screen opened.
    float closing;  ///< Seconds since the outcome, when there is one.
    float progress; ///< 0 to 1.
    std::size_t shaders_done, shaders_total;
};

struct Painter : UiPainter {
    Painter() {
        winrt::check_hresult(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                              IID_PPV_ARGS(&wic)));
        winrt::check_hresult(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, IID_PPV_ARGS(&d2d)));
        winrt::check_hresult(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                                 reinterpret_cast<IUnknown**>(text_factory.GetAddressOf())));
        winrt::check_hresult(d2d->CreateStrokeStyle(
            D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                        D2D1_LINE_JOIN_ROUND),
            nullptr, 0, &round_caps));
    }

    D3D12::MenuImage Draw(const View& view, PulseScreen::Kind kind, std::wstring_view game,
                          std::pair<u32, u32> screen) {
        using L = Layout;
        const auto [screen_width, screen_height] = screen;
        if (screen_width == 0 || screen_height == 0) return {};
        const float scale = std::min({static_cast<float>(screen_height) / 720.0f, 1.5f,
                                      static_cast<float>(screen_width) * 0.95f / L::Width,
                                      static_cast<float>(screen_height) * 0.95f / L::Height});
        const auto wide = static_cast<UINT>(std::ceil(L::Width * scale));
        const auto high = static_cast<UINT>(std::ceil(L::Height * scale));
        if (!bitmap || wide != bitmap_width || high != bitmap_height) {
            target.Reset();
            bitmap.Reset();
            winrt::check_hresult(wic->CreateBitmap(wide, high, GUID_WICPixelFormat32bppPBGRA,
                                                   WICBitmapCacheOnDemand, &bitmap));
            const auto properties = D2D1::RenderTargetProperties(
                D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            winrt::check_hresult(d2d->CreateWicBitmapRenderTarget(bitmap.Get(), properties, &target));
            target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
            Attach(target.Get(), text_factory.Get());
            bitmap_width = wide;
            bitmap_height = high;
        }
        SyncLanguage();
        target->BeginDraw();
        // The renderer fills the rest of the screen with the same color: the image has no edge.
        target->Clear(D2D1::ColorF(Palette::Backdrop));
        target->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale));
        DrawRing(view);
        DrawTexts(view, kind, game);
        // Fades in from the backdrop.
        if (const float fade = std::clamp(view.t / 0.3f, 0.0f, 1.0f); fade < 1) {
            brush->SetColor(D2D1::ColorF(Palette::Backdrop, 1 - fade));
            surface->FillRectangle(D2D1::RectF(0, 0, L::Width, L::Height), brush.Get());
        }
        winrt::check_hresult(target->EndDraw());
        return Pixels();
    }

private:
    static D2D1_POINT_2F At(float cx, float cy, float radius, float degrees) {
        const float radians = degrees * std::numbers::pi_v<float> / 180;
        return {cx + radius * std::sin(radians), cy - radius * std::cos(radians)};
    }

    void Color(unsigned rgb, float alpha) {
        brush->SetColor(D2D1::ColorF(rgb, std::clamp(alpha, 0.0f, 1.0f)));
    }

    void Disc(float cx, float cy, float radius, unsigned rgb, float alpha) {
        Color(rgb, alpha);
        surface->FillEllipse(D2D1::Ellipse({cx, cy}, radius, radius), brush.Get());
    }

    void Circle(float cx, float cy, float radius, float width, unsigned rgb, float alpha) {
        Color(rgb, alpha);
        surface->DrawEllipse(D2D1::Ellipse({cx, cy}, radius, radius), brush.Get(), width);
    }

    /// A clockwise arc from `from` degrees (0 is up), with round caps.
    void Arc(float cx, float cy, float radius, float from, float sweep, float width, unsigned rgb,
             float alpha) {
        if (sweep <= 0.05f) return;
        if (sweep >= 359.9f) return Circle(cx, cy, radius, width, rgb, alpha);
        ComPtr<ID2D1PathGeometry> path;
        ComPtr<ID2D1GeometrySink> sink;
        winrt::check_hresult(d2d->CreatePathGeometry(&path));
        winrt::check_hresult(path->Open(&sink));
        sink->BeginFigure(At(cx, cy, radius, from), D2D1_FIGURE_BEGIN_HOLLOW);
        sink->AddArc(D2D1::ArcSegment(At(cx, cy, radius, from + sweep), D2D1::SizeF(radius, radius), 0,
                                      D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                      sweep > 180 ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
        sink->EndFigure(D2D1_FIGURE_END_OPEN);
        winrt::check_hresult(sink->Close());
        Color(rgb, alpha);
        surface->DrawGeometry(path.Get(), brush.Get(), width, round_caps.Get());
    }

    void Line(D2D1_POINT_2F a, D2D1_POINT_2F b, float width, unsigned rgb, float alpha) {
        Color(rgb, alpha);
        surface->DrawLine(a, b, brush.Get(), width, round_caps.Get());
    }

    void DrawRing(const View& view) {
        using L = Layout;
        constexpr float cx = L::RingX, cy = L::RingY, r = L::Radius;
        const bool failed = view.outcome == Outcome::Failed;
        const unsigned accent = failed ? Palette::Warning : Palette::Accent;
        const float beat = view.outcome == Outcome::None ? Heartbeat(view.t) : 0;

        // Ambient light and the heartbeat: rings of glow that swell on each beat.
        Disc(cx, cy, r + 70, accent, 0.022f + 0.02f * beat);
        Disc(cx, cy, r + 38, accent, 0.03f + 0.03f * beat);
        constexpr int GlowRings = 10;
        for (int k = 0; k < GlowRings; ++k) {
            const float fall = 1 - static_cast<float>(k) / GlowRings;
            Circle(cx, cy, r + 5 + k * 3.0f * (1 + 0.45f * beat), 3, accent,
                   (0.07f + 0.13f * beat) * fall * fall);
        }
        Disc(cx, cy, r - 6, Palette::Row, 0.85f);

        // The track and the progress over it, with a glint at its head.
        Circle(cx, cy, r, L::Stroke, Palette::Track, 1);
        const float sweep = 360 * std::clamp(view.progress, 0.0f, 1.0f);
        Arc(cx, cy, r, 0, sweep, L::Stroke + 6, accent, 0.18f);
        Arc(cx, cy, r, 0, sweep, L::Stroke, accent, 1);
        if (view.outcome == Outcome::None && sweep > 1) {
            const auto head = At(cx, cy, r, sweep);
            Disc(head.x, head.y, 9 + 2 * beat, accent, 0.25f);
            Disc(head.x, head.y, 4.5f, Glint, 1);
        }

        if (view.outcome == Outcome::None) {
            // A comet orbiting outside the ring, its tail fading, and three slow arcs inside it
            // turning the other way.
            constexpr int Tail = 18;
            constexpr float Step = 7;
            const float head = std::fmod(view.t * 360 / 1.3f, 360.0f);
            for (int i = 0; i < Tail; ++i) {
                const float fade = 1 - static_cast<float>(i) / Tail;
                Arc(cx, cy, r + 18, head - (i + 1) * Step, Step + 0.6f, 0.6f + 3.4f * fade, Comet,
                    std::pow(fade, 1.6f));
            }
            const auto tip = At(cx, cy, r + 18, head);
            Disc(tip.x, tip.y, 7, Comet, 0.25f);
            Disc(tip.x, tip.y, 3.2f, Glint, 1);
            const float inner = -std::fmod(view.t * 360 / 3.5f, 360.0f);
            for (int i = 0; i < 3; ++i) {
                Arc(cx, cy, r - 17, inner + i * 120, 34, 2.5f, accent, 0.35f);
            }
        } else {
            // The outcome: a burst that spreads from the ring as the mark pops in.
            const float burst = std::clamp(view.closing / 0.6f, 0.0f, 1.0f);
            Circle(cx, cy, r + 8 + 110 * burst, 3 * (1 - burst) + 0.5f, accent, 0.5f * (1 - burst));
        }

        if (view.outcome == Outcome::None) {
            const int percent = std::clamp(static_cast<int>(view.progress * 100), 0, 99);
            Text(std::to_wstring(percent) + L"%", cx - 100, cy - 38, 44, Palette::Text, 200, 56, true,
                 false, true);
            Text(L"P U L S E", cx - 100, cy + 18, 11, accent, 200, 20, true, false, true);
            return;
        }
        const float pop = EaseOutBack(view.closing / 0.35f);
        const auto point = [&](float x, float y) { return D2D1::Point2F(cx + x * pop, cy + y * pop); };
        if (failed) {
            Line(point(0, -26), point(0, 6), 9 * pop, accent, 1);
            Disc(cx, cy + 24 * pop, 5.5f * pop, accent, 1);
            return;
        }
        ComPtr<ID2D1PathGeometry> path;
        ComPtr<ID2D1GeometrySink> sink;
        winrt::check_hresult(d2d->CreatePathGeometry(&path));
        winrt::check_hresult(path->Open(&sink));
        sink->BeginFigure(point(-27, 1), D2D1_FIGURE_BEGIN_HOLLOW);
        sink->AddLine(point(-8, 20));
        sink->AddLine(point(28, -18));
        sink->EndFigure(D2D1_FIGURE_END_OPEN);
        winrt::check_hresult(sink->Close());
        Color(accent, 0.2f);
        surface->DrawGeometry(path.Get(), brush.Get(), 20 * pop, round_caps.Get());
        Color(accent, 1);
        surface->DrawGeometry(path.Get(), brush.Get(), 10 * pop, round_caps.Get());
    }

    void DrawTexts(const View& view, PulseScreen::Kind kind, std::wstring_view game) {
        using L = Layout;
        const bool save = kind == PulseScreen::Kind::Save;
        const T title = view.outcome == Outcome::Failed ? T::PulseNotSavedTitle
                           : view.outcome == Outcome::Done
                               ? (save ? T::PulseSavedTitle : T::PulseRestoredTitle)
                               : (save ? T::PulseSaving : T::PulseRestoring);
        Text(I18n::Tr(title), 0, L::TitleY, 28, Palette::Text, L::Width, 40, true, false, true);
        if (!game.empty()) {
            Text(game, 20, L::GameY, 17, Palette::TextHint, L::Width - 40, 26, false, false, true);
        }
        if (view.outcome != Outcome::None) return;

        std::wstring step;
        switch (view.stage) {
        case Stage::Preparing: step = I18n::Tr(T::PulseStepPreparing); break;
        case Stage::Capture: step = I18n::Tr(T::PulseStepCapture); break;
        case Stage::Write: step = I18n::Tr(T::PulseStepWrite); break;
        case Stage::Verify: step = I18n::Tr(T::PulseStepVerify); break;
        case Stage::Sync:
        case Stage::Replace: step = I18n::Tr(T::PulseStepSync); break;
        case Stage::Restore: step = I18n::Tr(T::PulseStepRestore); break;
        case Stage::Shaders:
            step = view.shaders_total == 0
                       ? I18n::Tr(T::PulseStepRestore)
                       : I18n::Format(T::PulseStepShaders,
                                      {std::to_wstring(std::min(view.shaders_done, view.shaders_total)),
                                       std::to_wstring(view.shaders_total)});
            break;
        case Stage::Starting: step = I18n::Tr(T::PulseStepStarting); break;
        }
        // The step and three dots after it that light up in turn; the pair is centered.
        constexpr float DotsWidth = 30;
        const float width = MeasureText(step, 16, false);
        Text(step, -DotsWidth / 2, L::StepY, 16, Palette::TextSecondary, L::Width, 28, false, false, true);
        const float dots = L::Width / 2 - DotsWidth / 2 + width / 2 + 8;
        for (int i = 0; i < 3; ++i) {
            const float wave = 0.5f + 0.5f * std::sin(view.t * 6.0f - i * 0.9f);
            Disc(dots + i * 9.0f, L::StepY + 16, 2.2f, Palette::Accent, 0.25f + 0.75f * wave);
        }

        // A thin bar under it, and a shimmer that runs along its filled part.
        constexpr float x = (L::Width - L::BarWidth) / 2;
        Round(x, L::BarY, L::BarWidth, L::BarHeight, L::BarHeight / 2, Palette::Track);
        const float filled = L::BarWidth * std::clamp(view.progress, 0.0f, 1.0f);
        if (filled > 0.5f) {
            Round(x, L::BarY, filled, L::BarHeight, L::BarHeight / 2, Palette::Accent);
            surface->PushAxisAlignedClip(D2D1::RectF(x, L::BarY, x + filled, L::BarY + L::BarHeight),
                                         D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            constexpr int Slices = 11;
            constexpr float Slice = 4;
            const float start = x - Slices * Slice +
                                std::fmod(view.t, 1.6f) / 1.6f * (L::BarWidth + 2 * Slices * Slice);
            for (int i = 0; i < Slices; ++i) {
                const float peak = 1 - std::abs(i - Slices / 2.0f) / (Slices / 2.0f + 0.5f);
                Color(Glint, 0.75f * peak);
                surface->FillRectangle(D2D1::RectF(start + i * Slice, L::BarY, start + (i + 1) * Slice,
                                                   L::BarY + L::BarHeight),
                                       brush.Get());
            }
            surface->PopAxisAlignedClip();
        }
        Text(I18n::Tr(T::PulseKeepOn), 0, L::HintY, 13, Palette::TextSecondary, L::Width, 24, false,
             false, true);
    }

    /// The bitmap as RGBA rows, the swapchain's order (D2D draws BGRA).
    D3D12::MenuImage Pixels() {
        D3D12::MenuImage image{bitmap_width, bitmap_height, {}, true, Palette::Backdrop};
        image.pixels.resize(static_cast<std::size_t>(bitmap_width) * bitmap_height * 4);
        const WICRect rect{0, 0, static_cast<INT>(bitmap_width), static_cast<INT>(bitmap_height)};
        ComPtr<IWICBitmapLock> lock;
        winrt::check_hresult(bitmap->Lock(&rect, WICBitmapLockRead, &lock));
        UINT stride = 0, size = 0;
        BYTE* data = nullptr;
        winrt::check_hresult(lock->GetStride(&stride));
        winrt::check_hresult(lock->GetDataPointer(&size, &data));
        for (UINT y = 0; y < bitmap_height; ++y) {
            const BYTE* in = data + static_cast<std::size_t>(y) * stride;
            u8* out = image.pixels.data() + static_cast<std::size_t>(y) * bitmap_width * 4;
            for (UINT x = 0; x < bitmap_width; ++x, in += 4, out += 4) {
                out[0] = in[2];
                out[1] = in[1];
                out[2] = in[0];
                out[3] = 0xFF;
            }
        }
        return image;
    }

    ComPtr<IWICImagingFactory> wic;
    ComPtr<ID2D1Factory> d2d;
    ComPtr<IDWriteFactory> text_factory;
    ComPtr<ID2D1StrokeStyle> round_caps;
    ComPtr<IWICBitmap> bitmap;
    ComPtr<ID2D1RenderTarget> target;
    UINT bitmap_width{}, bitmap_height{};
};
} // namespace

struct PulseScreen::Impl {
    Impl(Core::System& system_, Kind kind_, std::wstring game_)
        : system{system_}, kind{kind_}, game{std::move(game_)} {
        stage = kind == Kind::Save ? Stage::Preparing : Stage::Restore;
        thread = std::jthread{[this](std::stop_token stop) { Run(stop); }};
        std::unique_lock lock{mutex};
        // Shown before the caller goes on: a save's capture starts right after.
        changed.wait_for(lock, std::chrono::seconds{1}, [this] { return first_shown || exited; });
    }

    ~Impl() {
        thread.request_stop();
        changed.notify_all();
        if (thread.joinable()) thread.join();
        if (kind == Kind::Resume && shown_any && !hidden) {
            system.GPU().RunOnGpuThread([this] { D3D12::HideGameMenu(system.Renderer()); });
        }
    }

    void SetStage(Stage next) {
        std::scoped_lock lock{mutex};
        if (stage == next) return;
        stage = next;
        stage_start = Clock::now();
    }

    void Run(std::stop_token stop) {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        struct Uninitialize {
            ~Uninitialize() { winrt::uninit_apartment(); }
        } uninitialize;
        try {
            Animate(stop);
        } catch (const std::exception& e) {
            LOG_WARNING(Frontend, "Pulse screen stopped: {}", e.what());
        } catch (const winrt::hresult_error& e) {
            LOG_WARNING(Frontend, "Pulse screen stopped: {}", winrt::to_string(e.message()));
        }
        std::scoped_lock lock{mutex};
        exited = true;
        changed.notify_all();
    }

    void Animate(std::stop_token stop) {
        Painter painter;
        const auto opened = Clock::now();
        auto previous = opened;
        float progress = 0;
        auto next = opened;
        while (!stop.stop_requested()) {
            const auto now = Clock::now();
            View view{};
            bool hold = false;
            {
                std::scoped_lock lock{mutex};
                if (kind == Kind::Resume && stage == Stage::Starting && outcome == Outcome::None) {
                    const auto frames = system.GetPerfStats().TotalGameFrames();
                    if (frames >= start_frames + GameFramesToClose ||
                        Seconds(now - stage_start) > StartingTimeoutSeconds) {
                        outcome = Outcome::Done;
                        outcome_at = now;
                    }
                }
                view.stage = stage;
                view.outcome = outcome;
                view.closing = outcome == Outcome::None ? 0 : Seconds(now - outcome_at);
                view.shaders_done = shaders_done.load(std::memory_order_relaxed);
                view.shaders_total = shaders_total.load(std::memory_order_relaxed);
                hold = stage == Stage::Capture && outcome == Outcome::None;
                // The goal the bar eases to; it never goes back.
                float goal = 1;
                if (outcome == Outcome::None) {
                    const Range range = RangeOf(stage);
                    float share = 1 - std::exp(-Seconds(now - stage_start) / range.tau);
                    if (stage == Stage::Shaders && view.shaders_total != 0) {
                        share = static_cast<float>(std::min(view.shaders_done, view.shaders_total)) /
                                static_cast<float>(view.shaders_total);
                    }
                    goal = range.from + (range.to - range.from) * share;
                }
                const float smoothing = outcome == Outcome::None ? 0.12f : 0.06f;
                progress = std::max(progress, progress + (goal - progress) *
                                                             (1 - std::exp(-Seconds(now - previous) / smoothing)));
                view.progress = progress;
            }
            previous = now;
            view.t = Seconds(now - opened);

            if (!hold) {
                auto image = painter.Draw(view, kind, game, D3D12::GameMenuScreenSize(system.Renderer()));
                std::scoped_lock present{present_mutex};
                // Rechecked under the lock SaveStep takes before the capture.
                bool capturing;
                {
                    std::scoped_lock lock{mutex};
                    capturing = stage == Stage::Capture && outcome == Outcome::None;
                }
                if (!capturing && !image.pixels.empty()) {
                    system.GPU().RunOnGpuThread(
                        [&] { D3D12::ShowGameMenu(system.Renderer(), std::move(image)); });
                    shown_any = true;
                }
            }
            {
                std::scoped_lock lock{mutex};
                if (!first_shown) {
                    first_shown = true;
                    changed.notify_all();
                }
            }
            if (view.outcome != Outcome::None && view.closing >= CloseSeconds) {
                if (kind == Kind::Resume) {
                    system.GPU().RunOnGpuThread([this] { D3D12::HideGameMenu(system.Renderer()); });
                    hidden = true;
                }
                return;
            }
            next += FramePeriod;
            if (next < Clock::now()) next = Clock::now();
            std::unique_lock lock{mutex};
            changed.wait_until(lock, next, [&] { return stop.stop_requested(); });
        }
    }

    Core::System& system;
    const Kind kind;
    const std::wstring game;

    std::mutex mutex;
    std::condition_variable changed;
    Stage stage;
    Clock::time_point stage_start{Clock::now()};
    Outcome outcome{Outcome::None};
    Clock::time_point outcome_at;
    unsigned long long start_frames{};
    std::atomic<std::size_t> shaders_done{}, shaders_total{};
    bool first_shown{}, exited{};

    /// Held while a frame is presented; the capture waits for it.
    std::mutex present_mutex;
    // Written by the drawing thread only, read after it is joined.
    bool shown_any{}, hidden{};

    std::jthread thread;
};

PulseScreen::PulseScreen(Core::System& system, Kind kind, std::wstring game)
    : impl{std::make_unique<Impl>(system, kind, std::move(game))} {}

PulseScreen::~PulseScreen() = default;

void PulseScreen::SaveStep(std::string_view step) {
    Stage next = Stage::Capture;
    if (step == "write") next = Stage::Write;
    else if (step == "verify") next = Stage::Verify;
    else if (step == "sync") next = Stage::Sync;
    else if (step == "replace") next = Stage::Replace;
    if (next == Stage::Capture) {
        // No frame in flight once this returns, and none starts until the capture is over.
        std::scoped_lock present{impl->present_mutex};
        impl->SetStage(next);
        return;
    }
    impl->SetStage(next);
}

void PulseScreen::ShadersStarting() {
    impl->SetStage(Stage::Shaders);
}

void PulseScreen::Shaders(std::size_t done, std::size_t total) {
    impl->shaders_done.store(done, std::memory_order_relaxed);
    impl->shaders_total.store(total, std::memory_order_relaxed);
}

void PulseScreen::GameStarting() {
    std::scoped_lock lock{impl->mutex};
    impl->stage = Stage::Starting;
    impl->stage_start = Clock::now();
    impl->start_frames = impl->system.GetPerfStats().TotalGameFrames();
}

void PulseScreen::Finish(bool saved) {
    {
        std::scoped_lock lock{impl->mutex};
        if (impl->outcome == Outcome::None) {
            impl->outcome = saved ? Outcome::Done : Outcome::Failed;
            impl->outcome_at = Clock::now();
        }
    }
    // Drawing stops by itself once the closing frame has been seen.
    if (impl->thread.joinable()) impl->thread.join();
}

} // namespace EdenXbox
