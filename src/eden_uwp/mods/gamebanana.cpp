// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#include "eden_uwp/mods/gamebanana.h"

#include <algorithm>
#include <array>
#include <charconv>

#include <nlohmann/json.hpp>

namespace EdenXbox::Mods::GameBanana {
namespace {
using nlohmann::json;

constexpr std::string_view Api = "https://gamebanana.com/apiv11/";

constexpr std::array<std::string_view, SortCount> SortNames{
    "Generic_MostDownloaded", "Generic_Newest", "Generic_LatestUpdated", "Generic_MostLiked"};

// The API is loose with types (a count may come as a string): read every field leniently, so one
// odd field costs that field, not the answer.
std::string Str(const json& object, const char* key) {
    const auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

std::int64_t Int(const json& object, const char* key) {
    const auto it = object.find(key);
    if (it == object.end()) return 0;
    if (it->is_number_integer()) return it->get<std::int64_t>();
    if (it->is_number_float()) return static_cast<std::int64_t>(it->get<double>());
    if (it->is_string()) {
        const auto& text = it->get_ref<const std::string&>();
        std::int64_t value = 0;
        std::from_chars(text.data(), text.data() + text.size(), value);
        return value;
    }
    return 0;
}

bool Bool(const json& object, const char* key) {
    const auto it = object.find(key);
    return it != object.end() && it->is_boolean() && it->get<bool>();
}

const json* Object(const json& object, const char* key) {
    const auto it = object.find(key);
    return it != object.end() && it->is_object() ? &*it : nullptr;
}

const json* Array(const json& object, const char* key) {
    const auto it = object.find(key);
    return it != object.end() && it->is_array() ? &*it : nullptr;
}

/// The URLs of a submission's pictures, at the first of `sizes` each has.
std::vector<std::string> Pictures(const json& item, std::initializer_list<const char*> sizes) {
    std::vector<std::string> urls;
    const auto* media = Object(item, "_aPreviewMedia");
    const auto* images = media ? Array(*media, "_aImages") : nullptr;
    if (!images) return urls;
    for (const auto& image : *images) {
        if (!image.is_object()) continue;
        const auto base = Str(image, "_sBaseUrl");
        if (base.empty()) continue;
        std::string file;
        for (const char* size : sizes)
            if (file = Str(image, size); !file.empty()) break;
        if (file.empty()) file = Str(image, "_sFile");
        if (!file.empty()) urls.push_back(base + '/' + file);
    }
    return urls;
}

ModSummary Summary(const json& item) {
    ModSummary mod;
    mod.id = Int(item, "_idRow");
    mod.name = Str(item, "_sName");
    if (const auto* submitter = Object(item, "_aSubmitter")) mod.author = Str(*submitter, "_sName");
    const auto* category = Object(item, "_aRootCategory");
    if (!category) category = Object(item, "_aCategory");
    if (category) mod.category = Str(*category, "_sName");
    mod.version = Str(item, "_sVersion");
    if (const auto pictures = Pictures(item, {"_sFile220"}); !pictures.empty()) mod.thumbnail = pictures.front();
    mod.likes = Int(item, "_nLikeCount");
    mod.views = Int(item, "_nViewCount");
    mod.added = Int(item, "_tsDateAdded");
    mod.modified = std::max(Int(item, "_tsDateModified"), Int(item, "_tsDateUpdated"));
    // "show" is for everyone; "warn" and "hide" are what the site keeps behind its adult filter.
    const auto visibility = Str(item, "_sInitialVisibility");
    mod.sensitive = !visibility.empty() && visibility != "show";
    return mod;
}

bool IsHex(std::string_view text) {
    return std::all_of(text.begin(), text.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

ModFile File(const json& item) {
    ModFile file;
    file.id = Int(item, "_idRow");
    file.name = Str(item, "_sFile");
    file.description = Str(item, "_sDescription");
    file.url = Str(item, "_sDownloadUrl");
    file.size = static_cast<std::uint64_t>(std::max<std::int64_t>(Int(item, "_nFilesize"), 0));
    file.added = Int(item, "_tsDateAdded");
    file.downloads = Int(item, "_nDownloadCount");
    auto md5 = Str(item, "_sMd5Checksum");
    std::transform(md5.begin(), md5.end(), md5.begin(),
                   [](char c) { return c >= 'A' && c <= 'F' ? static_cast<char>(c - 'A' + 'a') : c; });
    if (md5.size() == 32 && IsHex(md5)) file.md5 = std::move(md5);
    // Files still being analysed say so too: only a finished, clean result counts.
    file.safe = Str(item, "_sAvResult") == "clean" && Str(item, "_sAnalysisResult") == "ok" &&
                !Bool(item, "_bIsArchived") && file.url.starts_with("https://");
    return file;
}

json Parse(std::string_view text) {
    json root = json::parse(text, nullptr, false);
    // The API answers errors as an object with an error code, sometimes with a 200.
    if (!root.is_object() || root.contains("_sErrorCode")) return json{};
    return root;
}

// Text helpers for the description and for matching names.

char LowerAscii(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

void AppendUtf8(std::string& out, std::uint32_t code) {
    if (code == 0 || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) return;
    if (code < 0x80) {
        out += static_cast<char>(code);
    } else if (code < 0x800) {
        out += static_cast<char>(0xC0 | (code >> 6));
        out += static_cast<char>(0x80 | (code & 0x3F));
    } else if (code < 0x10000) {
        out += static_cast<char>(0xE0 | (code >> 12));
        out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (code & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (code >> 18));
        out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (code & 0x3F));
    }
}

/// Decodes the entity at `html[at]` ('&') into `out`; returns its length, 0 when it is none.
std::size_t DecodeEntity(std::string_view html, std::size_t at, std::string& out) {
    const auto end = html.find(';', at);
    if (end == std::string_view::npos || end - at > 10) return 0;
    const auto name = html.substr(at + 1, end - at - 1);
    static constexpr std::pair<std::string_view, std::uint32_t> named[] = {
        {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''},
        {"nbsp", ' '}, {"hellip", 0x2026}, {"mdash", 0x2014}, {"ndash", 0x2013},
        {"rsquo", 0x2019}, {"lsquo", 0x2018}, {"rdquo", 0x201D}, {"ldquo", 0x201C}, {"copy", 0xA9}};
    for (const auto& [entity, code] : named) {
        if (name != entity) continue;
        AppendUtf8(out, code);
        return end - at + 1;
    }
    if (name.size() < 2 || name[0] != '#') return 0;
    const bool hex = name[1] == 'x' || name[1] == 'X';
    const auto digits = name.substr(hex ? 2 : 1);
    std::uint32_t code = 0;
    if (std::from_chars(digits.data(), digits.data() + digits.size(), code, hex ? 16 : 10).ptr !=
        digits.data() + digits.size())
        return 0;
    AppendUtf8(out, code == 0xA0 ? ' ' : code);
    return end - at + 1;
}

/// Lower-case words of a name, without trademark signs and punctuation; non-ASCII letters stay.
std::vector<std::string> Words(std::string_view text) {
    std::vector<std::string> words;
    std::string word;
    const auto flush = [&] {
        if (!word.empty()) words.push_back(std::move(word));
        word.clear();
    };
    for (std::size_t i = 0; i < text.size();) {
        const auto rest = text.substr(i);
        if (rest.starts_with("\xE2\x84\xA2")) { i += 3; continue; }                         // ™
        if (rest.starts_with("\xC2\xAE") || rest.starts_with("\xC2\xA9")) { i += 2; continue; } // ® ©
        const auto c = static_cast<unsigned char>(text[i]);
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c >= 0x80)
            word += LowerAscii(static_cast<char>(c));
        else
            flush();
        ++i;
    }
    flush();
    return words;
}

std::string Joined(const std::vector<std::string>& words) {
    std::string out;
    for (const auto& word : words) {
        if (!out.empty()) out += ' ';
        out += word;
    }
    return out;
}

/// How well a game's name matches the title: 100 the same words, 80 one starts with the other,
/// 60 it has all the title's words, less for some of them.
int MatchScore(const std::vector<std::string>& title, std::string_view name) {
    const auto words = Words(name);
    if (title.empty() || words.empty()) return 0;
    const auto a = Joined(title), b = Joined(words);
    if (a == b) return 100;
    if (b.starts_with(a + ' ') || a.starts_with(b + ' ')) return 80;
    const auto found = static_cast<int>(std::count_if(title.begin(), title.end(), [&](const std::string& w) {
        return std::find(words.begin(), words.end(), w) != words.end();
    }));
    if (found == static_cast<int>(title.size())) return 60;
    return found * 50 / static_cast<int>(title.size());
}

} // namespace

std::string PercentEncode(std::string_view text) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        const bool plain = (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'z') ||
                           (byte >= 'A' && byte <= 'Z') || byte == '-' || byte == '_' || byte == '.' ||
                           byte == '~';
        if (plain) {
            out += c;
        } else {
            out += '%';
            out += hex[byte >> 4];
            out += hex[byte & 0xF];
        }
    }
    return out;
}

std::string SearchGamesUrl(std::string_view name) {
    return std::string{Api} + "Util/Search/Results?_sModelName=Game&_nPage=1&_sSearchString=" +
           PercentEncode(name);
}

std::string ModListUrl(std::int64_t game, Sort sort, unsigned page, std::string_view search,
                       std::int64_t category, std::string_view rating) {
    std::string url = std::string{Api} + "Mod/Index?_nPage=" + std::to_string(std::max(page, 1u)) +
                      "&_nPerpage=" + std::to_string(PageSize) +
                      "&_aFilters%5BGeneric_Game%5D=" + std::to_string(game) +
                      "&_sSort=" + std::string{SortNames[static_cast<std::size_t>(sort) % SortCount]};
    if (!search.empty()) url += "&_aFilters%5BGeneric_Name%5D=contains," + PercentEncode(search);
    if (category > 0) url += "&_aFilters%5BGeneric_Category%5D=" + std::to_string(category);
    if (!rating.empty()) url += "&_aFilters%5BGeneric_ContentRatings%5D=" + PercentEncode(rating);
    return url;
}

std::string CategoriesUrl(std::int64_t game, std::int64_t parent) {
    std::string url = std::string{Api} + "Mod/Categories?_sSort=a_to_z&_bShowEmpty=false&";
    url += parent > 0 ? "_idCategoryRow=" + std::to_string(parent) : "_idGameRow=" + std::to_string(game);
    return url;
}

std::string ModDetailUrl(std::int64_t mod) {
    return std::string{Api} + "Mod/" + std::to_string(mod) + "/ProfilePage";
}

std::string ModFilesUrl(std::int64_t mod) {
    return std::string{Api} + "Mod/" + std::to_string(mod) + "?_csvProperties=_idRow,_aFiles,_tsDateModified";
}

std::optional<std::vector<Game>> ParseGames(std::string_view text) {
    const json root = Parse(text);
    const auto* records = root.is_object() ? Array(root, "_aRecords") : nullptr;
    if (!records) return std::nullopt;
    std::vector<Game> games;
    for (const auto& item : *records) {
        if (!item.is_object() || Str(item, "_sModelName") != "Game") continue;
        Game game{Int(item, "_idRow"), Str(item, "_sName")};
        if (game.id > 0 && !game.name.empty()) games.push_back(std::move(game));
    }
    return games;
}

std::optional<ModList> ParseModList(std::string_view text) {
    const json root = Parse(text);
    const auto* records = root.is_object() ? Array(root, "_aRecords") : nullptr;
    if (!records) return std::nullopt;
    ModList list;
    for (const auto& item : *records) {
        if (!item.is_object()) continue;
        auto mod = Summary(item);
        if (mod.id > 0 && !mod.name.empty()) list.mods.push_back(std::move(mod));
    }
    if (const auto* metadata = Object(root, "_aMetadata")) {
        list.total = static_cast<std::size_t>(std::max<std::int64_t>(Int(*metadata, "_nRecordCount"), 0));
        list.complete = Bool(*metadata, "_bIsComplete");
    }
    // A short page is the last one, whatever the metadata says.
    if (list.mods.size() < PageSize) list.complete = true;
    return list;
}

std::optional<std::vector<Category>> ParseCategories(std::string_view text) {
    // The only answer that is an array, not an object: errors still come as objects.
    const json root = json::parse(text, nullptr, false);
    if (!root.is_array()) return std::nullopt;
    std::vector<Category> categories;
    for (const auto& item : root) {
        if (!item.is_object()) continue;
        Category category{Int(item, "_idRow"), Str(item, "_sName"), Int(item, "_nItemCount"),
                          Int(item, "_nCategoryCount") > 0};
        if (category.id > 0 && !category.name.empty() && category.mods > 0) categories.push_back(std::move(category));
    }
    return categories;
}

std::vector<ModSummary> MergeLists(const std::vector<ModList>& lists, Sort sort) {
    std::vector<ModSummary> mods;
    for (const auto& list : lists)
        for (const auto& mod : list.mods)
            if (std::none_of(mods.begin(), mods.end(), [&](const ModSummary& m) { return m.id == mod.id; }))
                mods.push_back(mod);
    const auto key = [sort](const ModSummary& mod) {
        switch (sort) {
        case Sort::Popular: return mod.views;
        case Sort::Newest: return mod.added;
        case Sort::Updated: return mod.modified;
        case Sort::Liked: return mod.likes;
        }
        return mod.views;
    };
    std::stable_sort(mods.begin(), mods.end(), [&](const ModSummary& a, const ModSummary& b) { return key(a) > key(b); });
    return mods;
}

std::optional<ModDetail> ParseModDetail(std::string_view text) {
    const json root = Parse(text);
    if (!root.is_object() || Int(root, "_idRow") <= 0) return std::nullopt;
    ModDetail detail;
    detail.summary = Summary(root);
    detail.text = HtmlToText(Str(root, "_sText"), 1600);
    if (detail.text.empty()) detail.text = Str(root, "_sDescription");
    detail.images = Pictures(root, {"_sFile530", "_sFile800"});
    detail.downloads = Int(root, "_nDownloadCount");
    if (const auto* files = Array(root, "_aFiles")) {
        for (const auto& item : *files)
            if (item.is_object()) detail.files.push_back(File(item));
    }
    // Newest first: what most players want is the latest release.
    std::stable_sort(detail.files.begin(), detail.files.end(),
                     [](const ModFile& a, const ModFile& b) { return a.added > b.added; });
    return detail;
}

std::optional<std::size_t> BestGame(const std::vector<Game>& games, std::string_view title) {
    const auto words = Words(title);
    int best = 0;
    std::size_t index = 0, ties = 0;
    for (std::size_t i = 0; i < games.size(); ++i) {
        const int score = MatchScore(words, games[i].name);
        if (score > best) {
            best = score;
            index = i;
            ties = 1;
        } else if (score == best) {
            ++ties;
        }
    }
    // The same name is taken at once; a close one only when no other is as close.
    if (best == 100 || (best >= 60 && ties == 1)) return index;
    return std::nullopt;
}

std::int64_t NewestFile(const std::vector<ModFile>& files) {
    std::int64_t newest = 0;
    for (const auto& file : files) newest = std::max(newest, file.added);
    return newest;
}

std::string HtmlToText(std::string_view html, std::size_t max_bytes) {
    std::string out;
    // Ends the text so far with `count` line breaks (at most), without spaces before them.
    const auto breaks = [&](std::size_t count) {
        while (!out.empty() && out.back() == ' ') out.pop_back();
        if (out.empty()) return;
        std::size_t have = 0;
        for (auto it = out.rbegin(); it != out.rend() && *it == '\n'; ++it) ++have;
        for (; have < count; ++have) out += '\n';
    };
    for (std::size_t i = 0; i < html.size();) {
        const char c = html[i];
        if (c == '<') {
            const auto end = html.find('>', i);
            if (end == std::string_view::npos) break;
            const auto tag = html.substr(i + 1, end - i - 1);
            const bool closing = !tag.empty() && tag.front() == '/';
            std::string name;
            for (std::size_t k = closing ? 1 : 0; k < tag.size() && tag[k] != ' ' && tag[k] != '/'; ++k)
                name += LowerAscii(tag[k]);
            i = end + 1;
            if (!closing && (name == "script" || name == "style")) {
                const auto close = html.find("</", i);
                i = close == std::string_view::npos ? html.size() : close;
                continue;
            }
            static constexpr std::string_view blocks[] = {"p", "div", "h1", "h2", "h3", "h4", "h5", "h6",
                                                          "ul", "ol", "hr", "blockquote", "table", "tr"};
            if (name == "br") breaks(1);
            else if (std::find(std::begin(blocks), std::end(blocks), name) != std::end(blocks)) breaks(2);
            else if (name == "li" && !closing) {
                breaks(1);
                out += "\xE2\x80\xA2 "; // •
            } else if (name == "li") breaks(1);
            else if ((name == "td" || name == "th") && closing) out += ' ';
            continue;
        }
        if (c == '&') {
            if (const auto used = DecodeEntity(html, i, out)) {
                i += used;
                continue;
            }
        }
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
            if (!out.empty() && out.back() != ' ' && out.back() != '\n') out += ' ';
            ++i;
            continue;
        }
        out += c;
        ++i;
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '\n')) out.pop_back();
    if (out.size() > max_bytes) {
        std::size_t cut = max_bytes;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) --cut;
        out.resize(cut);
        while (!out.empty() && (out.back() == ' ' || out.back() == '\n')) out.pop_back();
        out += "\xE2\x80\xA6"; // …
    }
    return out;
}

} // namespace EdenXbox::Mods::GameBanana
