// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
// Build with src/eden_uwp/mods/mod_layout.cpp.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include "eden_uwp/mods/mod_layout.h"
#include "eden_uwp/mods/mod_model.h"

namespace {
using namespace EdenXbox::Mods;

constexpr const char* Game = "0100AAAABBBB0000";

void Check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        std::fflush(stdout);
        std::abort();
    }
}

LayoutPlan Plan(std::vector<std::string> paths, const char* archive = "Pack.zip") {
    std::vector<ArchiveItem> items;
    for (auto& path : paths) items.push_back({std::move(path), 10});
    return PlanLayout(items, Game, archive);
}

std::vector<std::string> Targets(const ModCandidate& candidate) {
    std::vector<std::string> out;
    for (const auto& file : candidate.files) out.push_back(file.to);
    return out;
}
} // namespace

int main() {
    using V = std::vector<std::string>;
    {
        // A named mod folder, with a readme beside its parts that is left out.
        const auto plan = Plan({"Better Water/romfs/Model/sea.bfres", "Better Water/exefs/main",
                                "Better Water/readme.txt"});
        Check(plan.candidates.size() == 1, "one mod");
        const auto& mod = plan.candidates[0];
        Check(mod.name == "Better Water", "folder name");
        Check(Targets(mod) == V{"exefs/main", "romfs/Model/sea.bfres"}, "files");
        Check(mod.parts == (RomFs | ExeFs) && mod.bytes == 20, "parts and size");
    }
    {
        // Atmosphere layout: the contents and exefs_patches halves are one mod, named after the
        // archive; the parts' folders are matched without case.
        const auto plan = Plan({"atmosphere/contents/0100AAAABBBB0000/RomFS/a.bin",
                                "atmosphere/exefs_patches/fps/0123456789ABCDEF0000.ips"},
                               "60 FPS.7z");
        Check(plan.candidates.size() == 1, "atmosphere is one mod");
        Check(plan.candidates[0].name == "60 FPS", "archive name");
        Check(Targets(plan.candidates[0]) == V{"exefs/0123456789ABCDEF0000.ips", "romfs/a.bin"},
              "atmosphere files");
        Check(plan.candidates[0].parts == (RomFs | Patch), "patch part");
        Check(!plan.candidates[0].other_title, "own title");
    }
    {
        // Variants side by side, each with the Atmosphere layout inside.
        const auto plan = Plan({"30fps/atmosphere/contents/0100AAAABBBB0000/exefs/x.pchtxt",
                                "60fps/atmosphere/contents/0100AAAABBBB0000/exefs/x.pchtxt"});
        Check(plan.candidates.size() == 2, "two variants");
        Check(plan.candidates[0].name == "30fps" && plan.candidates[1].name == "60fps", "variant names");
    }
    {
        // Another title's folder is still a mod, but flagged.
        const auto plan = Plan({"atmosphere/contents/0100AAAABBBB0800/romfs/a"});
        Check(plan.candidates.size() == 1 && plan.candidates[0].other_title, "other title");
    }
    {
        // Translation packs: <title id>/romfs/..., the game's own or another's.
        Check(!Plan({"0100AAAABBBB0000/romfs/a"}).candidates[0].other_title, "own title folder");
        Check(Plan({"0100AAAABBBB0800/romfs/a"}).candidates[0].other_title, "other title folder");
    }
    {
        // exefs_patches on its own: the patch set's folder names it.
        const auto plan = Plan({"exefs_patches/Uncapped/ABCDEF.ips"});
        Check(plan.candidates.size() == 1 && plan.candidates[0].name == "Uncapped", "exefs_patches name");
        Check(Targets(plan.candidates[0]) == V{"exefs/ABCDEF.ips"}, "exefs_patches target");
    }
    {
        // No mod folders: loose patches and cheats, one mod per folder they are in.
        const auto plan = Plan({"v1.2/aa.pchtxt", "v1.3/bb.pchtxt", "0123456789ABCDEF.txt", "notes.txt"});
        Check(plan.candidates.size() == 3, "loose groups");
        Check(plan.candidates[0].name == "Pack" && Targets(plan.candidates[0]) == V{"cheats/0123456789ABCDEF.txt"},
              "loose cheats");
        Check(plan.candidates[1].name == "v1.2" && Targets(plan.candidates[1]) == V{"exefs/aa.pchtxt"},
              "loose patch");
    }
    {
        // Unsafe paths never reach a plan, and the same target twice keeps the first.
        const auto plan = Plan({"m/romfs/../../evil", "/m/romfs/abs", "m/romfs/C:x", "m/romfs/a",
                                "m/ROMFS/a"});
        Check(plan.candidates.size() == 1 && Targets(plan.candidates[0]) == V{"romfs/a"}, "unsafe");
        Check(Plan({"readme.md", "images/shot.png"}).candidates.empty(), "not a mod");
    }
    {
        // The game's own folders with no romfs/ around them (a real store archive): all of it is
        // romfs but the readme beside them, and the archive names the mod.
        const V bare{"Audio/Chara/Chara_X.bars", "Chara/X.szs", "Vehicle/Body/Body_X.szs",
                     "readme.txt", "UI/cmn/tc_Chara_X.png"};
        const V romfs{"romfs/Audio/Chara/Chara_X.bars", "romfs/Chara/X.szs", "romfs/UI/cmn/tc_Chara_X.png",
                      "romfs/Vehicle/Body/Body_X.szs"};
        const std::vector<std::string> top{"audio", "chara", "vehicle", "ui", "stage"};
        const auto plan_with = [&](V paths) {
            std::vector<ArchiveItem> items;
            for (auto& path : paths) items.push_back({std::move(path), 10});
            return PlanLayout(items, Game, "skin_343b2.zip", top);
        };
        for (const auto& plan : {plan_with(bare), Plan(bare, "skin_343b2.zip")}) {
            Check(plan.candidates.size() == 1 && Targets(plan.candidates[0]) == romfs &&
                      plan.candidates[0].parts == RomFs && plan.candidates[0].name == "skin_343b2",
                  "bare romfs");
        }
        // A wrapping folder, found by the game's folders or by the archive's shape.
        V wrapped;
        for (const auto& path : bare) wrapped.push_back("Skin v2/" + path);
        for (const auto& plan : {plan_with(wrapped), Plan(wrapped)})
            Check(plan.candidates.size() == 1 && Targets(plan.candidates[0]) == romfs &&
                      plan.candidates[0].name == "Skin v2",
                  "wrapped bare romfs");
        // A lone game folder is no wrapper; a deep one is found with the game's folders.
        Check(Targets(Plan({"Chara/X.szs"}).candidates.at(0)) == V{"romfs/Chara/X.szs"}, "lone folder");
        const auto deep = plan_with({"Skin/UI/cmn/a.png", "Skin/UI/cmn/b.bflim"});
        Check(deep.candidates.size() == 1 && Targets(deep.candidates[0]) == V{"romfs/UI/cmn/a.png", "romfs/UI/cmn/b.bflim"},
              "deep folder");
        // Variants: folders side by side holding the same game folders.
        const V variants{"Red/Chara/X.szs", "Red/UI/x.bflim", "Blue/Chara/X.szs", "Blue/UI/x.bflim"};
        for (const auto& plan : {plan_with(variants), Plan(variants)}) {
            Check(plan.candidates.size() == 2 && plan.candidates[0].name == "Blue" &&
                      Targets(plan.candidates[1]) == V{"romfs/Chara/X.szs", "romfs/UI/x.bflim"},
                  "bare variants");
        }
        // Known folders and nothing of the game's: no mod for this game.
        Check(plan_with({"Other/thing.bin"}).candidates.empty(), "not this game's");
    }
    {
        Check(SafeRelativePath("a/b.txt"), "safe");
        Check(!SafeRelativePath("a//b") && !SafeRelativePath("./a") && !SafeRelativePath("a\\b") &&
                  !SafeRelativePath(""),
              "unsafe forms");
    }
    {
        Check(FolderName("Better Water", {}) == "Better Water", "plain");
        Check(FolderName("a<b>:c?", {}) == "a_b__c_", "refused characters");
        Check(FolderName("  Mod. . ", {}) == "Mod", "trimmed");
        Check(FolderName("CON", {}) == "CON_" && FolderName("com1.txt", {}) == "com1.txt_" &&
                  FolderName("SDMC", {}) == "SDMC_",
              "reserved");
        Check(FolderName("Mod", {"mod", "Mod (2)"}) == "Mod (3)", "unique");
        Check(FolderName("", {}) == "Mod", "empty");
        const std::string accents = "Ñandú ";
        std::string long_name;
        for (int i = 0; i < 20; ++i) long_name += accents; // 7 bytes each with the space.
        const auto cut = FolderName(long_name, {});
        Check(cut.size() <= 64 && (static_cast<unsigned char>(cut.back()) & 0xC0) != 0xC0, "utf-8 cut");
    }
    std::puts("mod-layout PASS");
}
