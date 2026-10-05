// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cassert>
#include <string>
#include "eden_uwp/storage_path.h"

int main() {
    using namespace EdenXbox;
    for (const auto* relative : {"", "game.nsp", "Games/Title.xci", "a/b/c.nro"}) {
        const auto parsed = ParseStoragePath(MakeStoragePath("eden-games-012345", relative));
        assert(parsed && parsed->token == "eden-games-012345" && parsed->relative == relative);
    }
    for (const auto* relative : {"/a.nsp", "a//b", "a/", "../a", "a/../b", "./a", "a/./b",
                                 "a\\b", "E:/a", "a:stream", "a/../../b"}) {
        assert(!ValidStorageRelative(relative));
        assert(!ParseStoragePath(MakeStoragePath("eden-games-1", relative)));
    }
    assert(!ValidStorageRelative(std::string{"a\0b", 3}));
    for (const auto* path : {"eden-usb:/bad/game.nsp", "eden-usb:/eden-games-1?/a", "E:/a.nsp",
                             "eden-usb:/eden-games-1_2/a", "eden-usb://eden-games-1/a"})
        assert(!ParseStoragePath(path));
    const auto a = MakeStoragePath("eden-games-1", "same.nsp");
    const auto b = MakeStoragePath("eden-games-2", "same.nsp");
    assert(a != b);
    assert(!ParseStoragePath(MakeStoragePath(std::string(101, 'a'))));
}
