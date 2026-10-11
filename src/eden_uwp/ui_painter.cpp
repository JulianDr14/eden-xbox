// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#include "eden_uwp/ui_painter.h"

#include <array>
#include <winrt/base.h>

#include "eden_uwp/localization.h"

namespace EdenXbox {
using Microsoft::WRL::ComPtr;

void UiPainter::Attach(ID2D1RenderTarget* target, IDWriteFactory* write_) {
    surface = target;
    write = write_;
    brush.Reset();
    winrt::check_hresult(surface->CreateSolidColorBrush(D2D1::ColorF(Palette::Text), &brush));
}

void UiPainter::SyncLanguage() {
    const auto tag = I18n::CurrentUiLanguage().tag;
    if (tag != format_locale) {
        formats.clear();
        format_locale = std::wstring{tag};
    }
}

void UiPainter::Round(float x, float y, float w, float h, float radius, unsigned color) {
    brush->SetColor(D2D1::ColorF(color));
    surface->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, y, x + w, y + h), radius, radius),
                                  brush.Get());
}

IDWriteTextFormat* UiPainter::Format(float size, bool bold, bool wrap, bool center, bool trailing) {
    const unsigned key = static_cast<unsigned>(size) * 16 + (bold ? 1 : 0) + (wrap ? 2 : 0) +
                         (center ? 4 : 0) + (trailing ? 8 : 0);
    auto& format = formats[key];
    if (!format) {
        winrt::check_hresult(write->CreateTextFormat(L"Segoe UI", nullptr,
            bold ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, format_locale.c_str(), &format));
        if (center) {
            format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        } else if (trailing) {
            format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        }
        format->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
        DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        ComPtr<IDWriteInlineObject> ellipsis;
        winrt::check_hresult(write->CreateEllipsisTrimmingSign(format.Get(), &ellipsis));
        format->SetTrimming(&trim, ellipsis.Get());
    }
    return format.Get();
}

void UiPainter::Text(std::wstring_view text, float x, float y, float size, unsigned color,
                     float width, float height, bool bold, bool wrap, bool center, bool trailing) {
    brush->SetColor(D2D1::ColorF(color));
    surface->DrawText(text.data(), static_cast<UINT32>(text.size()),
                      Format(size, bold, wrap, center, trailing),
                      D2D1::RectF(x, y, x + width, y + height), brush.Get(),
                      D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

float UiPainter::MeasureText(std::wstring_view text, float size, bool bold) {
    if (text.empty()) return 0;
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(write->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()),
                                       Format(size, bold, false, false, false), 4096, 100, &layout)))
        return 0;
    DWRITE_TEXT_METRICS metrics{};
    layout->GetMetrics(&metrics);
    return metrics.widthIncludingTrailingWhitespace;
}

void UiPainter::Icon(UiIcon icon, float x, float y, unsigned color) {
    brush->SetColor(D2D1::ColorF(color));
    const auto line = [&](float ax, float ay, float bx, float by) {
        surface->DrawLine(D2D1::Point2F(x + ax, y + ay), D2D1::Point2F(x + bx, y + by), brush.Get(), 2);
    };
    const auto circle = [&](float cx, float cy, float radius) {
        surface->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(x + cx, y + cy), radius, radius), brush.Get(), 2);
    };
    switch (icon) {
    case UiIcon::Folder:
    case UiIcon::Remove:
        line(3, 10, 3, 28); line(3, 28, 29, 28); line(29, 28, 29, 10);
        line(29, 10, 16, 10); line(16, 10, 12, 6); line(12, 6, 3, 6); line(3, 6, 3, 10);
        line(3, 14, 29, 14);
        if (icon == UiIcon::Remove) line(11, 21, 21, 21);
        break;
    case UiIcon::Controller:
        surface->DrawRoundedRectangle(D2D1::RoundedRect(
            D2D1::RectF(x + 1, y + 7, x + 31, y + 27), 7, 7), brush.Get(), 2);
        line(7, 17, 15, 17); line(11, 13, 11, 21);
        circle(23, 14, 1.4f); circle(26, 20, 1.4f);
        break;
    case UiIcon::Key:
        circle(10, 12, 7); circle(10, 12, 2);
        line(15, 17, 28, 30); line(22, 24, 26, 20); line(26, 28, 30, 24);
        break;
    case UiIcon::Firmware:
        surface->DrawRoundedRectangle(D2D1::RoundedRect(
            D2D1::RectF(x + 7, y + 7, x + 25, y + 25), 3, 3), brush.Get(), 2);
        surface->DrawRectangle(D2D1::RectF(x + 12, y + 12, x + 20, y + 20), brush.Get(), 2);
        for (float p : {11.0f, 21.0f}) {
            line(p, 2, p, 7); line(p, 25, p, 30);
            line(2, p, 7, p); line(25, p, 30, p);
        }
        break;
    case UiIcon::User:
        circle(16, 10, 6);
        line(5, 29, 7, 22); line(7, 22, 12, 19); line(12, 19, 20, 19);
        line(20, 19, 25, 22); line(25, 22, 27, 29); line(27, 29, 5, 29);
        break;
    case UiIcon::Language:
        circle(16, 16, 12);
        surface->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(x + 16, y + 16), 5, 12), brush.Get(), 2);
        line(4, 16, 28, 16); line(7, 10, 25, 10); line(7, 22, 25, 22);
        break;
    case UiIcon::Region:
        circle(16, 12, 7); circle(16, 12, 2.5f);
        line(10, 16, 16, 28); line(16, 28, 22, 16);
        break;
    case UiIcon::Cache:
        line(6, 9, 26, 9); line(13, 9, 13, 5); line(13, 5, 19, 5); line(19, 5, 19, 9);
        line(8, 9, 10, 28); line(10, 28, 22, 28); line(22, 28, 24, 9);
        line(13, 13, 13, 24); line(19, 13, 19, 24);
        break;
    case UiIcon::Settings: {
        circle(16, 16, 10); circle(16, 16, 4);
        static constexpr std::array<D2D1_POINT_2F, 8> directions{{
            {0, -1}, {0.707f, -0.707f}, {1, 0}, {0.707f, 0.707f},
            {0, 1}, {-0.707f, 0.707f}, {-1, 0}, {-0.707f, -0.707f}}};
        for (const auto d : directions)
            line(16 + 11 * d.x, 16 + 11 * d.y, 16 + 15 * d.x, 16 + 15 * d.y);
        break;
    }
    case UiIcon::Cheats:
        // A spark: a four-pointed star and a small one beside it.
        line(13, 3, 16, 13); line(16, 13, 26, 16); line(26, 16, 16, 19); line(16, 19, 13, 29);
        line(13, 29, 10, 19); line(10, 19, 0, 16); line(0, 16, 10, 13); line(10, 13, 13, 3);
        line(26, 2, 26, 10); line(22, 6, 30, 6);
        break;
    case UiIcon::Game:
        surface->DrawRoundedRectangle(D2D1::RoundedRect(
            D2D1::RectF(x + 3, y + 3, x + 29, y + 29), 6, 6), brush.Get(), 2);
        line(9, 22, 14, 13); line(14, 13, 18, 19); line(18, 19, 20, 16); line(20, 16, 24, 22);
        circle(21, 10, 2);
        break;
    case UiIcon::Mods:
        // Layers stacked over the game: what mods are to the loader.
        line(16, 4, 28, 10); line(28, 10, 16, 16); line(16, 16, 4, 10); line(4, 10, 16, 4);
        line(4, 16, 16, 22); line(16, 22, 28, 16);
        line(4, 22, 16, 28); line(16, 28, 28, 22);
        break;
    case UiIcon::Archive:
        // A box with a zip down its middle.
        surface->DrawRoundedRectangle(D2D1::RoundedRect(
            D2D1::RectF(x + 6, y + 3, x + 26, y + 29), 3, 3), brush.Get(), 2);
        for (float p : {6.0f, 12.0f}) { line(14, p, 16, p); line(16, p + 3, 18, p + 3); }
        surface->DrawRectangle(D2D1::RectF(x + 13, y + 18, x + 19, y + 25), brush.Get(), 2);
        break;
    }
}

void UiPainter::Chevron(float cx, float cy, unsigned color, bool left) {
    brush->SetColor(D2D1::ColorF(color));
    const float d = left ? -1.0f : 1.0f;
    surface->DrawLine(D2D1::Point2F(cx - 4 * d, cy - 5), D2D1::Point2F(cx + 1 * d, cy), brush.Get(), 2);
    surface->DrawLine(D2D1::Point2F(cx + 1 * d, cy), D2D1::Point2F(cx - 4 * d, cy + 5), brush.Get(), 2);
}

void UiPainter::ScrollIndicator(float x, float y, float height, std::size_t first,
                                std::size_t visible, std::size_t count, bool more_below) {
    if (height < 48 || !visible || (!more_below && count <= visible)) return;
    first = std::min(first, count > visible ? count - visible : std::size_t{0});
    const bool above = first > 0;
    const bool below = more_below || visible < count - first;
    const auto arrow = [&](float cy, bool up, bool available) {
        brush->SetColor(D2D1::ColorF(available ? Palette::Accent : Palette::Track));
        const float tip = cy + (up ? -3.0f : 3.0f);
        const float base = cy + (up ? 3.0f : -3.0f);
        surface->DrawLine(D2D1::Point2F(x - 4, base), D2D1::Point2F(x, tip), brush.Get(), 2);
        surface->DrawLine(D2D1::Point2F(x, tip), D2D1::Point2F(x + 4, base), brush.Get(), 2);
    };
    arrow(y + 5, true, above);
    arrow(y + height - 5, false, below);
    const float track_y = y + 18, track_h = height - 36;
    const float fraction = count ? std::min(1.0f, static_cast<float>(visible) / count) : 1.0f;
    const float thumb_h = std::min(track_h, std::max(20.0f, track_h * fraction));
    const float position = count > visible ? static_cast<float>(first) / (count - visible) : 0.0f;
    Round(x - 2, track_y, 4, track_h, 2, Palette::Track);
    Round(x - 3, track_y + (track_h - thumb_h) * position, 6, thumb_h, 3, Palette::Accent);
}

void UiPainter::RowFrame(float x, float y, float width, float height, bool active, UiIcon icon) {
    Round(x, y, width, height, 12, active ? Palette::RowActive : Palette::Row);
    if (active) Round(x, y + 14, 3, height - 28, 1.5f, Palette::Accent);
    Icon(icon, x + 20, y + (height - 32) / 2, active ? Palette::Accent : Palette::TextSecondary);
}

void UiPainter::PageHeader(float x, float y, UiIcon icon, std::wstring_view title,
                           std::wstring_view subtitle) {
    Round(x, y + 5, 48, 48, 12, Palette::RowActive);
    Icon(icon, x + 8, y + 13, Palette::Accent);
    Text(title, x + 66, y, 32, Palette::Text, 850, 50, true);
    Text(subtitle, x + 66, y + 47, 17, Palette::TextSecondary, 790);
}

void UiPainter::Switch(float x, float y, bool on, bool active) {
    Round(x, y, 52, 28, 14, on ? Palette::Accent : Palette::Track);
    if (active && !on) {
        brush->SetColor(D2D1::ColorF(Palette::TextSecondary));
        surface->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, y, x + 52, y + 28), 14, 14),
                                      brush.Get(), 1.5f);
    }
    const float knob = on ? x + 38 : x + 14;
    brush->SetColor(D2D1::ColorF(on ? Palette::OnAccent : Palette::Text));
    surface->FillEllipse(D2D1::Ellipse(D2D1::Point2F(knob, y + 14), 10, 10), brush.Get());
}

void UiPainter::ButtonGlyph(std::wstring_view letters, float x, float y, unsigned color) {
    brush->SetColor(D2D1::ColorF(color));
    if (letters.size() > 1) {
        surface->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, y + 5, x + 38, y + 27), 8, 8),
                                      brush.Get(), 2);
        Text(letters, x, y + 5, 13, color, 38, 22, true, false, true);
        return;
    }
    surface->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(x + 16, y + 16), 13, 13), brush.Get(), 2);
    Text(letters, x, y, 18, color, 32, 32, true, false, true);
}

float UiPainter::Prompt(std::wstring_view letters, unsigned color, std::wstring_view label, float x,
                        float y) {
    ButtonGlyph(letters, x, y, color);
    const float glyph = letters.size() > 1 ? 46.0f : 40.0f;
    const float width = MeasureText(label, 19, false);
    Text(label, x + glyph, y + 3, 19, Palette::TextSecondary, width + 4);
    return glyph + width + 28;
}

} // namespace EdenXbox
