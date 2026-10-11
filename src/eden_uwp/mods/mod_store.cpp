// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#include "eden_uwp/mods/mod_store.h"

#include <algorithm>
#include <cstdio>

#include <nlohmann/json.hpp>

#include "common/logging.h"
#include "eden_uwp/atomic_file.h"
#include "eden_uwp/mods/mod_web.h"

namespace EdenXbox::Mods {
namespace {
namespace fs = std::filesystem;
namespace GB = GameBanana;
using nlohmann::json;

/// Pictures kept on disk between sessions.
constexpr std::uint64_t PictureBudget = 64ull << 20;
/// The CNX index changes a few times a month: checked at most once an hour.
constexpr int CnxRecheckMinutes = 60;
/// A new page loads when the cursor gets this close to the end of the list (two grid rows).
constexpr std::size_t LoadAhead = 8;

fs::path PicturesFolder() {
    const auto folder = Web::CacheFolder() / "pictures";
    ThumbnailCache::Trim(folder, PictureBudget);
    return folder;
}

fs::path GamesFile(const fs::path& data) {
    return data / "gamebanana.json";
}

char Lower(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

bool Contains(std::string_view text, std::string_view part) {
    return std::search(text.begin(), text.end(), part.begin(), part.end(),
                       [](char a, char b) { return Lower(a) == Lower(b); }) != text.end();
}

std::string FileName(std::string_view url) {
    const auto slash = url.rfind('/');
    return std::string{slash == std::string_view::npos ? url : url.substr(slash + 1)};
}

StoreDetail CnxDetail(const Cnx::Entry& entry, std::int64_t index) {
    StoreDetail detail;
    detail.source = StoreSource::Cnx;
    detail.card.id = index;
    detail.card.name = entry.name;
    detail.card.size = entry.size;
    detail.card.translation = entry.kind == Cnx::Kind::Translation;
    GB::ModFile file;
    file.name = FileName(entry.url);
    file.url = entry.url;
    file.size = Cnx::ParseSize(entry.size);
    file.safe = true; // The index is curated; there is no checksum to check against.
    detail.files.push_back(std::move(file));
    return detail;
}

std::string Hex(std::string_view text) {
    std::uint64_t hash = 14695981039346656037ull;
    for (const char c : text) hash = (hash ^ static_cast<unsigned char>(c)) * 1099511628211ull;
    char out[17];
    std::snprintf(out, sizeof(out), "%016llx", static_cast<unsigned long long>(hash));
    return out;
}

} // namespace

ModStore::ModStore(std::uint64_t title_id_, std::string game_name_, bool show_sensitive_, fs::path data_)
    : title_id{title_id_}, game_name{std::move(game_name_)}, show_sensitive{show_sensitive_},
      data{std::move(data_)}, pictures{PicturesFolder()} {
    if (const auto text = ReadWholeFile(GamesFile(data))) {
        const json root = json::parse(*text, nullptr, false);
        const auto entry = root.is_object() ? root.find(TitleFolder(title_id)) : root.end();
        if (root.is_object() && entry != root.end() && entry->is_object()) {
            GB::Game game{entry->value("id", std::int64_t{}), entry->value("name", std::string{})};
            if (game.id > 0) state.game = std::move(game);
        }
    }
}

void ModStore::Post(std::function<Change(std::stop_token)> job, bool urgent) {
    queue.Post([this, job = std::move(job)](std::stop_token stop) {
        auto change = job(stop);
        if (!change || stop.stop_requested()) return;
        const std::scoped_lock lock{mutex};
        changes.push_back(std::move(change));
    }, urgent);
}

bool ModStore::Poll() {
    std::vector<Change> ready;
    {
        const std::scoped_lock lock{mutex};
        ready.swap(changes);
    }
    for (auto& change : ready) change();
    return !ready.empty();
}

void ModStore::Open() {
    if (state.status == StoreStatus::Idle) Restart();
}

void ModStore::Restart() {
    ++generation;
    pages = 0;
    state.cards.clear();
    state.hidden = 0;
    state.complete = false;
    state.loading_more = false;
    state.status = StoreStatus::Loading;
    if (state.source == StoreSource::Cnx) LoadCnx();
    else if (state.game) LoadPage();
    else LookUpGame(game_name, true);
}

void ModStore::LookUpGame(std::string name, bool pick) {
    state.searching_games = true;
    state.games.clear();
    Post([this, name = std::move(name), pick](std::stop_token stop) -> Change {
        const auto body = Web::Get(GB::SearchGamesUrl(name), stop);
        auto games = body ? GB::ParseGames(*body) : std::nullopt;
        return [this, games = std::move(games), name, pick] {
            state.searching_games = false;
            if (!games) {
                if (pick) state.status = StoreStatus::Offline;
                return;
            }
            state.games = *games;
            const auto best = pick ? GB::BestGame(state.games, name) : std::nullopt;
            if (best) {
                LOG_INFO(Frontend, "Mods: GameBanana game {} ({})", state.games[*best].name, state.games[*best].id);
                PickGame(*best);
            } else if (pick) {
                state.status = StoreStatus::NoGame;
            }
        };
    }, true);
}

void ModStore::FindGame(std::string name) {
    LookUpGame(std::move(name), false);
}

void ModStore::PickGame(std::size_t index) {
    if (index >= state.games.size()) return;
    state.game = state.games[index];
    state.games.clear();
    SaveGame();
    if (state.source == StoreSource::GameBanana) Restart();
}

void ModStore::SaveGame() const {
    if (!state.game) return;
    const auto path = GamesFile(data);
    json root = json::object();
    if (const auto text = ReadWholeFile(path)) {
        root = json::parse(*text, nullptr, false);
        if (!root.is_object()) root = json::object();
    }
    root[TitleFolder(title_id)] = {{"id", state.game->id}, {"name", state.game->name}};
    WriteFileAtomically(path, root.dump(2, ' ', false, json::error_handler_t::replace));
}

void ModStore::LoadPage() {
    if (state.loading_more || state.complete || !state.game) return;
    state.loading_more = true;
    const auto url = GB::ModListUrl(state.game->id, state.sort, pages + 1, state.search);
    Post([this, url, gen = generation](std::stop_token stop) -> Change {
        const auto body = Web::Get(url, stop);
        auto list = body ? GB::ParseModList(*body) : std::nullopt;
        return [this, list = std::move(list), gen] {
            if (gen != generation) return; // A list that was since started over.
            state.loading_more = false;
            if (!list) {
                if (state.cards.empty()) state.status = StoreStatus::Offline;
                return;
            }
            ++pages;
            for (const auto& mod : list->mods) {
                if (mod.sensitive && !show_sensitive) {
                    ++state.hidden;
                    continue;
                }
                state.cards.push_back({mod.id, mod.name, mod.author, mod.category, {}, mod.thumbnail,
                                       mod.likes, mod.views, mod.sensitive, false});
            }
            state.complete = list->complete;
            if (!state.cards.empty()) state.status = StoreStatus::Ready;
            else if (state.complete) state.status = StoreStatus::Empty;
            else LoadPage(); // A page of hidden mods only: the next may have some to show.
        };
    });
}

void ModStore::LoadCnx() {
    Post([this, gen = generation](std::stop_token stop) -> Change {
        const auto text = Web::GetCached(std::string{Cnx::IndexUrl}, Web::CacheFolder() / "cnx-index.json",
                                         CnxRecheckMinutes, stop);
        auto entries = text ? std::optional{Cnx::ParseIndex(*text, TitleFolder(title_id))} : std::nullopt;
        return [this, entries = std::move(entries), gen] {
            if (gen != generation) return;
            state.complete = true;
            if (!entries) {
                state.status = StoreStatus::Offline;
                return;
            }
            cnx = *entries;
            for (std::size_t i = 0; i < cnx.size(); ++i) {
                if (!state.search.empty() && !Contains(cnx[i].name, state.search)) continue;
                StoreCard card;
                card.id = static_cast<std::int64_t>(i);
                card.name = cnx[i].name;
                card.size = cnx[i].size;
                card.translation = cnx[i].kind == Cnx::Kind::Translation;
                state.cards.push_back(std::move(card));
            }
            state.status = state.cards.empty() ? StoreStatus::Empty : StoreStatus::Ready;
        };
    });
}

void ModStore::Want(std::size_t index) {
    if (state.source == StoreSource::GameBanana && state.status == StoreStatus::Ready &&
        index + LoadAhead >= state.cards.size())
        LoadPage();
}

void ModStore::CycleSort() {
    state.sort = static_cast<GB::Sort>((static_cast<std::size_t>(state.sort) + 1) % GB::SortCount);
    if (state.source == StoreSource::GameBanana) Restart();
}

void ModStore::Search(std::string text) {
    const auto first = text.find_first_not_of(' ');
    text.erase(0, first == std::string::npos ? text.size() : first);
    while (!text.empty() && text.back() == ' ') text.pop_back();
    if (text == state.search) return;
    state.search = std::move(text);
    Restart();
}

void ModStore::SwitchSource() {
    state.source = state.source == StoreSource::GameBanana ? StoreSource::Cnx : StoreSource::GameBanana;
    CloseDetail();
    Restart();
}

void ModStore::OpenDetail(std::size_t index) {
    if (index >= state.cards.size()) return;
    const auto& card = state.cards[index];
    if (state.source == StoreSource::Cnx) {
        CloseDetail();
        if (static_cast<std::size_t>(card.id) < cnx.size()) state.detail = CnxDetail(cnx[card.id], card.id);
        return;
    }
    LoadDetail(card.id, card, {});
}

void ModStore::LoadDetail(std::int64_t id, StoreCard card, std::string replace) {
    CloseDetail();
    state.detail_loading = true;
    state.replace = std::move(replace);
    detail_id = id;
    Post([this, id, card = std::move(card)](std::stop_token stop) -> Change {
        const auto body = Web::Get(GB::ModDetailUrl(id), stop);
        auto parsed = body ? GB::ParseModDetail(*body) : std::nullopt;
        return [this, parsed = std::move(parsed), card, id] {
            if (detail_id != id) return; // Closed, or another one opened since.
            state.detail_loading = false;
            if (!parsed) {
                state.detail_failed = true;
                return;
            }
            StoreDetail detail;
            detail.source = StoreSource::GameBanana;
            const auto& mod = parsed->summary;
            detail.card = {id, mod.name.empty() ? card.name : mod.name, mod.author, mod.category, {},
                           mod.thumbnail, mod.likes, mod.views, mod.sensitive, false};
            detail.text = parsed->text;
            detail.version = mod.version;
            detail.images = parsed->images;
            detail.files = parsed->files;
            detail.downloads = parsed->downloads;
            state.detail = std::move(detail);
        };
    }, true);
}

void ModStore::OpenUpdate(const std::string& folder, const std::string& name) {
    const auto found = state.updates.find(folder);
    if (found == state.updates.end()) return;
    const auto& update = found->second;
    if (update.source == StoreSource::Cnx) {
        CloseDetail();
        state.detail = CnxDetail(update.entry, -1);
        state.replace = folder;
        return;
    }
    StoreCard card;
    card.id = update.mod_id;
    card.name = name;
    LoadDetail(update.mod_id, std::move(card), folder);
}

void ModStore::CloseDetail() {
    state.detail.reset();
    state.detail_loading = false;
    state.detail_failed = false;
    state.replace.clear();
    detail_id = -1;
}

void ModStore::CheckUpdates(const std::vector<ModRecord>& mods) {
    std::vector<ModRecord> online;
    for (const auto& mod : mods)
        if ((mod.source.kind == SourceKind::GameBanana && mod.source.mod_id > 0) ||
            (mod.source.kind == SourceKind::Cnx && !mod.source.url.empty()))
            online.push_back(mod);
    if (online.empty()) return;
    Post([this, online = std::move(online)](std::stop_token stop) -> Change {
        std::map<std::string, ModUpdate> found;
        std::optional<std::vector<Cnx::Entry>> index;
        for (const auto& mod : online) {
            if (stop.stop_requested()) return {};
            if (mod.source.kind == SourceKind::GameBanana) {
                const auto body = Web::Get(GB::ModFilesUrl(mod.source.mod_id), stop);
                const auto files = body ? GB::ParseModDetail(*body) : std::nullopt;
                if (files && GB::NewestFile(files->files) > mod.source.updated)
                    found[mod.folder] = {StoreSource::GameBanana, mod.source.mod_id, {}};
                continue;
            }
            if (!index) {
                const auto text = Web::GetCached(std::string{Cnx::IndexUrl}, Web::CacheFolder() / "cnx-index.json",
                                                 CnxRecheckMinutes, stop);
                index = text ? Cnx::ParseIndex(*text, TitleFolder(title_id)) : std::vector<Cnx::Entry>{};
            }
            // The same link with another size, or a new link under the same name, is a new release.
            const auto same_link = std::find_if(index->begin(), index->end(),
                                                [&](const Cnx::Entry& e) { return e.url == mod.source.url; });
            if (same_link != index->end()) {
                if (same_link->size != mod.source.version) found[mod.folder] = {StoreSource::Cnx, 0, *same_link};
                continue;
            }
            const auto same_name = std::find_if(index->begin(), index->end(),
                                                [&](const Cnx::Entry& e) { return e.name == mod.name; });
            if (same_name != index->end()) found[mod.folder] = {StoreSource::Cnx, 0, *same_name};
        }
        if (!found.empty()) LOG_INFO(Frontend, "Mods: {} of {} have updates", found.size(), online.size());
        return [this, found = std::move(found)] { state.updates = found; };
    });
}

std::optional<DownloadRequest> ModStore::Request(std::size_t index) const {
    if (!state.detail || index >= state.detail->files.size()) return std::nullopt;
    const auto& detail = *state.detail;
    const auto& file = detail.files[index];
    if (!file.safe) return std::nullopt;
    DownloadRequest request;
    request.title = detail.card.name + " \xC2\xB7 " + file.name;
    request.name = detail.card.name;
    request.url = file.url;
    request.md5 = file.md5;
    request.size = file.size;
    request.replace = state.replace;
    if (detail.source == StoreSource::GameBanana) {
        request.key = "gb-" + std::to_string(file.id);
        request.source = {SourceKind::GameBanana, file.name, detail.card.id, file.id, detail.version, file.added,
                          file.url};
    } else {
        request.key = "cnx-" + Hex(file.url);
        request.source = {SourceKind::Cnx, file.name, 0, 0, detail.card.size, 0, file.url};
    }
    return request;
}

} // namespace EdenXbox::Mods
