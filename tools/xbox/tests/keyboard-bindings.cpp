// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdio>
#include <cstdlib>
#include "eden_uwp/keyboard_bindings.h"
int main() {
    using namespace EdenXbox;
    auto check = [](bool ok) { if (!ok) std::abort(); };
    auto bindings = DefaultKeyboardBindings;
    check(AssignKeyboardKey(bindings, 0, 'Q'));
    check(AssignKeyboardKey(bindings, 1, 'Q'));
    check(bindings[0] == 0 && bindings[1] == 'Q');
    check(AssignKeyboardKey(bindings, 27, 'T'));
    check(bindings[27] == 'T');
    auto before = bindings;
    check(!AssignKeyboardKey(bindings, 0, 27));
    check(!AssignKeyboardKey(bindings, 0, 195));
    check(!AssignKeyboardKey(bindings, KeyboardActionCount, 65));
    check(!AssignKeyboardKey(bindings, 0, 65535));
    check(bindings == before);
    check(AssignKeyboardKey(bindings, 1, 0) && bindings[1] == 0);
    for (size_t i = 0; i < KeyboardActionCount; ++i) {
        if (i == 16 || i == 17) continue;
        const auto tile = KeyboardTileFor(i);
        check(Contains(tile, tile.x + tile.w / 2, tile.y + tile.h / 2));
        check(!Contains(tile, tile.x + tile.w, tile.y + tile.h));
        check(tile.y + tile.h <= 573);
        for (size_t j = 0; j < KeyboardActionCount; ++j) {
            if (i == j || KeyboardPageFor(i) != KeyboardPageFor(j)) continue;
            if (j == 16 || j == 17) continue;
            const auto other = KeyboardTileFor(j);
            check(!Contains(other, tile.x + tile.w / 2, tile.y + tile.h / 2));
        }
    }
    std::puts("PASS keyboard conflicts, Q/T, clear, invalid keys, three pages and hit bounds");
}
