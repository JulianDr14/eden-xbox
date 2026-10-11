// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#include "eden_uwp/cheats/cheat_session.h"

#include <algorithm>
#include <mutex>
#include <optional>
#include <thread>

#include <winrt/base.h>

#include "common/logging.h"
#include "eden_uwp/cheats/cheat_db.h"
#include "eden_uwp/cheats/cheat_fetcher.h"

namespace EdenXbox::Cheats {

struct CheatSession::Shared {
    struct Result {
        std::optional<CheatCatalog> catalog; ///< Empty when the worker failed.
        std::uint32_t edits{}; ///< The session's edits when the catalog was read.
        std::optional<CheatSource> source;
        std::size_t other_builds{};
        std::vector<std::string> credits;
    };
    std::mutex mutex;
    std::vector<Result> results;
    std::atomic<std::uint32_t> edits{};
    std::atomic<bool> stop{};
};

namespace {
/// Reads the files, then asks the database for both categories, installs what it has for this
/// build and reads the files again. Runs detached: leaving the page never waits for the network.
void Run(std::shared_ptr<CheatSession::Shared> shared, CheatGame game, std::filesystem::path title_dir,
         bool online) {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    struct Uninitialize { ~Uninitialize() { winrt::uninit_apartment(); } } guard;
    const auto publish = [&](CheatSession::Shared::Result result) {
        const std::scoped_lock lock{shared->mutex};
        shared->results.push_back(std::move(result));
    };
    try {
        publish({LoadCatalog(title_dir, game.build_id), shared->edits.load()});
        if (!online) return;
        const std::string title_id = HexId(game.title_id);
        const std::string build_id = HexId(game.build_id);
        const auto cache_dir = DefaultCheatCacheDirectory();
        CheatSession::Shared::Result result;
        std::vector<std::string> builds;
        bool reached = false, cached = false;
        for (const CheatCategory category : {CheatCategory::Gameplay, CheatCategory::Graphics}) {
            if (shared->stop) return;
            const FetchResult fetched = FetchCheatDb(cache_dir, category, title_id);
            reached |= fetched.status == FetchStatus::Fresh || fetched.status == FetchStatus::Missing;
            cached |= fetched.status == FetchStatus::Stale;
            if (fetched.json.empty()) continue;
            auto title = ParseCheatDbTitle(fetched.json, build_id);
            if (!title) {
                LOG_WARNING(Frontend, "Cheats database: unreadable {} file for {}",
                            category == CheatCategory::Graphics ? "graphics" : "gameplay", title_id);
                continue;
            }
            for (auto& build : title->builds)
                if (build != build_id && std::find(builds.begin(), builds.end(), build) == builds.end())
                    builds.push_back(std::move(build));
            for (auto& credit : title->credits)
                if (std::find(result.credits.begin(), result.credits.end(), credit) == result.credits.end())
                    result.credits.push_back(std::move(credit));
            if (!title->cheats.empty() && !InstallDbCheats(title_dir, category, game.build_id, title->cheats))
                LOG_WARNING(Frontend, "Cheats database: could not write the cheats of {}", title_id);
        }
        result.source = cached ? CheatSource::Cached : reached ? CheatSource::Online : CheatSource::Offline;
        result.other_builds = builds.size();
        result.edits = shared->edits.load();
        result.catalog = LoadCatalog(title_dir, game.build_id);
        publish(std::move(result));
    } catch (const std::exception& error) {
        LOG_ERROR(Frontend, "Cheats: {}", error.what());
        publish({std::nullopt, shared->edits.load(), CheatSource::Offline});
    } catch (const winrt::hresult_error& error) {
        LOG_ERROR(Frontend, "Cheats: {}", winrt::to_string(error.message()));
        publish({std::nullopt, shared->edits.load(), CheatSource::Offline});
    }
}
} // namespace

CheatSession::CheatSession(CheatGame game_)
    : game{game_}, title_dir{TitleLoadDirectory(game.title_id)} {
    Start(true);
}

CheatSession::~CheatSession() {
    shared->stop = true;
}

void CheatSession::Start(bool online) {
    if (shared) shared->stop = true;
    shared = std::make_shared<Shared>();
    std::thread(Run, shared, game, title_dir, online).detach();
}

bool CheatSession::Poll() {
    std::vector<Shared::Result> results;
    {
        const std::scoped_lock lock{shared->mutex};
        results.swap(shared->results);
    }
    for (auto& result : results) {
        // A change saved after the worker read the files is on disk: read them again.
        if (result.catalog)
            catalog = result.edits == shared->edits ? std::move(*result.catalog)
                                                    : LoadCatalog(title_dir, game.build_id);
        if (result.source) {
            source = *result.source;
            other_builds = result.other_builds;
            credits = std::move(result.credits);
        }
        loaded = true;
    }
    return !results.empty();
}

ToggleResult CheatSession::Toggle(CheatRef ref) {
    const ToggleResult result = catalog.Toggle(ref);
    if (result == ToggleResult::Changed) Save(ref.set);
    return result;
}

ToggleResult CheatSession::SetCategory(CheatCategory category, bool enabled) {
    const ToggleResult result = catalog.SetCategory(category, enabled);
    // All on stops at the budget, maybe after turning some on: save whatever changed.
    for (std::uint32_t set = 0; set < catalog.Sets().size(); ++set)
        if (catalog.Sets()[set].category == category) Save(set);
    return result;
}

void CheatSession::Save(std::uint32_t set) {
    ++shared->edits;
    saved = SaveToggles(catalog.Sets()[set]);
}

void CheatSession::Retry() {
    if (source != CheatSource::Offline && source != CheatSource::Cached) return;
    source = CheatSource::Checking;
    Start(true);
}

} // namespace EdenXbox::Cheats
