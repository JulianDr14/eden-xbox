// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>
#include "core/memory/dmnt_cheat_types.h"

// The text formats of Atmosphere's cheats: `<build id>.txt` (cheat definitions) and `toggles.txt`
// (which cheats are on). Pure functions, so the frontends and the tests use the same code as the
// loader.

namespace Core::Memory {

// Intermediary class that parses a text file or other disk format for storing cheats into a
// CheatList object, that can be used for execution.
class CheatParser {
public:
    virtual ~CheatParser();

    [[nodiscard]] virtual std::vector<CheatEntry> Parse(std::string_view data) const = 0;
};

// CheatParser implementation that parses text files. Entry 0 is always the master cheat (`{Name}`,
// empty when the file has none). A damaged cheat (a bad opcode word, too many opcodes, a second
// master cheat, an unknown token) is left out instead of discarding the whole file, and so is a
// header with no opcodes (a section label); `//` and `#` comments and byte order marks are
// skipped. Every cheat starts enabled.
class TextCheatParser final : public CheatParser {
public:
    ~TextCheatParser() override;

    [[nodiscard]] std::vector<CheatEntry> Parse(std::string_view data) const override;
};

/// The cheat's name, without the terminator.
[[nodiscard]] std::string_view CheatName(const CheatEntry& entry);

/// Opcode words the VM runs for `cheats`: the sum over the enabled ones.
[[nodiscard]] std::size_t EnabledOpcodeCount(std::span<const CheatEntry> cheats);

/// `cheats` back as text TextCheatParser reads (empty master cheat left out).
[[nodiscard]] std::string FormatCheatFile(std::span<const CheatEntry> cheats);

struct CheatToggle {
    std::string name;
    bool enabled{};
};

/// Atmosphere's `toggles.txt`: `[Name]` followed by `true`/`false` (also `1`/`0`, `on`/`off`).
[[nodiscard]] std::vector<CheatToggle> ParseCheatToggles(std::string_view text);

/// The master cheat (entry 0) only sets up the file's cheats: it runs while one of them is on.
void SyncMasterCheat(std::span<CheatEntry> cheats);

/// Sets `enabled` of every cheat named in `toggles`, for `cheats` as TextCheatParser returns them,
/// then SyncMasterCheat. Cheats not named keep their state.
void ApplyCheatToggles(std::span<CheatEntry> cheats, std::span<const CheatToggle> toggles);

/// `toggles.txt` for `cheats` as TextCheatParser returns them (the master cheat is left out).
[[nodiscard]] std::string FormatCheatToggles(std::span<const CheatEntry> cheats);

} // namespace Core::Memory
