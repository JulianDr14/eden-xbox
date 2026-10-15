// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#include "eden_uwp/cheats/cheats_view.h"

#include <algorithm>
#include <string>

#include "eden_uwp/localization.h"
#include "eden_uwp/ui_painter.h"
#include "eden_uwp/utf8.h"

namespace EdenXbox::Cheats {
namespace {
using I18n::Text;
using L = CheatsLayout;

std::wstring_view CategoryName(CheatCategory category) {
    switch (category) {
    case CheatCategory::Gameplay: return I18n::Tr(Text::CheatsCategoryGameplay);
    case CheatCategory::Graphics: return I18n::Tr(Text::CheatsCategoryGraphics);
    case CheatCategory::Mine: return I18n::Tr(Text::CheatsCategoryMine);
    }
    return {};
}

std::wstring_view SourceText(CheatSource source) {
    switch (source) {
    case CheatSource::Checking: return I18n::Tr(Text::CheatsSourceChecking);
    case CheatSource::Online: return I18n::Tr(Text::CheatsSourceOnline);
    case CheatSource::Cached: return I18n::Tr(Text::CheatsSourceCached);
    case CheatSource::Offline: return I18n::Tr(Text::CheatsOffline);
    }
    return {};
}

/// Tabs with how many of their cheats are on; LB and RB beside them when there is a choice.
void DrawTabs(UiPainter& paint, const CheatCatalog& catalog, CheatCategory current, float x, float y) {
    std::array<CheatCategory, CategoryCount> storage;
    const auto tabs = CheatTabs(catalog, storage);
    float left = x + L::Margin;
    if (tabs.size() > 1) {
        paint.ButtonGlyph(L"LB", left, y + 3, ButtonColor::Neutral);
        left += 50;
    }
    for (const CheatCategory category : tabs) {
        const std::wstring label = std::wstring{CategoryName(category)} + L"  " +
                                   std::to_wstring(catalog.EnabledCount(category)) + L"/" +
                                   std::to_wstring(catalog.Cheats(category).size());
        const bool active = category == current;
        const float width = paint.MeasureText(label, 17, true) + 36;
        paint.Round(left, y, width, L::TabHeight, L::TabHeight / 2, active ? Palette::Accent : Palette::Row);
        paint.Text(label, left, y, 17, active ? Palette::OnAccent : Palette::TextSecondary, width,
                   L::TabHeight, true, false, true);
        left += width + 10;
    }
    if (tabs.size() > 1) paint.ButtonGlyph(L"RB", left + 2, y + 3, ButtonColor::Neutral);
}

void DrawList(UiPainter& paint, const CheatCatalog& catalog, const CheatsPage& page, float x, float y) {
    const auto cheats = catalog.Cheats(page.category);
    const float row_width = L::Width - 2 * L::Margin;
    const std::size_t end = std::min(cheats.size(), page.first + L::VisibleRows);
    for (std::size_t i = page.first; i < end; ++i) {
        const float top = y + L::RowTop + static_cast<float>(i - page.first) * L::RowStride;
        const float left = x + L::Margin;
        const bool active = i == page.selected;
        const bool on = catalog.Enabled(cheats[i]);
        paint.Round(left, top, row_width, L::RowHeight, 12, active ? Palette::RowActive : Palette::Row);
        if (active) paint.Round(left, top + 12, 3, L::RowHeight - 24, 1.5f, Palette::Accent);
        paint.Text(Utf8ToUtf16(catalog.Name(cheats[i])), left + 24, top + 9, 19,
                   on || active ? Palette::Text : Palette::TextHint, row_width - 130, 30, active);
        paint.Switch(left + row_width - 76, top + 9, on, active);
    }
    paint.ScrollIndicator(x + L::Width - L::Margin + 14, y + L::RowTop,
                          L::VisibleRows * L::RowStride - (L::RowStride - L::RowHeight),
                          page.first, L::VisibleRows, cheats.size());
}

/// Why the list is empty, in its place.
void DrawEmpty(UiPainter& paint, const CheatSession& session, float x, float y) {
    const float width = L::Width - 2 * L::Margin;
    const float top = y + L::RowTop + 70;
    std::wstring message;
    if (session.Loading() || session.Source() == CheatSource::Checking) {
        message = I18n::Tr(Text::CheatsLoading);
    } else if (session.Source() == CheatSource::Offline) {
        message = I18n::Tr(Text::CheatsOffline);
    } else {
        message = I18n::Tr(Text::CheatsNone);
        if (session.OtherBuilds())
            message += L"\n" + I18n::Plural(Text::CheatsOtherBuildsOne, Text::CheatsOtherBuildsOther,
                                            session.OtherBuilds());
    }
    paint.Icon(UiIcon::Cheats, x + L::Width / 2 - 16, top - 50, Palette::TextSecondary);
    paint.Text(message, x + L::Margin, top, 20, Palette::TextHint, width, 120, false, true, true);
}

void DrawStatus(UiPainter& paint, const CheatsView& view, float x, float y) {
    const CheatSession& session = view.session;
    const std::size_t used = session.Catalog().UsedWords();
    const float left = x + L::Margin;
    const float right = x + L::Width - L::Margin;
    // The cheat engine's room: what the enabled cheats take of the VM's program.
    paint.Text(I18n::Format(Text::CheatsBudget, {std::to_wstring(used), std::to_wstring(WordBudget)}),
               left, y + L::Status, 15, Palette::TextSecondary, 320, 22);
    constexpr float BarWidth = 300;
    const float filled = BarWidth * static_cast<float>(std::min(used, WordBudget)) / WordBudget;
    paint.Round(left, y + L::Status + 26, BarWidth, 8, 4, Palette::Track);
    if (filled > 0)
        paint.Round(left, y + L::Status + 26, std::max(filled, 8.0f), 8, 4,
                    used * 5 > WordBudget * 4 ? Palette::Warning : Palette::Accent);
    // Where the cheats came from, and who made them.
    paint.Text(SourceText(session.Source()), left + 340, y + L::Status, 15,
               session.Source() == CheatSource::Online ? Palette::Accent : Palette::Warning,
               right - left - 340, 22, false, false, false, true);
    std::wstring credit{I18n::Tr(Text::CheatsCredit)};
    for (std::size_t i = 0; i < std::min<std::size_t>(session.Credits().size(), 3); ++i)
        credit += (i ? L", " : L" · ") + Utf8ToUtf16(session.Credits()[i]);
    paint.Text(credit, left + 340, y + L::Status + 20, 13, Palette::TextSecondary, right - left - 340,
               20, false, false, false, true);
    // The last press's outcome, or when changes take effect.
    const bool notice = !view.notice.empty();
    const std::wstring_view note = notice ? view.notice
                                   : I18n::Tr(view.running ? Text::CheatsAppliesNow : Text::CheatsAppliesAtBoot);
    paint.Text(note, left, y + L::Note, 15, notice ? Palette::Warning : Palette::TextHint, right - left, 22);
}
} // namespace

void DrawCheatsPanel(UiPainter& paint, const CheatsView& view, float x, float y) {
    const CheatCatalog& catalog = view.session.Catalog();
    paint.Round(x, y, L::Width, L::Height, 24, Palette::Panel);
    paint.PageHeader(x + L::Margin, y + 26, UiIcon::Cheats, I18n::Tr(Text::CheatsTitle), view.game);
    std::size_t on = 0;
    for (const auto category : {CheatCategory::Gameplay, CheatCategory::Graphics, CheatCategory::Mine})
        on += catalog.EnabledCount(category);
    if (on)
        paint.Text(I18n::Plural(Text::CheatsOnOne, Text::CheatsOnOther, on), x + L::Width - 300 - L::Margin,
                   y + 40, 20, Palette::Accent, 300, 30, true, false, false, true);
    if (catalog.Empty()) {
        DrawEmpty(paint, view.session, x, y);
    } else {
        DrawTabs(paint, catalog, view.page.category, x, y + L::TabsTop);
        DrawList(paint, catalog, view.page, x, y);
    }
    DrawStatus(paint, view, x, y);
}

} // namespace EdenXbox::Cheats
