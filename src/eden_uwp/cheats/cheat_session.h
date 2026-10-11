// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// One game's cheats while a page shows them, in the library or in the in-game menu. The files on
// disk come first (a few small reads); the online database is asked on a worker meanwhile and its
// cheats join the list when they arrive. The page polls once per frame; nothing here blocks it.

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "eden_uwp/cheats/cheat_catalog.h"
#include "eden_uwp/cheats/cheat_store.h"

namespace EdenXbox::Cheats {

struct CheatGame {
    std::uint64_t title_id{};
    BuildId build_id{};
};

/// What the page says about the online database.
enum class CheatSource : std::uint8_t {
    Checking, ///< Asking it.
    Online,   ///< Up to date.
    Cached,   ///< It could not be reached; its last copy is used.
    Offline,  ///< It could not be reached and nothing was ever downloaded.
};

class CheatSession {
public:
    explicit CheatSession(CheatGame game);
    ~CheatSession();
    CheatSession(const CheatSession&) = delete;
    CheatSession& operator=(const CheatSession&) = delete;

    /// Takes what the worker finished. True when anything shown changed.
    bool Poll();

    [[nodiscard]] const CheatGame& Game() const { return game; }
    [[nodiscard]] bool Loading() const { return !loaded; }
    [[nodiscard]] const CheatCatalog& Catalog() const { return catalog; }
    [[nodiscard]] CheatSource Source() const { return source; }
    /// Builds of the game the database has cheats for, besides this one.
    [[nodiscard]] std::size_t OtherBuilds() const { return other_builds; }
    [[nodiscard]] const std::vector<std::string>& Credits() const { return credits; }
    /// False after a change could not be saved (it still applies until the game closes).
    [[nodiscard]] bool Saved() const { return saved; }

    /// Change cheats and save their files' toggles.
    ToggleResult Toggle(CheatRef ref);
    ToggleResult SetCategory(CheatCategory category, bool enabled);

    /// Asks the database again, after it could not be reached.
    void Retry();

    struct Shared;

private:
    void Start(bool online);
    void Save(std::uint32_t set);

    CheatGame game;
    std::filesystem::path title_dir;
    std::shared_ptr<Shared> shared;
    CheatCatalog catalog;
    CheatSource source{CheatSource::Checking};
    std::size_t other_builds{};
    std::vector<std::string> credits;
    bool loaded{};
    bool saved{true};
};

} // namespace EdenXbox::Cheats
