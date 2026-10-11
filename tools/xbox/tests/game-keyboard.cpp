// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
// Build with src/eden_uwp/localization.cpp and src/eden_uwp/locales/*.cpp.
#include <cstdio>
#include <cstdlib>
#include "eden_uwp/game_keyboard.h"

int main() {
    using namespace EdenXbox;
    using Command = TextEntry::Command;
    using Kind = KeyboardReply::Kind;
    auto check = [](bool ok) { if (!ok) std::abort(); };

    // A normal keyboard starts with the game's text and submits it once, then closes.
    GameKeyboard keyboard;
    check(!keyboard.IsOpen() && !keyboard.Handle(Command::Press));
    check(keyboard.Apply(KeyboardOpen{.title = L"Name", .initial = L"Ash", .max_length = 10}));
    check(keyboard.IsOpen() && keyboard.View().title == L"Name" && keyboard.Entry()->Value() == L"Ash");
    check(!keyboard.Type(L'!')); // Normal keyboards tell the game nothing until Done.
    auto reply = keyboard.Handle(Command::Done);
    check(reply && reply->kind == Kind::Submit && reply->text == L"Ash!" && !reply->is_inline && !reply->confirmed);
    check(!keyboard.IsOpen());

    // The game refuses the text: back to the keyboard with it and the game's message.
    check(keyboard.Apply(KeyboardTextCheck{L"Not allowed", false}));
    check(keyboard.IsOpen() && keyboard.Entry()->Value() == L"Ash!" && keyboard.View().notice == L"Not allowed");
    keyboard.Handle(Command::Delete);
    check(keyboard.View().notice.empty()); // A press clears the notice.
    // It asks to confirm: the next Done says so.
    check(keyboard.Handle(Command::Done)->text == L"Ash");
    check(keyboard.Apply(KeyboardTextCheck{L"Sure?", true}));
    check(keyboard.View().notice.starts_with(L"Sure?\n"));
    reply = keyboard.Handle(Command::Done);
    check(reply && reply->confirmed);

    // Minimum length, a generic title and a game that forbids cancelling.
    keyboard.Apply(KeyboardOpen{.min_length = 3, .can_cancel = false});
    check(!keyboard.View().title.empty() && keyboard.Entry()->MaxLength() == GameKeyboard::DefaultMaxLength);
    keyboard.Type(L'a');
    check(!keyboard.Handle(Command::Done) && !keyboard.View().notice.empty() && keyboard.IsOpen());
    check(!keyboard.Handle(Command::Cancel) && keyboard.IsOpen());
    check(keyboard.Apply(KeyboardClose{}) && !keyboard.IsOpen());

    // Cancelling a normal keyboard.
    keyboard.Apply(KeyboardOpen{.initial = L"x"});
    reply = keyboard.Handle(Command::Cancel);
    check(reply && reply->kind == Kind::Cancel && !keyboard.IsOpen());

    // Inline: every change is heard; the game's own changes update the box without a reply.
    keyboard.Apply(KeyboardOpen{.max_length = 8, .is_inline = true});
    reply = keyboard.Type(L'h');
    check(reply && reply->kind == Kind::Changed && reply->text == L"h" && reply->is_inline);
    check(!keyboard.Handle(Command::Up)); // Moving changes no text.
    check(keyboard.Apply(KeyboardInlineText{L"hey"}) && keyboard.Entry()->Value() == L"hey");
    check(!keyboard.Apply(KeyboardInlineText{L"hey"}));
    // Shown again while open: what was typed stays.
    check(!keyboard.Apply(KeyboardOpen{.max_length = 8, .is_inline = true}) && keyboard.Entry()->Value() == L"hey");
    reply = keyboard.PressKey(4, 8); // The Done key.
    check(reply && reply->kind == Kind::Submit && reply->text == L"hey" && reply->is_inline && !keyboard.IsOpen());

    // Masked text.
    keyboard.Apply(KeyboardOpen{.initial = L"pw", .masked = true});
    check(keyboard.View().masked);

    std::puts("game keyboard: normal, text check, confirm, limits, cancel and inline PASS");
}
