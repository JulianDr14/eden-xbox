// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#include "eden_uwp/text_entry_panel.h"

#include <algorithm>
#include <string>

#include "eden_uwp/localization.h"
#include "eden_uwp/ui_painter.h"

namespace EdenXbox {

void DrawTextEntryPanel(UiPainter& painter, const TextEntryPanelView& view, float x, float y) {
    using K = TextEntryPanelLayout;
    using I18n::Text;
    const auto& entry = view.entry;
    const float left = x + K::Left;
    painter.Text(view.title, left, y + 26, 28, Palette::Text, K::BoxWidth, 44, true);
    if (!view.hint.empty()) painter.Text(view.hint, left, y + 70, 16, 0x91a3b8, K::BoxWidth, 26);

    // The text box: value, caret after it, and how much room is left. A text wider than the box
    // shows its end, where the caret is.
    painter.Round(left, y + 114, K::BoxWidth, 64, 14, 0x182331);
    painter.Round(left, y + 176, K::BoxWidth, 2, 1, Palette::Accent);
    const std::wstring shown = view.masked ? std::wstring(entry.Value().size(), L'•') : entry.Value();
    const float text_width = K::BoxWidth - 172;
    const float measured = painter.MeasureText(shown, 28, true);
    const bool overflow = measured > text_width;
    painter.Text(shown, left + 22, y + 124, 28, Palette::Text, text_width, 44, true, false, false, overflow);
    const float caret = left + 22 + std::min(measured, text_width);
    painter.Round(caret + 2, y + 129, 2, 34, 1, Palette::Accent);
    painter.Text(std::to_wstring(entry.Value().size()) + L" / " + std::to_wstring(entry.MaxLength()),
                 left + K::BoxWidth - 142, y + 132, 18, 0x91a3b8, 120, 30, false, false, false, true);
    if (!view.notice.empty())
        painter.Text(view.notice, left, y + 192, 16, Palette::Warning, K::BoxWidth, 48, false, true);

    for (std::size_t row = 0; row < TextEntry::CharacterRows; ++row) {
        for (std::size_t column = 0; column < TextEntry::Columns; ++column) {
            const bool active = entry.Row() == row && entry.Column() == column;
            const float key_x = x + K::KeyX(column), key_y = y + K::KeyY(row);
            painter.Round(key_x, key_y, K::KeyWidth, K::KeyHeight, 10, active ? Palette::Accent : 0x1b2736);
            painter.Text(std::wstring(1, entry.Character(row, column)), key_x, key_y, 22,
                         active ? Palette::OnAccent : 0xe3eaf2, K::KeyWidth, K::KeyHeight, true, false, true);
        }
    }
    for (std::size_t key = 0; key < TextEntry::ActionCount; ++key) {
        const auto action = TextEntry::ActionAt(key * 2);
        const bool active = entry.Row() == TextEntry::CharacterRows && entry.Column() / 2 == key;
        const bool latched = (action == TextEntry::Action::Shift && entry.Shifted()) ||
                             (action == TextEntry::Action::Symbols && entry.ShowingSymbols());
        const float key_x = x + K::KeyX(key * 2), key_y = y + K::KeyY(TextEntry::CharacterRows);
        const float width = K::KeyWidth * 2 + K::Gap;
        const unsigned idle = action == TextEntry::Action::Done ? 0x20342f : latched ? 0x304c46 : 0x263445;
        painter.Round(key_x, key_y, width, K::KeyHeight, 10, active ? Palette::Accent : idle);
        Text label = Text::DoneKey;
        switch (action) {
        case TextEntry::Action::Shift:   label = Text::ShiftKey; break;
        case TextEntry::Action::Symbols: label = entry.ShowingSymbols() ? Text::LettersKey : Text::SymbolsKey; break;
        case TextEntry::Action::Space:   label = Text::SpaceKey; break;
        case TextEntry::Action::Delete:  label = Text::DeleteKey; break;
        case TextEntry::Action::Done:    label = Text::DoneKey; break;
        }
        painter.Text(I18n::Tr(label), key_x, key_y, 18, active ? Palette::OnAccent : latched ? 0x95ebca : 0xe3eaf2,
                     width, K::KeyHeight, true, false, true);
    }
}

} // namespace EdenXbox
