// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cassert>
#include <string>
#include "jadyx/storage_path.h"

int main() {
    using namespace Jadyx;
    for (const auto* token : {"jadyx-games-012345", "jadyx-content-012345"}) {
        for (const auto* relative : {"", "game.nsp", "Games/Title.xci", "a/b/c.nro"}) {
            const auto parsed = ParseStoragePath(MakeStoragePath(token, relative));
            assert(parsed && parsed->token == token && parsed->relative == relative);
        }
    }
    assert(MakeStoragePath("jadyx-games-1", "a.nsp") == "jadyx-usb:/jadyx-games-1/a.nsp");
    assert(IsGameToken("jadyx-games-1") && !IsGameToken("jadyx-content-1") && !IsGameToken("eden-games-1"));
    for (const auto* relative : {"/a.nsp", "a//b", "a/", "../a", "a/../b", "./a", "a/./b",
                                 "a\\b", "E:/a", "a:stream", "a/../../b"}) {
        assert(!ValidStorageRelative(relative));
        assert(!ParseStoragePath(MakeStoragePath("jadyx-games-1", relative)));
    }
    assert(!ValidStorageRelative(std::string{"a\0b", 3}));
    for (const auto* path : {"jadyx-usb:/bad/game.nsp", "jadyx-usb:/jadyx-games-1?/a", "E:/a.nsp",
                             "jadyx-usb:/jadyx-games-1_2/a", "jadyx-usb://jadyx-games-1/a",
                             "eden-usb:/eden-games-1/a"})
        assert(!ParseStoragePath(path));
    const auto a = MakeStoragePath("jadyx-games-1", "same.nsp");
    const auto b = MakeStoragePath("jadyx-games-2", "same.nsp");
    assert(a != b);
    assert(!ParseStoragePath(MakeStoragePath(std::string(101, 'a'))));
}
