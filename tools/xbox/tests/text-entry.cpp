// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdio>
#include <cstdlib>
#include "eden_uwp/text_entry.h"

int main() {
    using namespace EdenXbox;
    auto check = [](bool ok) { if (!ok) std::abort(); };

    // An empty name starts with a capital; Shift then releases itself.
    TextEntry entry{L"", 10};
    check(entry.Row() == 1 && entry.Column() == 0 && entry.Shifted());
    check(entry.Press() == TextEntry::Result::None && entry.Value() == L"Q" && !entry.Shifted());
    entry.Move(1, 0);
    entry.Press();
    check(entry.Value() == L"Qw");

    // Left wraps; down into the action row snaps to the key under the column, and back up.
    entry.Focus(2, 0);
    entry.Move(-1, 0);
    check(entry.Column() == 9 && entry.Character(2, 9) == L'ñ');
    entry.Move(0, 1);
    entry.Move(0, 1);
    check(entry.Row() == TextEntry::CharacterRows && entry.Column() == 8);
    check(TextEntry::ActionAt(entry.Column()) == TextEntry::Action::Done);
    entry.Move(1, 0);
    check(entry.Column() == 0 && TextEntry::ActionAt(entry.Column()) == TextEntry::Action::Shift);
    entry.Move(0, -1);
    check(entry.Row() == 3 && entry.Column() == 0);

    // Action keys.
    entry.Focus(4, 4); // Space
    entry.Press();
    entry.Press(); // A second space in a row is ignored.
    check(entry.Value() == L"Qw ");
    entry.Focus(4, 6); // Delete
    entry.Press();
    check(entry.Value() == L"Qw");
    entry.Focus(4, 2); // Symbols
    entry.Press();
    check(entry.ShowingSymbols() && entry.Character(0, 0) == L'á');
    entry.Focus(4, 0); // Shift
    entry.Press();
    check(entry.Character(0, 0) == L'Á' && entry.Character(1, 0) == L'!');
    entry.Focus(4, 8);
    check(entry.Press() == TextEntry::Result::Done);

    // The length limit, control characters and surrogate pairs.
    TextEntry limited{L"ABCDEFGHI", 10};
    limited.Type(L'\t');
    limited.Type(L'J');
    limited.Type(L'K');
    check(limited.Value() == L"ABCDEFGHIJ");
    TextEntry emoji{L"A\U0001F600", 10};
    emoji.Backspace();
    check(emoji.Value() == L"A");
    check(TextEntry{L"ABCDEFGHIJKL", 10}.Value() == L"ABCDEFGHIJ");

    // Commands, as every input source sends them.
    using Command = TextEntry::Command;
    TextEntry commands{L"Ab", 4};
    check(commands.Apply(Command::Space) == TextEntry::Result::None && commands.Value() == L"Ab ");
    check(commands.Apply(Command::Delete) == TextEntry::Result::None && commands.Value() == L"Ab");
    commands.Apply(Command::Shift);
    check(commands.Shifted());
    commands.Apply(Command::Down);
    check(commands.Row() == 2);
    check(commands.Apply(Command::Done) == TextEntry::Result::Done);
    check(commands.Apply(Command::Cancel) == TextEntry::Result::Cancel);
    commands.SetValue(L"ABCDEF");
    check(commands.Value() == L"ABCD");

    std::puts("text entry: typing, shift, symbols, navigation, actions and limits PASS");
}
