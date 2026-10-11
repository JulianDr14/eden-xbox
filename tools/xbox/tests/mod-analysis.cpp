// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
// Build with src/eden_uwp/mods/mod_inspect.cpp.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include "eden_uwp/mods/mod_analysis.h"
#include "eden_uwp/mods/mod_inspect.h"

namespace {
using namespace EdenXbox::Mods;
namespace fs = std::filesystem;

void Check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        std::fflush(stdout);
        std::abort();
    }
}

void Write(const fs::path& path, std::string_view text) {
    fs::create_directories(path.parent_path());
    std::ofstream{path, std::ios::binary} << text;
}
} // namespace

int main() {
    Check(ShortBuildId("0123456789abcdef0011") == "0123456789ABCDEF", "long id");
    Check(ShortBuildId("ABC") == "ABC0000000000000", "padded id");
    Check(ShortBuildId("main").empty(), "not hex");
    Check(PchtxtBuildId("@flag x\n@nsobid-a1b2c3d4e5f60718293a\n") == "A1B2C3D4E5F60718", "pchtxt");
    Check(PchtxtBuildId("@enabled").empty(), "pchtxt without id");

    const auto root = fs::temp_directory_path() / "eden-mod-analysis";
    fs::remove_all(root);
    const auto a = root / "A";
    Write(a / "RomFS" / "Model" / "Sea.bfres", "12345");
    Write(a / "romfs_ext" / "Data" / "old.pack.stub", "");
    Write(a / "exefs" / "main", "xx");
    Write(a / "exefs" / "0123456789ABCDEF00.ips", "PATCH");
    Write(a / "exefs" / "fps.pchtxt", "@nsobid-FEDCBA9876543210\n");
    Write(a / "cheats" / "1111222233334444.txt", "[x]");
    Write(a / "cheats" / "toggles.txt", "");
    const auto contents = InspectFolder(a);
    Check(contents.parts == (RomFs | RomFsExt | ExeFs | Patch | Cheats), "parts");
    Check(contents.files == std::vector<std::string>{"exefs/main", "romfs/data/old.pack", "romfs/model/sea.bfres"},
          "files");
    Check(contents.build_ids.size() == 3, "build ids");
    Check(contents.bytes == 5 + 2 + 5 + 25 + 3, "bytes");
    Check(InspectFolder(root / "missing").files.empty(), "missing folder");
    Check(contents.listing.size() == 7 && contents.listing.front().path == "RomFS/Model/Sea.bfres" &&
              contents.listing.front().bytes == 5,
          "every file, as written");
    // The largest files the game gets, by name; cheats and readmes are not among them.
    Check(MainFiles(contents.listing, 2) == std::vector<std::string>{"fps.pchtxt", "Sea.bfres"},
          "main files");
    Check(MainFiles({{"readme.txt", 99}, {"romfs/a/b.szs", 1}}, 3) == std::vector<std::string>{"b.szs"},
          "a readme is not a main file");

    Check(CheckCompat(contents, "FEDCBA9876543210") == Compat::Matches, "matches");
    Check(CheckCompat(contents, "0000000000000000") == Compat::OtherVersion, "other version");
    Check(CheckCompat(contents, "") == Compat::Any, "unknown game");
    Check(CheckCompat(ModContents{RomFs}, "0000000000000000") == Compat::Any, "files only");

    const auto b = root / "B";
    Write(b / "romfs" / "model" / "sea.bfres", "1");
    Write(b / "romfs" / "data" / "old.pack", "1");
    Write(b / "romfs" / "other", "1");
    const auto c = root / "C";
    Write(c / "romfs" / "model" / "sea.bfres", "1");
    const std::vector<ModContents> all{contents, InspectFolder(b), InspectFolder(c)};
    auto conflicts = FindConflicts(all, {true, true, true});
    Check(conflicts[0].size() == 2 && conflicts[0][0].other == 1 && conflicts[0][0].files == 2 &&
              conflicts[0][0].wins,
          "A beats B");
    Check(conflicts[1].size() == 2 && !conflicts[1][0].wins && conflicts[1][1].wins, "B between");
    Check(conflicts[2].size() == 2 && !conflicts[2][0].wins && !conflicts[2][1].wins, "C loses");
    conflicts = FindConflicts(all, {true, false, true});
    Check(conflicts[1].empty() && conflicts[0].size() == 1 && conflicts[0][0].other == 2, "off mods");

    fs::remove_all(root);
    std::puts("mod-analysis PASS");
}
