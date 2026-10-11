// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#include "eden_uwp/game_menu_surface.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <winrt/base.h>

#include "eden_uwp/cheats/cheats_view.h"
#include "eden_uwp/game_menu.h"
#include "eden_uwp/localization.h"
#include "eden_uwp/text_entry_panel.h"
#include "eden_uwp/ui_painter.h"

namespace EdenXbox {
namespace {
using Microsoft::WRL::ComPtr;
using I18n::Text;

/// The menu's list, in layout units of the 1280x720 screen.
struct MenuLayout {
    static constexpr float Width = 640, Margin = 32;
    static constexpr float Header = 96, RowStride = 58, RowHeight = 50, Footer = 72;
    static float Height(std::size_t rows) { return Header + rows * RowStride + Footer; }
};
} // namespace

struct GameMenuSurface::Impl : UiPainter {
    Impl() {
        winrt::check_hresult(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                              IID_PPV_ARGS(&wic)));
        winrt::check_hresult(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, IID_PPV_ARGS(&d2d)));
        winrt::check_hresult(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                                 reinterpret_cast<IUnknown**>(text_factory.GetAddressOf())));
    }

    /// Draws `width` x `height` layout units with `draw` and returns the pixels. The scale follows
    /// the screen's height (720 units) up to 1.5, so the image never outgrows a 1080p panel.
    template <typename Draw>
    D3D12::MenuImage Render(float width, float height, std::pair<u32, u32> screen, Draw&& draw) {
        const auto [screen_width, screen_height] = screen;
        if (screen_width == 0 || screen_height == 0) return {};
        const float scale = std::min({static_cast<float>(screen_height) / 720.0f, 1.5f,
                                      static_cast<float>(screen_width) * 0.95f / width,
                                      static_cast<float>(screen_height) * 0.95f / height});
        const auto pixels_wide = static_cast<UINT>(std::ceil(width * scale));
        const auto pixels_high = static_cast<UINT>(std::ceil(height * scale));
        if (!bitmap || pixels_wide != bitmap_width || pixels_high != bitmap_height) {
            // A new size (another page, or the first draw): the bitmap and its target follow it.
            target.Reset();
            bitmap.Reset();
            winrt::check_hresult(wic->CreateBitmap(pixels_wide, pixels_high, GUID_WICPixelFormat32bppPBGRA,
                                                   WICBitmapCacheOnDemand, &bitmap));
            const auto properties = D2D1::RenderTargetProperties(
                D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            winrt::check_hresult(d2d->CreateWicBitmapRenderTarget(bitmap.Get(), properties, &target));
            target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
            Attach(target.Get(), text_factory.Get());
            bitmap_width = pixels_wide;
            bitmap_height = pixels_high;
        }
        SyncLanguage();
        target->BeginDraw();
        // The image is copied, not blended: the panel's color fills it to its square corners.
        target->Clear(D2D1::ColorF(Palette::Panel));
        target->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale));
        draw();
        Round(0, 0, width, 4, 0, Palette::Accent);
        winrt::check_hresult(target->EndDraw());
        return Pixels();
    }

    /// The bitmap as RGBA rows, the swapchain's order (D2D draws BGRA).
    D3D12::MenuImage Pixels() {
        D3D12::MenuImage image{bitmap_width, bitmap_height, {}};
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

    void DrawMenu(const GameMenu& menu, std::wstring_view game) {
        using L = MenuLayout;
        PageHeader(L::Margin, 22, UiIcon::Game, I18n::Tr(Text::MenuTitle), game);
        const auto rows = menu.Rows();
        const float row_width = L::Width - 2 * L::Margin;
        for (std::size_t i = 0; i < rows.size(); ++i) {
            const auto& row = rows[i];
            const float top = L::Header + i * L::RowStride;
            const bool active = i == menu.Selected();
            Round(L::Margin, top, row_width, L::RowHeight, 12, active ? Palette::RowActive : Palette::Row);
            if (active) Round(L::Margin, top + 12, 3, L::RowHeight - 24, 1.5f, Palette::Accent);
            Text(row.label, L::Margin + 22, top + 11, 19, Palette::Text, 300, 30, active);
            const unsigned accent = active ? Palette::Accent : Palette::TextSecondary;
            const float right = L::Margin + row_width - 22;
            const float middle = top + L::RowHeight / 2;
            if (row.kind == GameMenuRow::Kind::Choice) {
                Chevron(L::Margin + 330, middle, accent, true);
                Chevron(right, middle, accent);
            } else if (row.kind == GameMenuRow::Kind::Page) {
                Chevron(right, middle, accent);
            }
            if (!row.value.empty())
                Text(row.value, L::Margin + 342, top + 12, 18, active ? Palette::Text : Palette::TextHint,
                     right - 16 - (L::Margin + 342), 30, false, false, false, true);
        }
        float x = L::Margin;
        const float footer = L::Height(rows.size()) - L::Footer + 20;
        x += Prompt(L"A", ButtonColor::A, I18n::Tr(Text::Select), x, footer);
        x += Prompt(L"B", ButtonColor::B, I18n::Tr(Text::Back), x, footer);
        Prompt(L"↔", ButtonColor::Neutral, I18n::Tr(Text::Change), x, footer);
    }

    void DrawCheats(const Cheats::CheatsView& view) {
        using L = Cheats::CheatsLayout;
        Cheats::DrawCheatsPanel(*this, view);
        float x = L::Margin;
        x += Prompt(L"A", ButtonColor::A, I18n::Tr(Text::ToggleCheat), x, L::Footer);
        x += Prompt(L"X", ButtonColor::X, I18n::Tr(Text::AllCheats), x, L::Footer);
        if (Cheats::CheatTabs(view.session.Catalog()).size() > 1)
            x += Prompt(L"LB", ButtonColor::Neutral, I18n::Tr(Text::Category), x, L::Footer);
        x += Prompt(L"Y", ButtonColor::Y, I18n::Tr(Text::Refresh), x, L::Footer);
        Prompt(L"B", ButtonColor::B, I18n::Tr(Text::Back), x, L::Footer);
    }

    void DrawKeyboard(const TextEntryPanelView& view, bool can_cancel) {
        using K = TextEntryPanelLayout;
        DrawTextEntryPanel(*this, view, 0, 0);
        // The pad as the console's keyboard: A types, X deletes, Y adds a space; the bumpers stand
        // in for + and -, which the game menu's shortcut holds.
        float x = K::Left;
        const float y = K::Footer;
        x += Prompt(L"A", ButtonColor::A, I18n::Tr(Text::TypeKey), x, y);
        x += Prompt(L"X", ButtonColor::X, I18n::Tr(Text::DeleteKey), x, y);
        x += Prompt(L"Y", ButtonColor::Y, I18n::Tr(Text::SpaceKey), x, y);
        x += Prompt(L"LB", ButtonColor::Neutral, I18n::Tr(Text::ShiftKey), x, y);
        x += Prompt(L"RB", ButtonColor::Neutral, I18n::Tr(Text::DoneKey), x, y);
        if (can_cancel) Prompt(L"B", ButtonColor::B, I18n::Tr(Text::Cancel), x, y);
    }

    ComPtr<IWICImagingFactory> wic;
    ComPtr<ID2D1Factory> d2d;
    ComPtr<IDWriteFactory> text_factory;
    ComPtr<IWICBitmap> bitmap;
    ComPtr<ID2D1RenderTarget> target;
    UINT bitmap_width{};
    UINT bitmap_height{};
};

GameMenuSurface::GameMenuSurface() : impl{std::make_unique<Impl>()} {}
GameMenuSurface::~GameMenuSurface() = default;

D3D12::MenuImage GameMenuSurface::DrawMenu(const GameMenu& menu, std::wstring_view game,
                                          std::pair<u32, u32> screen) {
    return impl->Render(MenuLayout::Width, MenuLayout::Height(menu.Rows().size()), screen,
                        [&] { impl->DrawMenu(menu, game); });
}

D3D12::MenuImage GameMenuSurface::DrawCheats(const Cheats::CheatsView& view, std::pair<u32, u32> screen) {
    using L = Cheats::CheatsLayout;
    return impl->Render(L::Width, L::Height, screen, [&] { impl->DrawCheats(view); });
}

D3D12::MenuImage GameMenuSurface::DrawKeyboard(const TextEntryPanelView& view, bool can_cancel,
                                              std::pair<u32, u32> screen) {
    using K = TextEntryPanelLayout;
    return impl->Render(K::Width, K::Height, screen, [&] { impl->DrawKeyboard(view, can_cancel); });
}

} // namespace EdenXbox
