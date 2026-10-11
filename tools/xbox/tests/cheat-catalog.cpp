// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
// Build with src/core/memory/cheat_text.cpp and src/eden_uwp/cheats/cheat_db.cpp.
#include <cstdio>
#include <cstdlib>
#include "eden_uwp/cheats/cheat_catalog.h"
#include "eden_uwp/cheats/cheat_db.h"

int main() {
    using namespace EdenXbox::Cheats;
    auto check = [](bool ok) { if (!ok) std::abort(); };
    const Core::Memory::TextCheatParser parser;
    const auto set = [&](std::string mod, std::string_view text) {
        auto cheats = parser.Parse(text);
        Core::Memory::ApplyCheatToggles(cheats, Core::Memory::ParseCheatToggles(""));
        return CheatSet{mod, CategoryOfMod(mod), std::move(cheats), {}};
    };

    // Categories come from the mod folder; the master cheat never shows as a cheat.
    {
        CheatCatalog catalog{{set("Online cheats", "{M}\n00000001 00000002\n[A]\n00000003\n[B]\n00000004\n"),
                              set("Online graphics", "[60 FPS]\n00000005\n"),
                              set("My mod", "[Mine]\n00000006\n")}};
        check(!catalog.Empty());
        check(catalog.Cheats(CheatCategory::Gameplay).size() == 2);
        check(catalog.Cheats(CheatCategory::Graphics).size() == 1);
        check(catalog.Cheats(CheatCategory::Mine).size() == 1);
        const CheatRef a = catalog.Cheats(CheatCategory::Gameplay)[0];
        check(catalog.Name(a) == "A" && catalog.SetOf(a).mod == "Online cheats");
        // Parsed files start all on, the masters with them: 2 + 1 + 1 + 1 + 1 words.
        check(catalog.UsedWords() == 6 && catalog.EnabledCount(CheatCategory::Gameplay) == 2);

        // Off one by one; the master stops with the last cheat of its file.
        check(catalog.Toggle(a) == ToggleResult::Changed && catalog.UsedWords() == 5);
        check(catalog.SetCategory(CheatCategory::Gameplay, false) == ToggleResult::Changed);
        check(catalog.UsedWords() == 2 && catalog.EnabledCount(CheatCategory::Gameplay) == 0);
        check(catalog.SetCategory(CheatCategory::Gameplay, false) == ToggleResult::Unchanged);
        check(catalog.AddedWords(a) == 3); // its word plus the master's two
        check(catalog.Toggle(a) == ToggleResult::Changed && catalog.UsedWords() == 5);

        // The engine gets every file, masters included, in the loader's order.
        const auto program = catalog.Program();
        check(program.size() == 3 + 2 + 2);
        check(program[0].enabled && program[1].enabled && !program[2].enabled);
        check(Core::Memory::EnabledOpcodeCount(program) == catalog.UsedWords());
    }

    // The VM budget: a cheat that would not fit is refused, nothing changes.
    {
        std::string big = "[Big]\n";
        for (int i = 0; i < 0xF0; ++i) big += "00000000 ";
        std::string text;
        for (int i = 0; i < 5; ++i) text += big + "\n";
        auto cheats = parser.Parse(text); // five cheats of 0xF0 words, all on: over the budget
        for (auto& cheat : cheats) cheat.enabled = false;
        CheatCatalog catalog{{CheatSet{"Online cheats", CheatCategory::Gameplay, cheats, {}}}};
        const auto refs = catalog.Cheats(CheatCategory::Gameplay);
        for (int i = 0; i < 4; ++i) check(catalog.Toggle(refs[i]) == ToggleResult::Changed);
        check(catalog.UsedWords() == 4 * 0xF0);
        check(catalog.Toggle(refs[4]) == ToggleResult::OverBudget && !catalog.Enabled(refs[4]));
        check(catalog.Toggle(refs[0]) == ToggleResult::Changed);
        check(catalog.SetCategory(CheatCategory::Gameplay, true) == ToggleResult::OverBudget);
        check(catalog.UsedWords() <= WordBudget);
    }

    // toggles.txt keeps what other builds' files set.
    {
        auto cheats = parser.Parse("[A]\n00000001\n[B]\n00000001\n");
        cheats[2].enabled = false;
        check(MergeCheatToggles("[B]\ntrue\n[Old]\ntrue\n[A]\nfalse\n", cheats) ==
              "[A]\ntrue\n[B]\nfalse\n[Old]\ntrue\n");
    }

    // The database's title file: 16 and 64 digit keys merged, escapes, credits, other builds.
    {
        const std::string_view json = R"({
            "55ee1a69145de04e": {
                "[Moon Jump]": "[Moon Jump]\n80000002\n20000000\n\n",
                "[Inf HP]": "[Inf HP]\n04000000 00000001 00000002\n\n"
            },
            "55EE1A69145DE04EA786BCE3365AA51E00000000000000000000000000000000": {
                "[Inf HP]": "[Inf HP]\n04000000 00000001 00000002\n\n",
                "[Café \"Max\"]": "[Café \"Max\"]\r\n04000000 00000003 00000004\n"
            },
            "0123456789ABCDEF": { "[Other]": "[Other]\n00000001\n" },
            "attribution": { "a.txt": "By someone\n", "b.txt": "By someone\n", "c.txt": "By 😀\n" },
            "unknown": [1, true, null, {"x": -2.5e3}]
        })";
        const auto title = ParseCheatDbTitle(json, "55EE1A69145DE04E00");
        check(title.has_value());
        check(title->builds.size() == 2 && title->builds[0] == "0123456789ABCDEF" &&
              title->builds[1] == "55EE1A69145DE04E");
        check(title->credits.size() == 2 && title->credits[1] == "By \xF0\x9F\x98\x80");
        const auto cheats = parser.Parse(title->cheats);
        check(cheats.size() == 4);
        check(Core::Memory::CheatName(cheats[1]) == "Moon Jump" &&
              Core::Memory::CheatName(cheats[2]) == "Inf HP" &&
              Core::Memory::CheatName(cheats[3]) == "Caf\xC3\xA9 \"Max\"");

        const auto none = ParseCheatDbTitle(json, "FFFFFFFFFFFFFFFF");
        check(none && none->cheats.empty() && none->builds.size() == 2);
        check(!ParseCheatDbTitle(R"({"55EE1A69145DE04E": {"[A]": 5}})", "55EE1A69145DE04E"));
        check(!ParseCheatDbTitle(R"({"55EE1A69145DE04E": {"[A]": "x"})", "55EE1A69145DE04E"));
        check(!ParseCheatDbTitle("404: Not Found", "55EE1A69145DE04E"));
        check(ParseCheatDbTitle("{}", "55EE1A69145DE04E")->builds.empty());
        check(CheatDbUrl(CheatCategory::Graphics, "0100000000010000") ==
              "https://raw.githubusercontent.com/sthetix/nx-cheats-db/main/cheats_gfx/0100000000010000.json");
    }

    std::puts("cheat-catalog PASS");
    return 0;
}
