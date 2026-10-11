// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
// Build with src/eden_uwp/mods/{mod_installer,mod_archive,mod_layout,mod_manifest}.cpp, libarchive's
// reader (externals/libarchive) and nlohmann_json's include folder.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include "eden_uwp/mods/mod_installer.h"
#include "stored_zip.h"

namespace {
using namespace EdenXbox::Mods;
namespace fs = std::filesystem;

constexpr std::uint64_t Title = 0x0100AAAABBBB0000;

void Check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        std::fflush(stdout);
        std::abort();
    }
}
} // namespace

int main() {
    const auto root = fs::temp_directory_path() / "eden-mod-installer";
    fs::remove_all(root);
    const ModPaths paths{root / "load", root / "mods"};
    const auto title = paths.load / "0100AAAABBBB0000";
    Check(TitleFolder(Title) == "0100AAAABBBB0000", "title folder");

    // A hand-copied mod is adopted; leftovers of a cut install are cleared.
    // Folders another page owns (the online cheats') are neither listed nor taken.
    fs::create_directories(title / "Manual" / "romfs");
    fs::create_directories(title / "Online cheats" / "cheats");
    fs::create_directories(paths.data / "staging" / "0100AAAABBBB0000" / "cut");
    ModLibrary library{paths, Title, {"Online cheats"}};
    auto mods = library.Load(1);
    Check(mods.size() == 1 && mods[0].folder == "Manual" && mods[0].source.kind == SourceKind::Local, "adopt");
    Check(!fs::exists(paths.data / "staging" / "0100AAAABBBB0000"), "staging cleared");
    Check(fs::exists(paths.data / "0100AAAABBBB0000.json"), "manifest saved");

    // Two variants of one archive become two mods, newest first, in their own folders.
    auto volumes = TestArchives::Volumes(TestArchives::StoredZip({
        {"30fps/romfs/a.bin", "aaa"}, {"60fps/romfs/a.bin", "bbbbbb"}, {"Manual/romfs/x", "c"}}));
    const auto listing = ListArchive(volumes);
    Check(listing.error == ArchiveError::None, "list");
    const auto plan = PlanLayout(listing.items, TitleFolder(Title), "fps.zip");
    Check(plan.candidates.size() == 3, "three candidates");
    ModSource source{SourceKind::Archive, "fps.zip"};
    std::size_t done = 0;
    std::string detail;
    std::uint64_t last_progress = 0;
    const auto error = library.Install(volumes, plan.candidates, source, mods, 2, {},
                                       [&](std::uint64_t d, std::uint64_t) { last_progress = d; }, done, detail);
    Check(error == ModError::None && done == 3, "install");
    Check(last_progress == 10, "progress reaches the total");
    Check(mods.size() == 4 && mods[0].folder == "Manual (2)" && mods[1].folder == "60fps" &&
              mods[2].folder == "30fps" && mods[3].folder == "Manual",
          "order and unique names");
    Check(fs::file_size(title / "60fps" / "romfs" / "a.bin") == 6, "files in place");
    Check(mods[1].source.archive == "fps.zip" && mods[1].installed == 2, "record");

    // The manifest on disk matches, and reloading keeps it.
    ModLibrary again{paths, Title, {"Online cheats"}};
    Check(again.Load(3) == mods, "reload");

    // Uninstall: folder and record go.
    Check(library.Remove(mods, "30fps") == ModError::None, "remove");
    Check(!fs::exists(title / "30fps") && mods.size() == 3, "removed");
    Check(library.Remove(mods, "nothing") == ModError::None && mods.size() == 3, "remove unknown");

    // An update takes the old mod's place and state, and the old one goes. Here "Manual (2)" (on
    // top) replaces "Manual" (last, turned off).
    mods.back().enabled = false;
    Check(library.Replace(mods, "Manual", "Manual (2)") == ModError::None, "replace");
    Check(mods.size() == 2 && mods[0].folder == "60fps" && mods[1].folder == "Manual (2)" && !mods[1].enabled,
          "replacement in the old place, off like it");
    Check(!fs::exists(title / "Manual") && fs::exists(title / "Manual (2)"), "old version uninstalled");
    Check(library.Replace(mods, "missing", "60fps") == ModError::None && mods.size() == 2, "replace unknown");

    // A folder deleted by hand drops from the list on the next load.
    fs::remove_all(title / "Manual (2)");
    Check(library.Load(4).size() == 1, "vanished folder");

    fs::remove_all(root);
    std::puts("mod-installer PASS");
}
