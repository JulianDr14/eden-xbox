// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// One game's mods page at work. What touches the disk or a USB drive (reading the mod folders,
// listing a folder, reading an archive, installing, uninstalling) runs as one job at a time on a
// worker thread, and the page polls for its result each frame; turning a mod on or off and moving
// it only rewrite the small manifest, so they apply at once.
//
// The store (mod_store.h) and the downloads (mod_downloads.h) run beside those jobs, each on its
// own worker: the player browses while files download, and each finished download is installed
// as soon as no other job runs.

#include <atomic>
#include <cstdint>
#include <functional>
#include <future>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <winrt/Windows.Storage.h>

#include "eden_uwp/mods/mod_analysis.h"
#include "eden_uwp/mods/mod_downloads.h"
#include "eden_uwp/mods/mod_installer.h"
#include "eden_uwp/mods/mod_store.h"

namespace EdenXbox::Mods {

/// One installed mod as the page shows it.
struct InstalledMod {
    ModRecord record;
    ModContents contents;
    Compat compat{};
    std::vector<Conflict> conflicts;
};

/// A row of the Files tab.
struct FileRow {
    enum class Kind : std::uint8_t { AddFolder, Folder, Archive };
    Kind kind{};
    std::wstring name;
    std::string path;                 ///< The folder, or the archive's first volume.
    std::vector<std::string> volumes; ///< An archive's volumes, in order.
};

enum class ModJob : std::uint8_t { None, Loading, Browsing, Reading, Installing, Removing };

struct ModsSnapshot {
    std::vector<InstalledMod> mods;
    std::vector<FileRow> files;
    std::string folder;       ///< The folder the Files list shows; empty for the sources.
    std::wstring folder_name;
    std::wstring archive;     ///< The archive read last.
    std::vector<std::string> archive_volumes;
    std::vector<ModCandidate> candidates; ///< Its mods.
    ModSource origin;         ///< What its mods record as their source.
    std::string origin_name;  ///< An online mod's name: it names the mods of its archive.
    std::string replace;      ///< The installed mod's folder an update replaces.
    std::string downloaded;   ///< A downloaded archive: deleted once installed or dismissed.
    ModError error{};         ///< The last job's outcome.
    std::size_t installed{};  ///< Mods the last install added.
    bool updated{};           ///< The last install replaced an older version of a mod.
    std::uint64_t free_space{};
    std::uint64_t mods_bytes{}; ///< What the installed mods take.
    bool loaded{};
};

class ModSession {
public:
    /// `build_id` is the running version's (16 upper-case hex digits), empty when unknown;
    /// `game_path` is its library launch path, read for its RomFS folders; `game_name` finds the
    /// game in the store; `show_sensitive` lists mods rated for adults.
    ModSession(std::uint64_t title_id, std::string build_id, std::string game_path, std::string game_name,
               bool show_sensitive);
    ~ModSession();
    ModSession(const ModSession&) = delete;
    ModSession& operator=(const ModSession&) = delete;

    /// Takes a finished job's result; returns which job it was (None when nothing finished).
    ModJob Poll();
    /// The store and the downloads: applies what they finished and starts installing a finished
    /// download when nothing else runs. True when the page is worth drawing again.
    bool Tick();
    /// A download that failed, to tell the player once.
    std::optional<ModError> TakeNotice() { return std::exchange(notice, std::nullopt); }

    [[nodiscard]] bool Busy() const { return pending.valid(); }
    [[nodiscard]] ModJob Running() const { return running; }
    [[nodiscard]] const ModsSnapshot& View() const { return view; }
    [[nodiscard]] std::uint64_t Done() const { return done.load(); }
    [[nodiscard]] std::uint64_t Total() const { return total.load(); }
    /// The mod list or the manifest changed this session: the library may show other counts.
    [[nodiscard]] bool Changed() const { return changed; }

    [[nodiscard]] const StoreState& Store() const { return store.State(); }
    [[nodiscard]] const ThumbnailCache& Pictures() const { return store.Pictures(); }
    [[nodiscard]] bool ShowsSensitive() const { return store.ShowsSensitive(); }
    [[nodiscard]] std::vector<DownloadStatus> Downloads() const { return downloads.Status(); }
    [[nodiscard]] bool HasUpdate(std::size_t index) const;

    /// Turns mod `index` on or off; false when the change could not be saved.
    bool Toggle(std::size_t index);
    /// Moves mod `from` to `to`; false when the change could not be saved.
    bool Move(std::size_t from, std::size_t to);
    void Remove(std::size_t index);

    void Open(std::size_t row);
    void Up();
    void Install(const std::vector<bool>& chosen);
    /// The player left an archive's variants without installing.
    void Dismiss();
    void Cancel();

    /// The player asked to add a folder: the host shows the folder picker, then calls back.
    [[nodiscard]] bool TakeFolderRequest() { return std::exchange(folder_request, false); }
    void AddFolder(const winrt::Windows::Storage::StorageFolder& folder);

    // The store.
    void OpenStore() { store.Open(); }
    void RetryStore() { store.Retry(); }
    void Want(std::size_t card) { store.Want(card); }
    void CycleSort() { store.CycleSort(); }
    void Search(std::string text) { store.Search(std::move(text)); }
    void SwitchSource() { store.SwitchSource(); }
    void FindGame(std::string name) { store.FindGame(std::move(name)); }
    void PickGame(std::size_t index) { store.PickGame(index); }
    void OpenCategories() { store.OpenCategories(); }
    [[nodiscard]] std::vector<CategoryRow> CategoryRows() const { return store.CategoryRows(); }
    void EnterCategory(std::size_t row) { store.EnterCategory(row); }
    void LeaveCategory() { store.LeaveCategory(); }
    void PickCategory(std::size_t row) { store.PickCategory(row); }
    void OpenDetail(std::size_t card) { store.OpenDetail(card); }
    void OpenUpdate(std::size_t index);
    void CloseDetail() { store.CloseDetail(); }
    /// Queues file `index` of the open detail; false when it is not safe to download.
    bool Download(std::size_t file);
    void CancelDownloads() { downloads.CancelAll(); }

private:
    using Job = std::function<void(ModsSnapshot&, std::stop_token)>;
    void Start(ModJob kind, Job job);
    void Reload(ModsSnapshot& state);
    void Browse(std::string folder, std::wstring name);
    void Analyze(ModsSnapshot& state) const;
    void InstallDownload(FinishedDownload download);
    void InstallPicked(ModsSnapshot& state, const std::vector<ModCandidate>& picked, std::stop_token stop);
    /// The game's top RomFS folders, read on the first archive that needs them (jobs only).
    const std::vector<std::string>& RomFsTop();

    std::uint64_t title_id;
    std::string build_id;
    std::string game_path;
    std::optional<std::vector<std::string>> romfs_top;
    ModLibrary library;
    ModsSnapshot view;
    ModJob running{ModJob::None};
    std::atomic<std::uint64_t> done{}, total{};
    std::future<ModsSnapshot> pending;
    std::jthread worker;
    bool folder_request{};
    bool changed{};
    bool checked_updates{};
    std::optional<ModError> notice;
    ModStore store;
    ModDownloads downloads;
};

} // namespace EdenXbox::Mods
