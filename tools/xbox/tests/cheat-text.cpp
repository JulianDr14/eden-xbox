// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
// Build with src/core/memory/cheat_text.cpp.
#include <cstdio>
#include <cstdlib>
#include "core/memory/cheat_text.h"

int main() {
    using namespace Core::Memory;
    auto check = [](bool ok) { if (!ok) std::abort(); };
    const TextCheatParser parser;

    // A valid file reads as before: the master first, every cheat on, ids in order.
    {
        const auto cheats = parser.Parse("{Master}\n580F0000 03426F60\n"
                                         "[Jump]\n80000002\n640F0000 00000000 40B00000\n20000000\n"
                                         "[Money] 04000000 00880580 52800035\n");
        check(cheats.size() == 3);
        check(CheatName(cheats[0]) == "Master" && cheats[0].definition.num_opcodes == 2);
        check(CheatName(cheats[1]) == "Jump" && cheats[1].definition.num_opcodes == 5);
        check(cheats[1].definition.opcodes[2] == 0x00000000 &&
              cheats[1].definition.opcodes[3] == 0x40B00000);
        check(CheatName(cheats[2]) == "Money" && cheats[2].definition.opcodes[2] == 0x52800035);
        check(cheats[0].enabled && cheats[1].enabled && cheats[2].enabled);
        check(cheats[2].cheat_id == 2);
        check(EnabledOpcodeCount(cheats) == 2 + 5 + 3);
    }

    // No master: entry 0 stays empty and off. Text before the first header is ignored.
    {
        const auto cheats = parser.Parse("\xEF\xBB\xBFGame title v1.0\r\n[A]\r\n04000000 00000001 "
                                         "00000002\r\n");
        check(cheats.size() == 2 && !cheats[0].enabled && cheats[0].definition.num_opcodes == 0);
        check(CheatName(cheats[1]) == "A" && cheats[1].definition.num_opcodes == 3);
    }

    // A damaged cheat is left out, the rest of the file survives (Eden used to drop it all).
    {
        const auto cheats = parser.Parse("// comment\n[Good]\n04000000 00000001 00000002\n"
                                         "[Bad word]\n0400000 00000001\n"
                                         "# another comment\n[Bad token]\n04000000 hello\n"
                                         "[Empty]\n"
                                         "[]\n04000000 00000001 00000002\n"
                                         "\xEF\xBB\xBF[Also good]\n04000000 00000003 00000004\n"
                                         "{Second master}\n04000000 00000005 00000006\n"
                                         "[Unterminated\n04000000");
        check(cheats.size() == 3);
        check(CheatName(cheats[1]) == "Good" && CheatName(cheats[2]) == "Also good");
        check(cheats[2].cheat_id == 2);
    }

    // More than 0x100 opcodes in one cheat drops that cheat only.
    {
        std::string text = "[Long]\n";
        for (int i = 0; i < 0x101; ++i) text += "00000000 ";
        text += "\n[Short]\n00000001\n";
        const auto cheats = parser.Parse(text);
        check(cheats.size() == 2 && CheatName(cheats[1]) == "Short");
    }

    // Long names keep 63 bytes without cutting a UTF-8 sequence.
    {
        std::string name(62, 'x');
        name += "\xC3\xA9\xC3\xA9"; // two two-byte letters: the first would straddle byte 63
        const auto cheats = parser.Parse("[" + name + "]\n00000001\n");
        check(cheats.size() == 2 && CheatName(cheats[1]) == std::string(62, 'x'));
    }

    // toggles.txt: Atmosphere's format, round trip, repeated names, case and spellings.
    {
        auto cheats = parser.Parse("{M}\n00000001\n[A]\n00000001\n[B]\n00000001\n[A]\n00000001\n"
                                   "[C]\n00000001\n");
        const auto toggles = ParseCheatToggles("\xEF\xBB\xBF[A]\r\ntrue\r\n[B]\nOFF\n[A]\n0\n"
                                               "[C]\nmaybe\n[Missing]\nfalse\n[M]\nfalse\n");
        check(toggles.size() == 5);
        ApplyCheatToggles(cheats, toggles);
        check(cheats[0].enabled); // the master always runs
        check(cheats[1].enabled && !cheats[2].enabled && !cheats[3].enabled && cheats[4].enabled);
        const std::string text = FormatCheatToggles(cheats);
        check(text == "[A]\ntrue\n[B]\nfalse\n[A]\nfalse\n[C]\ntrue\n");
        auto again = parser.Parse("{M}\n00000001\n[A]\n00000001\n[B]\n00000001\n[A]\n00000001\n"
                                  "[C]\n00000001\n");
        ApplyCheatToggles(again, ParseCheatToggles(text));
        for (std::size_t i = 0; i < again.size(); ++i) check(again[i].enabled == cheats[i].enabled);
        check(EnabledOpcodeCount(cheats) == 3);
        // With every cheat off the master stops too: it only sets up the file's cheats.
        ApplyCheatToggles(cheats, ParseCheatToggles("[A]\nfalse\n[C]\nfalse\n"));
        check(!cheats[0].enabled && EnabledOpcodeCount(cheats) == 0);
        cheats[2].enabled = true;
        SyncMasterCheat(cheats);
        check(cheats[0].enabled);
    }

    std::puts("cheat-text PASS");
    return 0;
}
