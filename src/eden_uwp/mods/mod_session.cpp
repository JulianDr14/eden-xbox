// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#include "eden_uwp/mods/mod_session.h"

#include <algorithm>
#include <chrono>

#include <winrt/base.h>

#include "common/logging.h"
#include "core/file_sys/vfs/vfs.h"
#include "eden_uwp/cheats/cheat_catalog.h"
#include "eden_uwp/localization.h"
#include "eden_uwp/mods/archive_volumes.h"
#include "eden_uwp/mods/game_romfs.h"
#include "eden_uwp/mods/mod_inspect.h"
#include "eden_uwp/mods/mod_settings.h"
#include "eden_uwp/mods/mod_web.h"
#include "eden_uwp/storage_path.h"
#include "eden_uwp/utf8.h"
#include "eden_uwp/uwp_rom_storage.h"

namespace EdenXbox::Mods {
namespace {
namespace fs = std::filesystem;

std::int64_t Now() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

/// Archives on internal storage go here (LocalState\eden\mods\inbox), through the Device Portal.
fs::path InboxFolder() {
    return DefaultModPaths().data / "inbox";
}

std::string Utf8(const fs::path& path) {
    const auto text = path.u8string();
    return {text.begin(), text.end()};
}

fs::path Utf8Path(const std::string& text) {
    return fs::path{std::u8string{text.begin(), text.end()}};
}

/// An archive volume read through the app's file system: internal paths and USB drives alike.
class VfsSource final : public ArchiveSource {
public:
    explicit VfsSource(FileSys::VirtualFile file_) : file{std::move(file_)}, size{file->GetSize()} {}

    std::uint64_t Size() const override {
        return size;
    }

    std::size_t Read(std::uint64_t offset, std::uint8_t* out, std::size_t length) override {
        // libarchive calls this from C: an exception (a USB drive pulled out) must not cross it.
        // A short read ends the archive early, which reads as damaged.
        try {
            return file->Read(out, length, static_cast<std::size_t>(offset));
        } catch (...) {
            return 0;
        }
    }

private:
    FileSys::VirtualFile file;
    std::uint64_t size;
};

/// Opens every volume, or none: an archive with a volume missing cannot be read. Folders the
/// player gave (USB drives) are read through the app's file system; internal storage directly.
std::vector<std::unique_ptr<ArchiveSource>> OpenVolumes(const std::vector<std::string>& paths) {
    const auto filesystem = MakeUwpFilesystem();
    std::vector<std::unique_ptr<ArchiveSource>> volumes;
    for (const auto& path : paths) {
        if (!IsStoragePath(path)) {
            auto source = OpenFileSource(Utf8Path(path));
            if (!source) return {};
            volumes.push_back(std::move(source));
            continue;
        }
        auto file = filesystem->OpenFile(path, FileSys::OpenMode::Read);
        if (file == nullptr) return {};
        volumes.push_back(std::make_unique<VfsSource>(std::move(file)));
    }
    return volumes;
}

/// Names the mods of an online mod's archive after it: the archive's own folder names are often
/// a title id or a file name. Variants keep theirs after the mod's.
void NameAfter(std::vector<ModCandidate>& candidates, const std::string& name) {
    if (name.empty()) return;
    for (auto& candidate : candidates)
        candidate.name = candidates.size() == 1 ? name : name + " \xC2\xB7 " + candidate.name;
}

/// A download that can not be installed again is deleted; one that stopped for a reason the
/// player can fix (space, a cancel) stays, and asking for it again installs it without a download.
bool KeepDownload(ModError error) {
    return error == ModError::Space || error == ModError::Cancelled || error == ModError::Write;
}

bool IsArchiveName(std::string_view name) {
    return Detail::ParseVolume(name).has_value();
}

/// A folder's subfolders, then its archives (split ones as one row).
std::vector<FileRow> FolderRows(const std::vector<StorageBrowserEntry>& entries) {
    std::vector<FileRow> rows;
    std::vector<FolderFile> files;
    for (const auto& entry : entries) {
        if (entry.folder) rows.push_back({FileRow::Kind::Folder, entry.name, entry.path, {}});
        else files.push_back({Utf16ToUtf8(entry.name), entry.path});
    }
    for (auto& archive : GroupArchives(files)) {
        std::wstring name = Utf8ToUtf16(archive.name);
        if (archive.volumes.size() > 1)
            name += L"  ·  " + I18n::Format(I18n::Text::ModsVolumes, {std::to_wstring(archive.volumes.size())});
        rows.push_back({FileRow::Kind::Archive, std::move(name), archive.volumes.front(), std::move(archive.volumes)});
    }
    return rows;
}

/// The top of the Files tab: add a folder, internal storage, then every folder the player gave.
void ShowSources(ModsSnapshot& state) {
    state.folder.clear();
    state.folder_name.clear();
    state.files.clear();
    state.files.push_back({FileRow::Kind::AddFolder, std::wstring{I18n::Tr(I18n::Text::ModsAddFolder)}, {}, {}});
    state.files.push_back({FileRow::Kind::Folder, std::wstring{I18n::Tr(I18n::Text::ModsInternal)},
                           Utf8(InboxFolder()), {}});
    for (const auto& source : ContentFolderSources())
        state.files.push_back({FileRow::Kind::Folder, source.name, source.path, {}});
}

/// Internal storage, listed with std::filesystem (the USB drives go through BrowseStorageFolder).
std::vector<StorageBrowserEntry> BrowseInternal(const fs::path& folder) {
    std::vector<StorageBrowserEntry> entries;
    std::error_code ec;
    for (fs::directory_iterator it{folder, ec}, end; !ec && it != end; it.increment(ec)) {
        const bool directory = it->is_directory(ec);
        const auto name = it->path().filename().wstring();
        if (!directory && !IsArchiveName(Utf16ToUtf8(name))) continue;
        entries.push_back({Utf8(it->path()), name, directory});
    }
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
        return a.folder != b.folder ? a.folder > b.folder : a.name < b.name;
    });
    return entries;
}

} // namespace

ModSession::ModSession(std::uint64_t title_id_, std::string build_id_, std::string game_path_,
                       std::string game_name, bool show_sensitive)
    : title_id{title_id_}, build_id{std::move(build_id_)}, game_path{std::move(game_path_)},
      library{DefaultModPaths(), title_id_,
              {std::string{Cheats::OnlineGameplayMod}, std::string{Cheats::OnlineGraphicsMod}}},
      store{title_id_, std::move(game_name), show_sensitive, DefaultModPaths().data},
      downloads{Web::CacheFolder() / "downloads"} {
    Start(ModJob::Loading, [this](ModsSnapshot& state, std::stop_token) {
        ModDownloads::Trim(Web::CacheFolder() / "downloads");
        Reload(state);
        std::error_code ec;
        fs::create_directories(InboxFolder(), ec);
        ShowSources(state);
    });
}

ModSession::~ModSession() {
    worker.request_stop();
    if (worker.joinable()) worker.join();
}

void ModSession::Start(ModJob kind, Job job) {
    if (Busy()) return;
    done = 0;
    total = 0;
    running = kind;
    std::packaged_task<ModsSnapshot(std::stop_token)> task{
        [job = std::move(job), state = view](std::stop_token stop) mutable {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            struct Apartment {
                ~Apartment() { winrt::uninit_apartment(); }
            } apartment;
            state.error = ModError::None;
            try {
                job(state, stop);
            } catch (const winrt::hresult_error& error) {
                LOG_WARNING(Frontend, "Mods: storage error {:08X}", static_cast<std::uint32_t>(error.code()));
                state.error = ModError::Unavailable;
            } catch (const std::exception& error) {
                LOG_WARNING(Frontend, "Mods: {}", error.what());
                state.error = ModError::Write;
            }
            if (stop.stop_requested() && state.error == ModError::None) state.error = ModError::Cancelled;
            return state;
        }};
    pending = task.get_future();
    worker = std::jthread{std::move(task)};
}

ModJob ModSession::Poll() {
    if (!Busy() || pending.wait_for(std::chrono::seconds{0}) != std::future_status::ready) return ModJob::None;
    const ModJob finished = std::exchange(running, ModJob::None);
    try {
        view = pending.get();
    } catch (...) {
        view.error = ModError::Write;
    }
    if (worker.joinable()) worker.join();
    if (finished == ModJob::Installing || finished == ModJob::Removing) changed = true;
    if (finished == ModJob::Loading && !checked_updates) {
        checked_updates = true;
        std::vector<ModRecord> records;
        for (const auto& mod : view.mods) records.push_back(mod.record);
        store.CheckUpdates(records);
    }
    return finished;
}

bool ModSession::Tick() {
    bool redraw = store.Poll();
    redraw |= store.TakePictures();
    // Progress bars and the store's spinner move while either works.
    redraw |= downloads.Active() || store.Working();
    if (!Busy() && view.candidates.empty()) {
        if (auto finished = downloads.TakeFinished()) {
            if (finished->error != ModError::None) notice = finished->error;
            else InstallDownload(std::move(*finished));
            redraw = true;
        }
    }
    return redraw;
}

bool ModSession::HasUpdate(std::size_t index) const {
    return index < view.mods.size() && store.State().updates.contains(view.mods[index].record.folder);
}

void ModSession::OpenUpdate(std::size_t index) {
    if (index < view.mods.size()) store.OpenUpdate(view.mods[index].record.folder, view.mods[index].record.name);
}

bool ModSession::Download(std::size_t file) {
    auto request = store.Request(file);
    if (!request) return false;
    downloads.Enqueue(std::move(*request));
    return true;
}

void ModSession::Reload(ModsSnapshot& state) {
    const auto records = library.Load(Now());
    state.mods.clear();
    for (const auto& record : records)
        state.mods.push_back({record, InspectFolder(library.FolderPath(record.folder)), {}, {}});
    Analyze(state);
    state.free_space = library.FreeSpace();
    state.mods_bytes = 0;
    for (const auto& mod : state.mods) state.mods_bytes += mod.contents.bytes;
    state.loaded = true;
}

void ModSession::Analyze(ModsSnapshot& state) const {
    std::vector<ModContents> contents;
    std::vector<bool> enabled;
    for (const auto& mod : state.mods) {
        contents.push_back(mod.contents);
        enabled.push_back(mod.record.enabled);
    }
    auto conflicts = FindConflicts(contents, enabled);
    for (std::size_t i = 0; i < state.mods.size(); ++i) {
        state.mods[i].compat = CheckCompat(state.mods[i].contents, build_id);
        state.mods[i].conflicts = std::move(conflicts[i]);
    }
}

bool ModSession::Toggle(std::size_t index) {
    if (Busy() || index >= view.mods.size()) return true;
    auto& record = view.mods[index].record;
    record.enabled = !record.enabled;
    Analyze(view);
    changed = true;
    std::vector<ModRecord> records;
    for (const auto& mod : view.mods) records.push_back(mod.record);
    return library.Save(records);
}

bool ModSession::Move(std::size_t from, std::size_t to) {
    if (Busy() || from >= view.mods.size() || to >= view.mods.size() || from == to) return true;
    auto& mods = view.mods;
    if (from < to) std::rotate(mods.begin() + from, mods.begin() + from + 1, mods.begin() + to + 1);
    else std::rotate(mods.begin() + to, mods.begin() + from, mods.begin() + from + 1);
    Analyze(view);
    changed = true;
    std::vector<ModRecord> records;
    for (const auto& mod : mods) records.push_back(mod.record);
    return library.Save(records);
}

void ModSession::Remove(std::size_t index) {
    if (index >= view.mods.size()) return;
    Start(ModJob::Removing, [this, folder = view.mods[index].record.folder](ModsSnapshot& state, std::stop_token) {
        auto records = library.Load(Now());
        state.error = library.Remove(records, folder);
        if (state.error == ModError::None) LOG_INFO(Frontend, "Mods: removed {}", folder);
        Reload(state);
    });
}

void ModSession::Browse(std::string folder, std::wstring name) {
    Start(ModJob::Browsing, [folder = std::move(folder), name = std::move(name)](ModsSnapshot& state,
                                                                              std::stop_token stop) {
        const auto entries = IsStoragePath(folder)
                                  ? BrowseStorageFolder(folder, stop, IsArchiveName)
                                  : BrowseInternal(fs::path{std::u8string{folder.begin(), folder.end()}});
        state.files = FolderRows(entries);
        state.folder = folder;
        state.folder_name = name;
    });
}

void ModSession::Open(std::size_t row) {
    if (Busy() || row >= view.files.size()) return;
    const FileRow file = view.files[row];
    switch (file.kind) {
    case FileRow::Kind::AddFolder:
        folder_request = true;
        return;
    case FileRow::Kind::Folder:
        Browse(file.path, file.name);
        return;
    case FileRow::Kind::Archive:
        break;
    }
    Start(ModJob::Reading, [this, file](ModsSnapshot& state, std::stop_token stop) {
        state.archive = file.name;
        state.archive_volumes = file.volumes;
        state.candidates.clear();
        state.origin = {SourceKind::Archive, Utf8(Utf8Path(file.path).filename())};
        state.origin_name.clear();
        state.replace.clear();
        state.downloaded.clear();
        auto volumes = OpenVolumes(file.volumes);
        if (volumes.empty()) {
            state.error = ModError::Unavailable;
            return;
        }
        const auto listing = ListArchive(volumes, stop);
        if (listing.error != ArchiveError::None) {
            LOG_WARNING(Frontend, "Mods: cannot read {}: {}", Utf16ToUtf8(file.name), listing.detail);
            state.error = FromArchiveError(listing.error);
            return;
        }
        const auto archive_name = fs::path{std::u8string{file.path.begin(), file.path.end()}}.filename();
        auto plan = PlanLayout(listing.items, TitleFolder(title_id), Utf8(archive_name), RomFsTop());
        LOG_INFO(Frontend, "Mods: {} ({}) has {} files, {} mods", Utf16ToUtf8(file.name), listing.format,
                 listing.items.size(), plan.candidates.size());
        if (plan.candidates.empty()) state.error = ModError::NotAMod;
        state.candidates = std::move(plan.candidates);
    });
}

void ModSession::Up() {
    if (Busy() || view.folder.empty()) return;
    // Up one level inside the source; from a source's top, back to the sources.
    if (const auto storage = ParseStoragePath(view.folder)) {
        if (!storage->relative.empty()) {
            const auto slash = storage->relative.rfind('/');
            const auto parent = slash == std::string::npos ? std::string{} : storage->relative.substr(0, slash);
            Browse(MakeStoragePath(storage->token, parent), view.folder_name);
            return;
        }
    } else {
        const fs::path folder{std::u8string{view.folder.begin(), view.folder.end()}};
        if (folder != InboxFolder()) {
            Browse(Utf8(folder.parent_path()), view.folder_name);
            return;
        }
    }
    Start(ModJob::Browsing, [](ModsSnapshot& state, std::stop_token) { ShowSources(state); });
}

void ModSession::Install(const std::vector<bool>& chosen) {
    std::vector<ModCandidate> picked;
    for (std::size_t i = 0; i < view.candidates.size() && i < chosen.size(); ++i)
        if (chosen[i]) picked.push_back(view.candidates[i]);
    if (picked.empty()) return;
    Start(ModJob::Installing, [this, picked = std::move(picked)](ModsSnapshot& state, std::stop_token stop) {
        InstallPicked(state, picked, stop);
    });
}

void ModSession::InstallPicked(ModsSnapshot& state, const std::vector<ModCandidate>& picked,
                               std::stop_token stop) {
    state.candidates.clear();
    state.updated = false;
    {
        auto volumes = OpenVolumes(state.archive_volumes);
        if (volumes.empty()) {
            state.error = ModError::Unavailable;
        } else {
            auto records = library.Load(Now());
            std::string detail;
            state.error = library.Install(volumes, picked, state.origin, records, Now(), stop,
                                          [this](std::uint64_t d, std::uint64_t t) { done = d; total = t; },
                                          state.installed, detail);
            if (state.error != ModError::None)
                LOG_WARNING(Frontend, "Mods: install stopped after {} of {}: {}", state.installed,
                            picked.size(), detail);
            else
                LOG_INFO(Frontend, "Mods: installed {} from {}", picked.size(), state.origin.archive);
            // An update takes the old mod's place with the first mod it installed (the newest
            // installs go on top, so that one sits below the others it brought).
            if (state.error == ModError::None && !state.replace.empty() && state.installed > 0) {
                state.error = library.Replace(records, state.replace, records[state.installed - 1].folder);
                state.updated = state.error == ModError::None;
                LOG_INFO(Frontend, "Mods: {} updated", state.replace);
            }
        }
    } // The archive closes before it may be deleted.
    if (!state.downloaded.empty() && !KeepDownload(state.error)) {
        std::error_code ec;
        fs::remove(Utf8Path(state.downloaded), ec);
    }
    state.downloaded.clear();
    state.replace.clear();
    state.origin_name.clear();
    Reload(state);
}

const std::vector<std::string>& ModSession::RomFsTop() {
    if (!romfs_top)
        romfs_top = GameRomFsTop(game_path, DefaultModPaths().data / "romfs" / (TitleFolder(title_id) + ".txt"));
    return *romfs_top;
}

void ModSession::InstallDownload(FinishedDownload download) {
    Start(ModJob::Installing, [this, download = std::move(download)](ModsSnapshot& state, std::stop_token stop) {
        const auto& request = download.request;
        state.archive = Utf8ToUtf16(request.title);
        state.archive_volumes = {Utf8(download.file)};
        state.candidates.clear();
        state.origin = request.source;
        state.origin_name = request.name;
        state.replace = request.replace;
        state.downloaded = Utf8(download.file);
        state.installed = 0;
        const auto forget = [&] {
            std::error_code ec;
            fs::remove(download.file, ec);
            state.downloaded.clear();
            state.replace.clear();
        };
        ArchiveListing listing;
        {
            auto volumes = OpenVolumes(state.archive_volumes);
            if (volumes.empty()) {
                state.error = ModError::Unavailable;
                return;
            }
            listing = ListArchive(volumes, stop);
        }
        if (listing.error != ArchiveError::None) {
            LOG_WARNING(Frontend, "Mods: cannot read the download {}: {}", request.key, listing.detail);
            state.error = FromArchiveError(listing.error);
            if (!KeepDownload(state.error)) forget();
            return;
        }
        auto plan = PlanLayout(listing.items, TitleFolder(title_id), request.source.archive, RomFsTop());
        LOG_INFO(Frontend, "Mods: download {} has {} files, {} mods", request.key, listing.items.size(),
                 plan.candidates.size());
        if (plan.candidates.empty()) {
            for (std::size_t i = 0; i < listing.items.size() && i < 12; ++i)
                LOG_INFO(Frontend, "Mods:   {}", listing.items[i].path);
            state.error = ModError::NotAMod;
            forget();
            return;
        }
        NameAfter(plan.candidates, request.name);
        if (plan.candidates.size() == 1) {
            InstallPicked(state, plan.candidates, stop);
            return;
        }
        // Several mods in it: the player picks, as with an archive from a folder.
        state.candidates = std::move(plan.candidates);
    });
}

void ModSession::Dismiss() {
    if (Busy()) return;
    if (!view.downloaded.empty()) {
        std::error_code ec;
        fs::remove(Utf8Path(view.downloaded), ec);
    }
    view.candidates.clear();
    view.downloaded.clear();
    view.replace.clear();
    view.origin_name.clear();
}

void ModSession::Cancel() {
    if (Busy()) worker.request_stop();
}

void ModSession::AddFolder(const winrt::Windows::Storage::StorageFolder& folder) {
    Start(ModJob::Browsing, [folder](ModsSnapshot& state, std::stop_token stop) {
        const auto path = MakeStoragePath(RememberContentFolder(folder));
        state.files = FolderRows(BrowseStorageFolder(path, stop, IsArchiveName));
        state.folder = path;
        state.folder_name = std::wstring{folder.Name()};
    });
}

} // namespace EdenXbox::Mods
