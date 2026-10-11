// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// GameBanana's public API (apiv11), as much of it as the store uses: finding a game by its name,
// its mod categories, listing its mods a page at a time (sorted, filtered by a text, a category or
// a content rating), and one mod's description, pictures and files. Only the URLs and the parsing of the answers live here, with no network, so
// they are tested against real answers (tools/xbox/tests/gamebanana-parse.cpp).

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace EdenXbox::Mods::GameBanana {

enum class Sort : std::uint8_t { Popular, Newest, Updated, Liked };
inline constexpr std::size_t SortCount = 4;

/// Mods asked for per page: two screens of the grid.
inline constexpr unsigned PageSize = 16;

/// A game as GameBanana names it.
struct Game {
    std::int64_t id{};
    std::string name;
};

/// A mod as a list shows it.
struct ModSummary {
    std::int64_t id{};
    std::string name;
    std::string author;
    std::string category;
    std::string version;
    std::string thumbnail; ///< URL of a small picture (220 px wide); empty when it has none.
    std::int64_t likes{};
    std::int64_t views{};
    std::int64_t added{};    ///< Unix time.
    std::int64_t modified{}; ///< Unix time.
    /// Rated for adults: the site shows it only behind a warning, or not at all.
    bool sensitive{};
};

/// A category of a game's mods. Categories nest (Skins > Characters > one character); listing a
/// category lists the mods of the categories under it too.
struct Category {
    std::int64_t id{};
    std::string name;
    std::int64_t mods{};
    bool nested{}; ///< It has categories under it.
};

/// The content ratings of sexual content: nudity, partial nudity, sexual activity, sexual themes.
/// The site filters by one rating at a time, so the store's adult list asks for each and merges.
inline constexpr std::array<std::string_view, 4> AdultRatings{"nu", "pn", "sa", "st"};

struct ModList {
    std::vector<ModSummary> mods;
    std::size_t total{};
    bool complete{true}; ///< No more pages.
};

/// One downloadable file of a mod: a mod often has several (versions, variants, extras).
struct ModFile {
    std::int64_t id{};
    std::string name;
    std::string description;
    std::string url;
    std::string md5; ///< Lower-case hex; empty when the site gives none.
    std::uint64_t size{};
    std::int64_t added{}; ///< Unix time.
    std::int64_t downloads{};
    /// Passed the site's antivirus and file analysis; others are shown but never downloaded.
    bool safe{};
};

struct ModDetail {
    ModSummary summary;
    std::string text;                ///< Its description as plain text.
    std::vector<std::string> images; ///< Its pictures (530 px wide when the site has that size).
    std::vector<ModFile> files;
    std::int64_t downloads{};
};

std::string SearchGamesUrl(std::string_view name);
/// Page `page` (from 1) of the game's mods. Each filter applies when set: `search` by name,
/// `category` by category (with those under it), `rating` by one content rating.
std::string ModListUrl(std::int64_t game, Sort sort, unsigned page, std::string_view search,
                       std::int64_t category = 0, std::string_view rating = {});
/// The categories under `parent`, or the game's top ones when `parent` is 0; empty ones are left out.
std::string CategoriesUrl(std::int64_t game, std::int64_t parent);
std::string ModDetailUrl(std::int64_t mod);
/// Only the files of a mod: what an update check needs.
std::string ModFilesUrl(std::int64_t mod);

/// Each returns nullopt when the text is not the answer it expects (an error page, an API error).
std::optional<std::vector<Game>> ParseGames(std::string_view json);
std::optional<ModList> ParseModList(std::string_view json);
std::optional<std::vector<Category>> ParseCategories(std::string_view json);
/// Reads a full profile (ModDetailUrl) or only its files (ModFilesUrl).
std::optional<ModDetail> ParseModDetail(std::string_view json);

/// The game among `games` whose name is `title`'s, if one matches well enough to pick it without
/// asking; marks and punctuation do not count ("Name™: Subtitle" matches "Name - Subtitle").
std::optional<std::size_t> BestGame(const std::vector<Game>& games, std::string_view title);

/// Several lists as one: each mod once, in `sort`'s order as the site sorts (views stand for
/// downloads, which lists do not carry).
std::vector<ModSummary> MergeLists(const std::vector<ModList>& lists, Sort sort);

/// The newest file's date: a mod has an update when it is newer than the file installed.
std::int64_t NewestFile(const std::vector<ModFile>& files);

/// A description's HTML as text: paragraphs and list items on their own lines, entities decoded,
/// cut to `max_bytes` on a character boundary.
std::string HtmlToText(std::string_view html, std::size_t max_bytes);

/// `text` (UTF-8) for a URL query.
std::string PercentEncode(std::string_view text);

} // namespace EdenXbox::Mods::GameBanana
