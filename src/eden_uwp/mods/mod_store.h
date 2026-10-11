// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The online half of a game's mods page: what GameBanana and the CNX index offer for the game,
// one mod's details, and which installed mods have a newer file. Requests run one at a time on a
// worker; each hands back a change that Poll applies on the page's thread, so the state is only
// ever touched there. A change for a list that was since replaced (another order, another search)
// is dropped.
//
// GameBanana knows games by its own ids: the game is looked up by name once and the answer kept
// (mods/gamebanana.json), and the player can pick another when the name finds the wrong one.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "eden_uwp/mods/cnx_index.h"
#include "eden_uwp/mods/gamebanana.h"
#include "eden_uwp/mods/mod_downloads.h"
#include "eden_uwp/mods/mod_thumbnails.h"
#include "eden_uwp/mods/work_queue.h"

namespace EdenXbox::Mods {

enum class StoreSource : std::uint8_t { GameBanana, Cnx };

enum class StoreStatus : std::uint8_t {
    Idle,    ///< Not opened yet.
    Loading,
    Ready,
    Empty,   ///< Nothing for this game (or this search).
    Offline, ///< The source could not be reached.
    NoGame,  ///< GameBanana has no game by this name: the player picks it.
};

/// A mod as the store's grid shows it.
struct StoreCard {
    std::int64_t id{}; ///< GameBanana's mod id; for CNX, the entry's place in the index.
    std::string name;
    std::string author;
    std::string category;
    std::string size; ///< CNX gives a size instead of an author.
    std::string thumbnail;
    std::int64_t likes{};
    std::int64_t views{};
    bool sensitive{};
    bool translation{}; ///< A CNX translation (else a modification).
};

struct StoreDetail {
    StoreSource source{};
    StoreCard card;
    std::string text;
    std::string version;
    std::vector<std::string> images;
    std::vector<GameBanana::ModFile> files;
    std::int64_t downloads{};
};

/// A row of the categories sheet: everything at this level, the adult list (at the top level, when
/// adult mods are shown), or one category.
struct CategoryRow {
    enum class Kind : std::uint8_t { All, Adult, Category };
    Kind kind{};
    std::size_t index{}; ///< Category: its place in StoreState::categories.
};

/// An installed mod that has a newer file at its source.
struct ModUpdate {
    StoreSource source{};
    std::int64_t mod_id{};
    Cnx::Entry entry; ///< CNX: the entry that replaces it.
};

struct StoreState {
    StoreSource source{StoreSource::GameBanana};
    GameBanana::Sort sort{GameBanana::Sort::Popular};
    std::string search;
    /// The category the list shows, with the ones above it (empty: all of them).
    std::vector<GameBanana::Category> category;
    bool adult{}; ///< The list shows the mods rated for adults, of every category.
    std::optional<GameBanana::Game> game;
    std::vector<GameBanana::Game> games; ///< A game search's results, to pick from.
    bool searching_games{};
    std::vector<StoreCard> cards;
    std::size_t hidden{}; ///< Sensitive mods left out of `cards`.
    bool complete{};
    StoreStatus status{StoreStatus::Idle};
    bool loading_more{};
    std::optional<StoreDetail> detail;
    bool detail_loading{};
    bool detail_failed{};
    std::string replace; ///< The installed mod's folder the open detail updates.
    std::map<std::string, ModUpdate> updates; ///< By installed folder.
    // The categories sheet: the level browsed (the categories above it) and its categories.
    std::vector<GameBanana::Category> browse;
    std::vector<GameBanana::Category> categories;
    bool categories_loading{};
    bool categories_failed{};
};

class ModStore {
public:
    /// `game_name` finds the game on GameBanana; `data` is mods/, where the game ids are kept.
    ModStore(std::uint64_t title_id, std::string game_name, bool show_sensitive, std::filesystem::path data);

    [[nodiscard]] const StoreState& State() const { return state; }
    [[nodiscard]] const ThumbnailCache& Pictures() const { return pictures; }
    [[nodiscard]] bool ShowsSensitive() const { return show_sensitive; }
    [[nodiscard]] bool Working() const { return !queue.Idle(); }
    /// True once when a picture arrived since the last call.
    bool TakePictures() { return pictures.TakeArrived(); }

    /// Applies what finished; true when the state changed.
    bool Poll();

    /// The store is shown: the first time, finds the game and lists its mods.
    void Open();
    /// Starts the list over (after the source could not be reached).
    void Retry() { Restart(); }
    /// The cursor is on card `index`: near the end, the next page loads.
    void Want(std::size_t index);
    void CycleSort();
    void Search(std::string text);
    void SwitchSource();
    void FindGame(std::string name);
    /// The categories sheet opens on the level of the category shown.
    void OpenCategories();
    /// The sheet's rows at the level browsed.
    [[nodiscard]] std::vector<CategoryRow> CategoryRows() const;
    /// Row `row` has categories under it: the sheet goes down to them.
    void EnterCategory(std::size_t row);
    /// The sheet goes up a level; false at the top.
    bool LeaveCategory();
    /// The list shows row `row`'s mods.
    void PickCategory(std::size_t row);
    void PickGame(std::size_t index);
    void OpenDetail(std::size_t card);
    /// Opens the source of an installed mod that has an update, to pick the new file.
    void OpenUpdate(const std::string& folder, const std::string& name);
    void CloseDetail();
    /// Looks for newer files of the installed mods that came from a store.
    void CheckUpdates(const std::vector<ModRecord>& mods);

    /// The download for file `index` of the open detail; nullopt for a file that is not safe.
    [[nodiscard]] std::optional<DownloadRequest> Request(std::size_t file) const;

private:
    /// A finished request's effect on the state, applied by Poll on the page's thread.
    using Change = std::function<void()>;
    void Post(std::function<Change(std::stop_token)> job, bool urgent = false);
    void Restart();
    void LookUpGame(std::string name, bool pick);
    void LoadPage();
    void LoadCnx();
    void LoadAdult();
    void LoadCategories();
    void LoadDetail(std::int64_t id, StoreCard card, std::string replace);
    void SaveGame() const;

    std::uint64_t title_id;
    std::string game_name;
    bool show_sensitive;
    std::filesystem::path data;
    StoreState state;
    std::vector<Cnx::Entry> cnx; ///< The game's entries in the CNX index.
    unsigned generation{};        ///< Bumped whenever the list starts over.
    unsigned pages{};
    std::int64_t detail_id{-1};
    std::map<std::int64_t, std::vector<GameBanana::Category>> category_cache; ///< By parent (0: the top).
    std::int64_t categories_of{-1}; ///< The parent whose categories the sheet waits for.
    std::mutex mutex;
    std::vector<Change> changes;
    ThumbnailCache pictures;
    WorkQueue queue; ///< Last: its thread stops before the rest goes.
};

} // namespace EdenXbox::Mods
