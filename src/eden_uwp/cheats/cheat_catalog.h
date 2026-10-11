// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// One game's cheats as the player sees them: every cheats file Eden would load for the running
// build, sorted into categories, with what each costs the cheat VM. The library page and the
// in-game menu share this model; the loader reads the same files through FileSys::ReadCheatFiles,
// and the engine gets Program() when cheats change while the game runs.

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/memory/cheat_text.h"

namespace EdenXbox::Cheats {

enum class CheatCategory : std::uint8_t { Gameplay, Graphics, Mine };
inline constexpr std::size_t CategoryCount = 3;

/// The mod folders the online database fills, one per category it has. Any other mod folder with
/// a `cheats` folder is the player's own.
inline constexpr std::string_view OnlineGameplayMod = "Online cheats";
inline constexpr std::string_view OnlineGraphicsMod = "Online graphics";

inline CheatCategory CategoryOfMod(std::string_view mod) {
    if (mod == OnlineGameplayMod) return CheatCategory::Gameplay;
    if (mod == OnlineGraphicsMod) return CheatCategory::Graphics;
    return CheatCategory::Mine;
}

inline std::string_view ModOfCategory(CheatCategory category) {
    return category == CheatCategory::Graphics ? OnlineGraphicsMod : OnlineGameplayMod;
}

/// Words the cheat VM holds for all enabled cheats (DmntCheatVm::MaximumProgramOpcodeCount).
inline constexpr std::size_t WordBudget = 0x400;

/// One cheats file: as Core::Memory::TextCheatParser returns it (entry 0 is the master cheat),
/// with its toggles applied, and where those toggles are saved.
struct CheatSet {
    std::string mod;
    CheatCategory category{CheatCategory::Mine};
    std::vector<Core::Memory::CheatEntry> cheats;
    std::filesystem::path toggles;
};

/// A cheat of the catalog: its set and its entry there (never 0, the master cheat).
struct CheatRef {
    std::uint32_t set{};
    std::uint32_t entry{};
};

enum class ToggleResult : std::uint8_t {
    Changed,
    Unchanged,
    OverBudget, ///< Turning it on would not fit in the VM: nothing changed.
};

class CheatCatalog {
public:
    CheatCatalog() = default;
    explicit CheatCatalog(std::vector<CheatSet> sets_) : sets{std::move(sets_)} {
        for (std::uint32_t set = 0; set < sets.size(); ++set) {
            auto& list = refs[static_cast<std::size_t>(sets[set].category)];
            for (std::uint32_t entry = 1; entry < sets[set].cheats.size(); ++entry)
                list.push_back({set, entry});
        }
    }

    [[nodiscard]] bool Empty() const {
        for (const auto& list : refs)
            if (!list.empty()) return false;
        return true;
    }
    [[nodiscard]] std::span<const CheatRef> Cheats(CheatCategory category) const {
        return refs[static_cast<std::size_t>(category)];
    }
    [[nodiscard]] const CheatSet& SetOf(CheatRef ref) const { return sets[ref.set]; }
    [[nodiscard]] std::span<const CheatSet> Sets() const { return sets; }
    [[nodiscard]] std::string_view Name(CheatRef ref) const {
        return Core::Memory::CheatName(Entry(ref));
    }
    [[nodiscard]] bool Enabled(CheatRef ref) const { return Entry(ref).enabled; }
    [[nodiscard]] std::size_t EnabledCount(CheatCategory category) const {
        std::size_t count = 0;
        for (const CheatRef ref : Cheats(category)) count += Enabled(ref) ? 1 : 0;
        return count;
    }

    /// VM words the enabled cheats take, master cheats included.
    [[nodiscard]] std::size_t UsedWords() const {
        std::size_t words = 0;
        for (const CheatSet& set : sets) words += Core::Memory::EnabledOpcodeCount(set.cheats);
        return words;
    }
    /// Words turning `ref` on adds: its own, plus its set's master cheat if that was off.
    [[nodiscard]] std::size_t AddedWords(CheatRef ref) const {
        const auto& cheats = sets[ref.set].cheats;
        std::size_t words = cheats[ref.entry].definition.num_opcodes;
        if (!cheats[0].enabled) words += cheats[0].definition.num_opcodes;
        return words;
    }

    ToggleResult Set(CheatRef ref, bool enabled) {
        auto& cheats = sets[ref.set].cheats;
        if (cheats[ref.entry].enabled == enabled) return ToggleResult::Unchanged;
        if (enabled && UsedWords() + AddedWords(ref) > WordBudget) return ToggleResult::OverBudget;
        cheats[ref.entry].enabled = enabled;
        Core::Memory::SyncMasterCheat(cheats);
        return ToggleResult::Changed;
    }
    ToggleResult Toggle(CheatRef ref) { return Set(ref, !Enabled(ref)); }

    /// Turns every cheat of `category` on (as many as fit, in order) or off.
    ToggleResult SetCategory(CheatCategory category, bool enabled) {
        ToggleResult result = ToggleResult::Unchanged;
        for (const CheatRef ref : Cheats(category)) {
            const ToggleResult step = Set(ref, enabled);
            if (step == ToggleResult::OverBudget) return ToggleResult::OverBudget;
            if (step == ToggleResult::Changed) result = ToggleResult::Changed;
        }
        return result;
    }

    /// The cheat list the engine runs, in the loader's order.
    [[nodiscard]] std::vector<Core::Memory::CheatEntry> Program() const {
        std::vector<Core::Memory::CheatEntry> program;
        for (const CheatSet& set : sets) program.insert(program.end(), set.cheats.begin(), set.cheats.end());
        return program;
    }

private:
    [[nodiscard]] const Core::Memory::CheatEntry& Entry(CheatRef ref) const {
        return sets[ref.set].cheats[ref.entry];
    }

    std::vector<CheatSet> sets;
    std::array<std::vector<CheatRef>, CategoryCount> refs;
};

/// `toggles.txt` for `cheats`, keeping the toggles in `existing` of cheats it does not have (those
/// of the game's other builds share the file, as on Atmosphere).
inline std::string MergeCheatToggles(std::string_view existing,
                                     std::span<const Core::Memory::CheatEntry> cheats) {
    std::string text = Core::Memory::FormatCheatToggles(cheats);
    for (const auto& toggle : Core::Memory::ParseCheatToggles(existing)) {
        bool known = false;
        for (std::size_t i = 1; i < cheats.size() && !known; ++i)
            known = Core::Memory::CheatName(cheats[i]) == toggle.name;
        if (!known) text += "[" + toggle.name + (toggle.enabled ? "]\ntrue\n" : "]\nfalse\n");
    }
    return text;
}

} // namespace EdenXbox::Cheats
