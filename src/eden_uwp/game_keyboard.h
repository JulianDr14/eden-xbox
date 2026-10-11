// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The keyboard a game opens to ask for text (a player name, a search...), as the play loop runs
// it: what the game asked for comes in as KeyboardEvents (uwp_software_keyboard.h turns the
// emulated applet's calls into them), presses come in as TextEntry commands, and what goes back
// to the game comes out as KeyboardReplies. Free of the emulator core so it is tested on its own.
//
// Two kinds, like the console's:
// - normal: a full-screen keyboard; Done submits the text once (the game may check it and send it
//   back with a message, KeyboardTextCheck), B cancels unless the game forbids it;
// - inline: the game draws its own text box and hears every change as it is typed.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>

#include "eden_uwp/localization.h"
#include "eden_uwp/text_entry.h"
#include "eden_uwp/text_entry_panel.h"

namespace EdenXbox {

/// The game opened the keyboard.
struct KeyboardOpen {
    std::wstring title;   ///< The game's header (or guide) text; empty uses a generic title.
    std::wstring hint;    ///< The game's sub text.
    std::wstring initial; ///< The text the keyboard starts with (often the player's name).
    std::size_t max_length{};
    std::size_t min_length{};
    bool masked{};
    bool can_cancel{true};
    bool is_inline{};
};
/// The game checked a submitted text: refused it (`confirm` false) or asks to confirm it.
struct KeyboardTextCheck {
    std::wstring message;
    bool confirm{};
};
/// The game changed an inline keyboard's text.
struct KeyboardInlineText {
    std::wstring text;
};
/// The game closed the keyboard.
struct KeyboardClose {};
using KeyboardEvent = std::variant<KeyboardOpen, KeyboardTextCheck, KeyboardInlineText, KeyboardClose>;

struct KeyboardReply {
    enum class Kind : std::uint8_t {
        Changed, ///< Inline only: the text changed.
        Submit,  ///< Done with this text.
        Cancel,  ///< Left without a text.
    };
    Kind kind{};
    std::wstring text;
    bool is_inline{};
    bool confirmed{}; ///< Normal only: the game already asked to confirm this text.
};

class GameKeyboard {
public:
    /// Longest text when the game sets no limit (the console's own limit).
    static constexpr std::size_t DefaultMaxLength = 500;

    [[nodiscard]] bool IsOpen() const { return entry.has_value(); }

    /// Applies what the game asked for; true when the keyboard must be drawn again (or hidden).
    bool Apply(const KeyboardEvent& event) {
        return std::visit([this](const auto& e) { return On(e); }, event);
    }

    /// A press from any input source; a reply when the game must hear about it.
    std::optional<KeyboardReply> Handle(TextEntry::Command command) {
        if (!entry) return std::nullopt;
        notice.clear();
        const std::wstring before = entry->Value();
        switch (entry->Apply(command)) {
        case TextEntry::Result::Done: return Finish();
        case TextEntry::Result::Cancel: return Cancel();
        case TextEntry::Result::None: break;
        }
        return Changed(before);
    }
    /// A key under a pointer, pressed.
    std::optional<KeyboardReply> PressKey(std::size_t row, std::size_t column) {
        if (!entry) return std::nullopt;
        entry->Focus(row, column);
        return Handle(TextEntry::Command::Press);
    }
    /// A character typed on a real keyboard.
    std::optional<KeyboardReply> Type(wchar_t c) {
        if (!entry) return std::nullopt;
        notice.clear();
        const std::wstring before = entry->Value();
        entry->Type(c);
        return Changed(before);
    }

    /// What the panel shows; only while open.
    [[nodiscard]] TextEntryPanelView View() const {
        return {*entry, open.title.empty() ? I18n::Tr(I18n::Text::TextInputTitle) : std::wstring_view{open.title},
                open.hint, notice, open.masked};
    }
    [[nodiscard]] const TextEntry* Entry() const { return entry ? &*entry : nullptr; }
    [[nodiscard]] bool CanCancel() const { return open.can_cancel; }

private:
    bool On(const KeyboardOpen& event) {
        if (entry && open.is_inline && event.is_inline) {
            // Shown again while open (a game repeats it with its requests): keep what was typed.
            open = event;
            if (open.max_length == 0) open.max_length = DefaultMaxLength;
            return false;
        }
        open = event;
        if (open.max_length == 0) open.max_length = DefaultMaxLength;
        if (open.min_length > open.max_length) open.min_length = 0;
        entry.emplace(open.initial, open.max_length);
        notice.clear();
        confirming = false;
        return true;
    }
    bool On(const KeyboardTextCheck& event) {
        // Back to the keyboard with the text the game checked and its message.
        entry.emplace(submitted, open.max_length);
        notice = event.message;
        confirming = event.confirm;
        if (confirming) {
            if (!notice.empty()) notice += L'\n';
            notice += I18n::Tr(I18n::Text::TextInputConfirmAgain);
        }
        return true;
    }
    bool On(const KeyboardInlineText& event) {
        open.initial = event.text;
        if (!entry || entry->Value() == event.text) return false;
        entry->SetValue(event.text);
        return true;
    }
    bool On(const KeyboardClose&) {
        if (!entry) return false;
        entry.reset();
        return true;
    }

    std::optional<KeyboardReply> Finish() {
        if (entry->Value().size() < open.min_length) {
            notice = I18n::Format(I18n::Text::TextInputTooShort, {std::to_wstring(open.min_length)});
            return std::nullopt;
        }
        KeyboardReply reply{KeyboardReply::Kind::Submit, entry->Value(), open.is_inline, confirming};
        submitted = entry->Value();
        entry.reset();
        return reply;
    }
    std::optional<KeyboardReply> Cancel() {
        if (!open.can_cancel) return std::nullopt;
        entry.reset();
        return KeyboardReply{KeyboardReply::Kind::Cancel, {}, open.is_inline, false};
    }
    std::optional<KeyboardReply> Changed(const std::wstring& before) {
        if (!open.is_inline || entry->Value() == before) return std::nullopt;
        return KeyboardReply{KeyboardReply::Kind::Changed, entry->Value(), true, false};
    }

    KeyboardOpen open;
    std::optional<TextEntry> entry;
    std::wstring notice;
    std::wstring submitted;
    bool confirming{};
};

} // namespace EdenXbox
