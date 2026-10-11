// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
// Build with src/eden_uwp/mods/mod_manifest.cpp and nlohmann_json's include folder.
#include <cstdio>
#include <cstdlib>
#include "eden_uwp/mods/mod_manifest.h"

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
    std::vector<ModRecord> mods(2);
    mods[0].folder = "Water";
    mods[0].name = "Agua mejorada";
    mods[0].enabled = false;
    mods[0].installed = 1700000000;
    mods[0].source = {SourceKind::GameBanana, "water.7z", 12345, 678, "1.2", 1700000001,
                       "https://gamebanana.com/dl/678"};
    mods[1].folder = "Fps";
    mods[1].name = "Fps";

    const auto parsed = ParseManifest(SerializeManifest(mods));
    Check(parsed && *parsed == mods, "round trip keeps order and fields");

    Check(!ParseManifest("not json"), "garbage");
    Check(!ParseManifest(R"({"version":99,"mods":[]})"), "newer schema");
    Check(!ParseManifest(R"({"version":1})"), "no list");
    // A bad record is skipped, not the file; duplicate folders keep the first.
    const auto tolerant = ParseManifest(
        R"({"version":1,"mods":[{"folder":"a","enabled":"yes"},{"folder":"b"},{"folder":"B"},3,{"name":"x"}]})");
    Check(tolerant && tolerant->size() == 1 && (*tolerant)[0].folder == "b" && (*tolerant)[0].enabled,
          "tolerant parse");

    // Invalid UTF-8 from an archive name must not lose the manifest.
    std::vector<ModRecord> odd(1);
    odd[0].folder = "x";
    odd[0].name = "bad \xff byte";
    Check(ParseManifest(SerializeManifest(odd)).has_value(), "invalid utf-8 name");

    // Reconcile: gone folders drop, new ones join last and on, matched without case.
    auto state = mods;
    Check(Reconcile(state, {"water", "New"}, 5), "reconcile changed");
    Check(state.size() == 2 && state[0].folder == "Water" && !state[0].enabled, "kept record");
    Check(state[1].folder == "New" && state[1].enabled && state[1].installed == 5 &&
              state[1].source.kind == SourceKind::Local,
          "adopted folder");
    Check(!Reconcile(state, {"Water", "New"}, 6), "stable");

    const auto settings = ToLoaderSettings(mods);
    Check(settings.priority == std::vector<std::string>{"Water", "Fps"}, "priority");
    Check(settings.disabled == std::vector<std::string>{"Water"}, "disabled");
    std::puts("mod-manifest PASS");
}
