// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdio>
#include <cstdlib>
#include "eden_uwp/mods/archive_volumes.h"

namespace {
using namespace EdenXbox::Mods;

void Check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        std::fflush(stdout);
        std::abort();
    }
}
} // namespace

int main() {
    using V = std::vector<std::string>;
    std::vector<FolderFile> files;
    for (const char* name : {"Water.part10.rar", "Water.part2.rar", "Water.part1.rar", "game.nsp", "Old.r01",
                             "Old.rar", "Old.r00", "Big.7z.002", "Big.7z.001", "Single.ZIP", "Pack.7z",
                             "notes.txt", "x.001", "y.r99999999999", "a.zip.1234567"}) {
        files.push_back({name, std::string{"/"} + name});
    }
    const auto groups = GroupArchives(files);
    Check(groups.size() == 5, "five archives");
    Check(groups[0].name == "Big.7z" && groups[0].volumes == V{"/Big.7z.001", "/Big.7z.002"}, "7z split");
    Check(groups[1].name == "Old.rar" && groups[1].volumes == V{"/Old.rar", "/Old.r00", "/Old.r01"}, "old rar");
    Check(groups[2].name == "Pack.7z" && groups[2].volumes == V{"/Pack.7z"}, "single 7z");
    Check(groups[3].name == "Single.ZIP", "zip any case");
    Check(groups[4].name == "Water.rar" &&
              groups[4].volumes == V{"/Water.part1.rar", "/Water.part2.rar", "/Water.part10.rar"},
          "parts in numeric order");
    std::puts("archive-volumes PASS");
}
