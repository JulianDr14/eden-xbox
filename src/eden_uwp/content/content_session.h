// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <atomic>
#include <utility>
#include <future>
#include <thread>
#include "eden_uwp/content/content_service.h"
#include "eden_uwp/uwp_rom_storage.h"

namespace EdenXbox::Content {
enum class Screen { Manager, Storage, Sources, Browser, Preview, Actions, ConfirmRemove, ConfirmImport, Result };
enum class Input { Up, Down, Confirm, Back, Import, Storage, Refresh };
struct ContentSnapshot {
    Catalog catalog;
    std::vector<Revision> completed;
    std::size_t added{}, already_present{};
    std::vector<Revision> rows;
    std::vector<Error> row_errors;
    std::vector<StorageBrowserEntry> browser;
    std::vector<InspectedContent> preview;
    std::vector<bool> checked;
    std::vector<std::string> selected_files;
    std::string folder, action_id;
    Screen screen{};
    std::size_t selected{};
    Error error{};
    std::uint64_t available{};
};
class ContentSession {
public:
    ContentSession(std::uint64_t application, std::string base_path, Kind kind,
                   std::vector<std::uint64_t> known_bases, std::jthread previous_metadata = {});
    ~ContentSession();
    bool Poll();
    // True asks the containing page to close. A busy job is cancelled first.
    bool Handle(Input input);
    void Authorized(std::string identity);
    void AuthorizeFolder(const winrt::Windows::Storage::StorageFolder& folder);
    bool TakeAuthorizeRequest() { return std::exchange(authorize, false); }
    bool Busy() const { return pending.valid(); }
    bool Changed() const { return changed; }
    bool CopyInternal() const { return copy_internal; }
    Kind Category() const { return kind; }
    const ContentSnapshot& View() const { return view; }
    std::uint64_t Done() const { return done.load(); }
    std::uint64_t Total() const { return total.load(); }
private:
    using Job = std::function<void(ContentSnapshot&, std::stop_token)>;
    void Start(Job job);
    void Reload(ContentSnapshot& state, std::stop_token stop);
    void Sources();
    void Browse(std::string folder);
    std::size_t Count() const;
    std::uint64_t application;
    std::string base_path;
    std::uint32_t base_version{};
    Kind kind;
    std::vector<std::uint64_t> known_bases;
    ContentStore store;
    FileSys::VirtualFilesystem vfs;
    ContentSnapshot view;
    std::atomic<std::uint64_t> done{}, total{};
    bool copy_internal{}, authorize{}, changed{}, relocating{};
    std::future<ContentSnapshot> pending;
    std::jthread worker;
};
} // namespace EdenXbox::Content
