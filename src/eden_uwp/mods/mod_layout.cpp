// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#include "eden_uwp/mods/mod_layout.h"

#include <algorithm>
#include <array>
#include <map>
#include <optional>

#include "eden_uwp/mods/mod_model.h"

namespace EdenXbox::Mods {
namespace {

char Lower(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

std::string Lowered(std::string_view text) {
    std::string out{text};
    std::transform(out.begin(), out.end(), out.begin(), Lower);
    return out;
}

bool SameName(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(),
                                              [](char x, char y) { return Lower(x) == Lower(y); });
}

std::vector<std::string_view> Steps(std::string_view path) {
    std::vector<std::string_view> steps;
    std::size_t start = 0;
    while (start <= path.size()) {
        const std::size_t end = std::min(path.find('/', start), path.size());
        steps.push_back(path.substr(start, end - start));
        start = end + 1;
    }
    return steps;
}

std::string Join(const std::vector<std::string_view>& steps, std::size_t from, std::size_t to) {
    std::string out;
    for (std::size_t i = from; i < to; ++i) {
        if (!out.empty()) out += '/';
        out += steps[i];
    }
    return out;
}

bool IsTitleId(std::string_view text) {
    return text.size() == 16 && std::all_of(text.begin(), text.end(), [](char c) {
        return (c >= '0' && c <= '9') || (Lower(c) >= 'a' && Lower(c) <= 'f');
    });
}

std::string_view Extension(std::string_view name) {
    const auto dot = name.rfind('.');
    return dot == std::string_view::npos ? std::string_view{} : name.substr(dot);
}

std::string_view Stem(std::string_view name) {
    const auto dot = name.rfind('.');
    return dot == std::string_view::npos ? name : name.substr(0, dot);
}

bool IsPatchFile(std::string_view name) {
    return SameName(Extension(name), ".ips") || SameName(Extension(name), ".pchtxt");
}

/// The folders the loader reads in a mod, by the name it gives them on disk.
struct KindFolder {
    std::string_view name;
    std::uint8_t part;
};
constexpr std::array KindFolders{
    KindFolder{"romfs", RomFs},       KindFolder{"romfslite", RomFs},
    KindFolder{"romfs_ext", RomFsExt}, KindFolder{"exefs", ExeFs},
    KindFolder{"cheats", Cheats},
};

std::optional<KindFolder> FindKind(std::string_view step) {
    for (const auto& kind : KindFolders)
        if (SameName(step, kind.name)) return kind;
    return std::nullopt;
}

/// Where one archive file goes, when it belongs to a mod.
struct Placement {
    std::string root; ///< The mod's folder in the archive.
    std::string to;   ///< Its path inside the mod.
    std::uint8_t part{};
    bool other_title{};
};

/// Atmosphere packs a mod as <...>/atmosphere/contents/<id>/... and <...>/atmosphere/exefs_patches/:
/// both halves are one mod, rooted at the atmosphere folder.
std::size_t AtmosphereRoot(const std::vector<std::string_view>& steps, std::size_t before) {
    for (std::size_t i = 0; i < before; ++i)
        if (SameName(steps[i], "atmosphere")) return i + 1;
    return std::string_view::npos;
}

bool OtherTitle(const std::vector<std::string_view>& steps, std::size_t before, std::string_view title_id) {
    for (std::size_t i = 0; i + 1 < before; ++i)
        if (SameName(steps[i], "contents") && IsTitleId(steps[i + 1]))
            return !SameName(steps[i + 1], title_id);
    // <title id>/romfs/... as fan translation packs lay a game out.
    if (before >= 1 && IsTitleId(steps[before - 1])) return !SameName(steps[before - 1], title_id);
    return false;
}

std::optional<Placement> Place(const std::vector<std::string_view>& steps, std::string_view title_id) {
    const std::size_t file = steps.size() - 1;
    // Atmosphere's exefs_patches/<name>/<build id>.ips: the loader reads them from exefs/.
    if (file >= 2 && SameName(steps[file - 2], "exefs_patches") && IsPatchFile(steps[file])) {
        const auto atmosphere = AtmosphereRoot(steps, file - 2);
        const std::size_t root = atmosphere != std::string_view::npos ? atmosphere : file;
        return Placement{Join(steps, 0, root), "exefs/" + std::string{steps[file]}, Patch};
    }
    for (std::size_t i = 0; i < file; ++i) {
        const auto kind = FindKind(steps[i]);
        if (!kind) continue;
        const auto atmosphere = AtmosphereRoot(steps, i);
        const std::size_t root = atmosphere != std::string_view::npos ? atmosphere : i;
        std::uint8_t part = kind->part;
        if (part == ExeFs && IsPatchFile(steps[file])) part = Patch;
        return Placement{Join(steps, 0, root),
                         std::string{kind->name} + '/' + Join(steps, i + 1, steps.size()), part,
                         OtherTitle(steps, i, title_id)};
    }
    return std::nullopt;
}

/// Archives with no mod folders at all: loose patches go to exefs/ and cheats named after a build
/// id to cheats/, one mod per folder they sit in (variants are often just folders of patches).
std::optional<Placement> PlaceLoose(const std::vector<std::string_view>& steps) {
    const auto name = steps.back();
    const std::string parent = Join(steps, 0, steps.size() - 1);
    if (IsPatchFile(name)) return Placement{parent, "exefs/" + std::string{name}, Patch};
    if (SameName(Extension(name), ".txt") && IsTitleId(Stem(name)))
        return Placement{parent, "cheats/" + std::string{name}, Cheats};
    return std::nullopt;
}

/// Files authors put beside a mod for people, not for the game: readmes, previews, links.
bool IsDocument(std::string_view name) {
    static constexpr std::array documents{".txt", ".md",  ".pdf", ".url", ".nfo", ".htm", ".html", ".rtf",
                                          ".doc", ".docx", ".jpg", ".jpeg", ".png", ".gif", ".webp", ".bmp",
                                          ".mp4", ".webm"};
    const auto extension = Extension(name);
    return std::any_of(documents.begin(), documents.end(),
                       [&](std::string_view document) { return SameName(extension, document); });
}

/// The folders right under `root` (a "" root is the top) and whether files other than documents
/// sit in `root` itself.
struct Level {
    std::map<std::string, std::vector<std::string>> folders; ///< Folder -> the paths under it.
    bool own_files{};
};

Level LevelOf(const std::vector<std::string>& paths, const std::string& root) {
    Level level;
    for (const auto& path : paths) {
        const std::string_view rest = std::string_view{path}.substr(root.empty() ? 0 : root.size() + 1);
        const auto slash = rest.find('/');
        if (slash == std::string_view::npos) {
            if (!IsDocument(rest)) level.own_files = true;
        } else {
            level.folders[std::string{rest.substr(0, slash)}].push_back(path);
        }
    }
    return level;
}

/// Goes down a folder that wraps everything else ("Mod v2/Audio, Chara..."): only one that holds
/// nothing but two folders or more, so a lone game folder ("Chara/x.szs") is never taken for one.
std::string Unwrap(const std::vector<std::string>& paths, std::string root) {
    while (true) {
        const auto level = LevelOf(paths, root);
        if (level.own_files || level.folders.size() != 1) return root;
        const auto inner = root.empty() ? level.folders.begin()->first : root + '/' + level.folders.begin()->first;
        const auto below = LevelOf(paths, inner);
        if (below.own_files || below.folders.size() < 2) return root;
        root = inner;
    }
}

/// The folders under a level, lower-cased.
std::vector<std::string> Children(const std::vector<std::string>& paths, const std::string& root) {
    std::vector<std::string> names;
    for (const auto& [name, _] : LevelOf(paths, root).folders) names.push_back(Lowered(name));
    return names;
}

/// A bare mod: its folder in the archive and its files, all of them romfs.
using BareMod = std::pair<std::string, std::vector<std::string>>;

/// With the game's top RomFS folders known, a mod starts at the first of them in each path; mods
/// side by side ("Red/Chara", "Blue/Chara") are variants. Paths that reach none are left out.
std::vector<BareMod> MatchRomFs(const std::vector<std::string>& paths, const std::vector<std::string>& romfs_top) {
    std::map<std::string, std::vector<std::string>> by_root;
    for (const auto& path : paths) {
        const auto steps = Steps(path);
        for (std::size_t i = 0; i + 1 < steps.size(); ++i) {
            if (std::find(romfs_top.begin(), romfs_top.end(), Lowered(steps[i])) == romfs_top.end()) continue;
            by_root[Join(steps, 0, i)].push_back(path);
            break;
        }
    }
    return {by_root.begin(), by_root.end()};
}

/// Without them, from the archive's shape: what is under the wrapping folders is romfs, and
/// folders side by side holding the same folders are variants.
std::vector<BareMod> GuessRomFs(const std::vector<std::string>& paths) {
    const std::string root = Unwrap(paths, {});
    const auto level = LevelOf(paths, root);
    std::vector<std::string> variants;
    if (!level.own_files && level.folders.size() >= 2) {
        std::vector<std::vector<std::string>> shapes;
        for (const auto& [name, _] : level.folders)
            shapes.push_back(Children(paths, root.empty() ? name : root + '/' + name));
        bool all_share = true;
        for (std::size_t i = 0; i < shapes.size() && all_share; ++i) {
            bool shares = false;
            for (std::size_t j = 0; j < shapes.size() && !shares; ++j)
                if (i != j)
                    shares = std::any_of(shapes[i].begin(), shapes[i].end(), [&](const std::string& n) {
                        return std::find(shapes[j].begin(), shapes[j].end(), n) != shapes[j].end();
                    });
            all_share = shares;
        }
        if (all_share)
            for (const auto& [name, _] : level.folders)
                variants.push_back(Unwrap(paths, root.empty() ? name : root + '/' + name));
    }
    if (variants.empty()) variants.push_back(root);

    std::vector<BareMod> mods;
    for (const auto& mod_root : variants) {
        std::vector<std::string> files;
        for (const auto& path : paths)
            if (mod_root.empty() || (path.starts_with(mod_root) && path.size() > mod_root.size() &&
                                     path[mod_root.size()] == '/'))
                files.push_back(path);
        mods.emplace_back(mod_root, std::move(files));
    }
    return mods;
}

/// Archives with the game's own folders and no romfs/ around them, as many games' mods ship.
/// Documents beside a mod's folders stay out (inside them they are the game's files), and a mod
/// needs something that is not a document, so an archive of screenshots and readmes is no mod.
std::vector<BareMod> BareRomFs(const std::vector<std::string>& all, const std::vector<std::string>& romfs_top) {
    std::vector<std::string> paths;
    for (const auto& path : all)
        if (path.find('/') != std::string::npos || !IsDocument(path)) paths.push_back(path);
    auto mods = romfs_top.empty() ? GuessRomFs(paths) : MatchRomFs(paths, romfs_top);
    std::erase_if(mods, [](BareMod& mod) {
        auto& [root, files] = mod;
        std::erase_if(files, [&](const std::string& path) {
            const auto inside = std::string_view{path}.substr(root.empty() ? 0 : root.size() + 1);
            return inside.find('/') == std::string_view::npos && IsDocument(inside);
        });
        return std::none_of(files.begin(), files.end(), [](const std::string& p) { return !IsDocument(p); });
    });
    return mods;
}

/// The name an author gave a mod: the innermost folder of its root that is not part of a layout.
std::string CandidateName(std::string_view root, std::string_view archive_name) {
    const auto steps = Steps(root);
    for (auto it = steps.rbegin(); it != steps.rend(); ++it) {
        if (it->empty() || IsTitleId(*it)) continue;
        if (SameName(*it, "contents") || SameName(*it, "atmosphere") ||
            SameName(*it, "exefs_patches") || SameName(*it, "switch"))
            continue;
        return std::string{*it};
    }
    return std::string{Stem(archive_name)};
}

} // namespace

bool SafeRelativePath(std::string_view path) {
    if (path.empty() || path.front() == '/' || path.find('\\') != std::string_view::npos ||
        path.find(':') != std::string_view::npos)
        return false;
    for (const auto step : Steps(path)) {
        if (step.empty() || step == "." || step == "..") return false;
        if (std::any_of(step.begin(), step.end(), [](char c) { return static_cast<unsigned char>(c) < 0x20; }))
            return false;
    }
    return true;
}

LayoutPlan PlanLayout(const std::vector<ArchiveItem>& items, std::string_view title_id,
                      std::string_view archive_name, const std::vector<std::string>& romfs_top) {
    const auto plan = [&](auto place) {
        std::map<std::string, ModCandidate> by_root;
        for (const auto& item : items) {
            if (!SafeRelativePath(item.path)) continue;
            const auto placement = place(Steps(item.path));
            if (!placement) continue;
            auto& candidate = by_root[placement->root];
            candidate.root = placement->root;
            const bool taken = std::any_of(candidate.files.begin(), candidate.files.end(),
                                           [&](const LayoutFile& f) { return SameName(f.to, placement->to); });
            if (taken) continue;
            candidate.files.push_back({item.path, placement->to, item.size});
            candidate.parts |= placement->part;
            candidate.bytes += item.size;
            candidate.other_title |= placement->other_title;
        }
        LayoutPlan result;
        for (auto& [root, candidate] : by_root) {
            candidate.name = CandidateName(root, archive_name);
            std::sort(candidate.files.begin(), candidate.files.end(),
                      [](const LayoutFile& a, const LayoutFile& b) { return a.to < b.to; });
            result.candidates.push_back(std::move(candidate));
        }
        return result;
    };
    auto result = plan([&](const std::vector<std::string_view>& steps) { return Place(steps, title_id); });
    if (result.candidates.empty()) result = plan(PlaceLoose);
    if (result.candidates.empty()) {
        std::vector<std::string> paths;
        std::map<std::string, std::uint64_t> sizes;
        for (const auto& item : items) {
            if (!SafeRelativePath(item.path)) continue;
            paths.push_back(item.path);
            sizes[item.path] = item.size;
        }
        for (auto& [root, files] : BareRomFs(paths, romfs_top)) {
            ModCandidate candidate;
            candidate.root = root;
            candidate.name = CandidateName(root, archive_name);
            candidate.parts = RomFs;
            for (const auto& path : files) {
                const auto inside = std::string_view{path}.substr(root.empty() ? 0 : root.size() + 1);
                candidate.files.push_back({path, "romfs/" + std::string{inside}, sizes[path]});
                candidate.bytes += sizes[path];
            }
            std::sort(candidate.files.begin(), candidate.files.end(),
                      [](const LayoutFile& a, const LayoutFile& b) { return a.to < b.to; });
            result.candidates.push_back(std::move(candidate));
        }
    }
    return result;
}

std::string FolderName(std::string_view name, const std::vector<std::string>& taken) {
    std::string base;
    for (const char c : name) {
        const bool refused = static_cast<unsigned char>(c) < 0x20 ||
                             std::string_view{"<>:\"/\\|?*"}.find(c) != std::string_view::npos;
        base += refused ? '_' : c;
    }
    const auto first = base.find_first_not_of(' ');
    base.erase(0, first == std::string::npos ? base.size() : first);
    // At most 64 bytes, cut on a UTF-8 character boundary.
    if (base.size() > 64) {
        std::size_t cut = 64;
        while (cut > 0 && (static_cast<unsigned char>(base[cut]) & 0xC0) == 0x80) --cut;
        base.resize(cut);
    }
    while (!base.empty() && (base.back() == ' ' || base.back() == '.')) base.pop_back();
    if (base.empty()) base = "Mod";
    // Device names Windows reserves, and SDMC, which the loader uses for the SD card's mods.
    static constexpr std::array reserved{"con", "prn", "aux", "nul", "sdmc"};
    const std::string stem = Lowered(Stem(base));
    const bool device = std::find(reserved.begin(), reserved.end(), stem) != reserved.end() ||
                        (stem.size() == 4 && (stem.starts_with("com") || stem.starts_with("lpt")) &&
                         stem[3] >= '1' && stem[3] <= '9');
    if (device) base += '_';

    const auto used = [&](const std::string& candidate) {
        return std::any_of(taken.begin(), taken.end(),
                           [&](const std::string& t) { return SameName(t, candidate); });
    };
    std::string result = base;
    for (int n = 2; used(result); ++n) result = base + " (" + std::to_string(n) + ")";
    return result;
}

} // namespace EdenXbox::Mods
