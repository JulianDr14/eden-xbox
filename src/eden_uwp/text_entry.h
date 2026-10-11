// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// On-screen keyboard for short texts, driven by a gamepad, a mouse or a real keyboard: the user
// name in the library and every text a game asks for (uwp_software_keyboard.h). The frontend
// draws on its own swapchain without XAML, so there is no text box for the system keyboard to
// attach to; this keeps text entry identical on the Series and the PC. The model knows nothing of
// where it is drawn (text_entry_panel.h) or which button does what (Command).
//
// Five rows of ten columns. The first four hold characters; the last holds five wide action keys
// (Shift, symbols, Space, Delete, Done), two columns each, so moving up and down keeps the column.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace EdenXbox {

class TextEntry {
public:
    static constexpr std::size_t Columns = 10;
    static constexpr std::size_t CharacterRows = 4;
    static constexpr std::size_t Rows = CharacterRows + 1;
    enum class Action : std::uint8_t { Shift, Symbols, Space, Delete, Done };
    static constexpr std::size_t ActionCount = 5;
    enum class Result : std::uint8_t { None, Done, Cancel };
    /// What every input source maps to, so a pad, a real keyboard and a pointer act alike.
    enum class Command : std::uint8_t { Up, Down, Left, Right, Press, Delete, Space, Shift, Done, Cancel };

    TextEntry() = default;
    TextEntry(std::wstring initial, std::size_t max_length_)
        : value{std::move(initial)}, max_length{max_length_} {
        if (value.size() > max_length) value.resize(max_length);
        shift = value.empty(); // A new name starts with a capital.
    }

    void Move(int dx, int dy) {
        if (dy != 0) {
            row = (row + Rows + (dy > 0 ? 1 : Rows - 1)) % Rows;
            // Entering the action row lands on the key under the current column.
            if (row == CharacterRows) column = column / 2 * 2;
        }
        if (dx != 0) {
            const std::size_t step = row == CharacterRows ? 2 : 1;
            column = (column + Columns + (dx > 0 ? step : Columns - step)) % Columns;
        }
    }
    void Focus(std::size_t row_, std::size_t column_) {
        row = row_ % Rows;
        column = row == CharacterRows ? column_ % Columns / 2 * 2 : column_ % Columns;
    }

    /// The highlighted key, as A on a pad or Enter would press it.
    Result Press() {
        if (row < CharacterRows) {
            Type(Character(row, column));
            return Result::None;
        }
        switch (ActionAt(column)) {
        case Action::Shift:   shift = !shift; break;
        case Action::Symbols: symbols = !symbols; break;
        case Action::Space:   Type(L' '); break;
        case Action::Delete:  Backspace(); break;
        case Action::Done:    return Result::Done;
        }
        return Result::None;
    }

    Result Apply(Command command) {
        switch (command) {
        case Command::Up:     Move(0, -1); break;
        case Command::Down:   Move(0, 1); break;
        case Command::Left:   Move(-1, 0); break;
        case Command::Right:  Move(1, 0); break;
        case Command::Press:  return Press();
        case Command::Delete: Backspace(); break;
        case Command::Space:  Type(L' '); break;
        case Command::Shift:  ToggleShift(); break;
        case Command::Done:   return Result::Done;
        case Command::Cancel: return Result::Cancel;
        }
        return Result::None;
    }

    /// Replaces the text (a game changed it), keeping the length limit.
    void SetValue(std::wstring text) {
        value = std::move(text);
        if (value.size() <= max_length) return;
        value.resize(max_length);
        if (!value.empty() && value.back() >= 0xD800 && value.back() <= 0xDBFF) value.pop_back();
    }

    /// Typed on a real keyboard or produced by a key: printable characters only.
    void Type(wchar_t c) {
        if (c < 0x20 || c == 0x7F || value.size() >= max_length) return;
        if (c == L' ' && (value.empty() || value.back() == L' ')) return;
        value += c;
        // Like phone keyboards: Shift applies to one letter, then turns itself off.
        if (shift && !symbols) shift = false;
    }
    void Backspace() {
        if (value.empty()) return;
        value.pop_back();
        if (!value.empty() && value.back() >= 0xD800 && value.back() <= 0xDBFF) value.pop_back();
    }
    void ToggleShift() { shift = !shift; }

    [[nodiscard]] wchar_t Character(std::size_t r, std::size_t c) const {
        static constexpr std::array<std::wstring_view, CharacterRows> letters{
            L"1234567890", L"qwertyuiop", L"asdfghjklñ", L"zxcvbnm-_."};
        static constexpr std::array<std::wstring_view, CharacterRows> symbol_rows{
            L"áéíóúüçàèò", L"!?¡¿@#$%&*", L"()[]{}<>/\\", L"+=:;'\",~^|"};
        if (r >= CharacterRows || c >= Columns) return 0;
        const wchar_t ch = (symbols ? symbol_rows : letters)[r][c];
        if (!shift) return ch;
        if (ch >= L'a' && ch <= L'z') return static_cast<wchar_t>(ch - L'a' + L'A');
        switch (ch) {
        case L'ñ': return L'Ñ';
        case L'á': return L'Á';
        case L'é': return L'É';
        case L'í': return L'Í';
        case L'ó': return L'Ó';
        case L'ú': return L'Ú';
        case L'ü': return L'Ü';
        case L'ç': return L'Ç';
        case L'à': return L'À';
        case L'è': return L'È';
        case L'ò': return L'Ò';
        default:   return ch;
        }
    }
    static constexpr Action ActionAt(std::size_t c) {
        return static_cast<Action>(c / 2 % ActionCount);
    }

    [[nodiscard]] const std::wstring& Value() const { return value; }
    [[nodiscard]] std::size_t MaxLength() const { return max_length; }
    [[nodiscard]] std::size_t Row() const { return row; }
    [[nodiscard]] std::size_t Column() const { return column; }
    [[nodiscard]] bool Shifted() const { return shift; }
    [[nodiscard]] bool ShowingSymbols() const { return symbols; }

private:
    std::wstring value;
    std::size_t max_length{10};
    std::size_t row{1}, column{0};
    bool shift{}, symbols{};
};

} // namespace EdenXbox
