// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#include "eden_uwp/content/content_session.h"
#include <winrt/base.h>
#include "eden_uwp/uwp_file_manager.h"
#include "eden_uwp/storage_path.h"
#include "core/file_sys/common_funcs.h"

namespace EdenXbox::Content {
ContentSession::ContentSession(std::uint64_t application_, std::string base_path_, Kind kind_,
        std::vector<std::uint64_t> known_bases_, std::jthread previous_metadata)
    : application(FileSys::GetBaseTitleID(application_)), base_path(std::move(base_path_)), kind(kind_),
      known_bases(std::move(known_bases_)), store(DefaultStore()), vfs(MakeUwpFilesystem()) {
    // Never join a metadata parser on the UI thread. It also uses the shared KeyManager.
    auto old = std::make_shared<std::jthread>(std::move(previous_metadata));
    old->request_stop();
    Start([this, old](ContentSnapshot& state, std::stop_token stop) {
        if (old->joinable()) old->join();
        state.error = store.Load(state.catalog);
        if (state.error != Error::None) return;
        store.Cleanup(state.catalog);
        if (!base_path.empty()) base_version = PackageBaseVersion(*vfs, base_path, application);
        // Discover embedded content once. Persist it too, so individual toggles have exact semantics.
        const bool first = application && std::find(state.catalog.managed_applications.begin(), state.catalog.managed_applications.end(), application) == state.catalog.managed_applications.end();
        if (!base_path.empty() && first) {
            auto embedded = InspectPackage(*vfs, base_path, false, stop);
            for (auto& item : embedded.contents) {
                if (stop.stop_requested()) { state.error = Error::Cancelled; break; }
                if (item.error != Error::None || PlanImport(state.catalog.revisions, item.revision) != ImportAction::Add) continue;
                const auto error = ExecuteImport(store, state.catalog, *vfs, item, false, true, stop,
                    [this](auto d, auto t) { done = d; total = t; }, base_version);
                if (error != Error::None) { state.error = error; break; }
            }
        }
        if (first && state.error == Error::None && !stop.stop_requested()) {
            if (std::find(state.catalog.managed_applications.begin(), state.catalog.managed_applications.end(), application) == state.catalog.managed_applications.end()) {
                state.catalog.managed_applications.push_back(application);
                state.error = store.Save(state.catalog);
            }
        }
        Reload(state, stop);
    });
}
ContentSession::~ContentSession() { worker.request_stop(); if (worker.joinable()) worker.join(); }
void ContentSession::Start(Job job) {
    if (Busy()) return;
    done = 0; total = 0;
    auto snapshot = view;
    snapshot.error = Error::None;
    std::packaged_task<ContentSnapshot(std::stop_token)> task{
        [job = std::move(job), state = std::move(snapshot)](std::stop_token stop) mutable {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            struct Apartment { ~Apartment() { winrt::uninit_apartment(); } } apartment;
            try { job(state, stop); }
            catch (const winrt::hresult_error&) { state.error = Error::Unavailable; }
            catch (const std::exception&) { state.error = Error::Io; }
            return state;
        }};
    pending = task.get_future(); worker = std::jthread(std::move(task));
}
bool ContentSession::Poll() {
    if (!Busy() || pending.wait_for(std::chrono::seconds{0}) != std::future_status::ready) return false;
    const auto previous_generation = view.catalog.generation;
    try { view = pending.get(); }
    catch (...) { view.error = Error::Io; }
    changed |= view.catalog.generation != previous_generation;
    if (worker.joinable()) worker.join();
    if (view.selected >= Count()) view.selected = Count() ? Count() - 1 : 0;
    return true;
}
void ContentSession::Reload(ContentSnapshot& state, std::stop_token stop) {
    if (const auto error = store.Load(state.catalog); error != Error::None) { state.error = error; return; }
    state.rows.clear(); state.row_errors.clear();
    for (const auto& r : state.catalog.revisions) {
        if (r.kind != kind || (application && r.application != application)) continue;
        state.rows.push_back(r);
        if (stop.stop_requested()) { state.row_errors.push_back(Error::Cancelled); continue; }
        const auto check = InspectPackage(*vfs, SourcePath(store, r), false, stop);
        const auto found = std::find_if(check.contents.begin(), check.contents.end(),
            [&](const auto& c) { return c.revision.id == r.id; });
        auto error = check.error != Error::None ? check.error : found == check.contents.end() ? Error::Invalid : found->error;
        const auto effective = r.kind == Kind::Update ? r.version : EffectiveVersion(state.catalog.revisions, r.application, base_version);
        if (error == Error::None && !RequirementsSatisfied(r, effective, SupportedSystemVersion())) error = Error::Requirements;
        state.row_errors.push_back(error);
    }
    std::error_code ec; const auto space = std::filesystem::space(store.Root().parent_path(), ec);
    state.available = ec ? 0 : space.available;
}
void ContentSession::Sources() {
    Start([](ContentSnapshot& state, std::stop_token) {
        state.browser.clear();
        state.browser = ContentFolderSources();
        state.screen = Screen::Sources; state.selected = 0;
    });
}
void ContentSession::Browse(std::string folder) {
    Start([folder = std::move(folder)](ContentSnapshot& state, std::stop_token stop) {
        state.browser = BrowseStorageFolder(folder, stop);
        state.folder = folder; state.screen = Screen::Browser; state.selected = 0;
    });
}
void ContentSession::Authorized(std::string identity) { Browse(std::move(identity)); }
void ContentSession::AuthorizeFolder(const winrt::Windows::Storage::StorageFolder& folder) {
    Start([folder](auto& state, auto stop) {
        const auto token = RememberContentFolder(folder);
        state.folder = MakeStoragePath(token);
        state.browser = BrowseStorageFolder(state.folder, stop);
        state.screen = Screen::Browser; state.selected = 0;
    });
}
std::size_t ContentSession::Count() const {
    switch (view.screen) {
    case Screen::Manager: return view.rows.size() + 1;
    case Screen::Sources: return view.browser.size() + 1;
    case Screen::Storage: return 2;
    case Screen::Result: return view.completed.size() + 1;
    case Screen::Browser: return view.browser.size() + 1;
    case Screen::Preview: return view.preview.size() + 1;
    case Screen::Actions: return 4;
    default: return 1;
    }
}
bool ContentSession::Handle(Input input) {
    if (Busy()) { if (input == Input::Back) worker.request_stop(); return false; }
    const auto count = Count();
    if (input == Input::Up || input == Input::Down) {
        if (count) view.selected = (view.selected + (input == Input::Up ? count - 1 : 1)) % count;
        return false;
    }
    if (input == Input::Storage) return false; // Destination is an explicit step, never a hidden toggle.
    if (input == Input::Back) {
        switch (view.screen) {
        case Screen::Manager: return true;
        case Screen::Storage: view.screen = Screen::Manager; break;
        case Screen::Sources: view.screen = relocating ? Screen::Manager : Screen::Storage; break;
        case Screen::Result: view.screen = Screen::Manager; break;
        case Screen::Browser: {
            const auto path = ParseStoragePath(view.folder);
            if (!path || path->relative.empty()) Sources();
            else {
                const auto slash = path->relative.rfind('/');
                Browse(MakeStoragePath(path->token, slash == std::string::npos ? "" : path->relative.substr(0, slash)));
            }
            break;
        }
        case Screen::Preview: Browse(view.folder); break;
        case Screen::ConfirmImport: view.screen = Screen::Preview; break;
        default: view.screen = Screen::Manager; break;
        }
        view.selected = 0; return false;
    }
    if (view.screen == Screen::Manager) {
        if (input == Input::Import || (input == Input::Confirm && view.selected == 0)) {
            relocating = false; view.selected_files.clear(); view.completed.clear();
            view.screen = Screen::Storage; view.selected = copy_internal ? 1 : 0;
        }
        else if (input == Input::Refresh) Start([this](auto& state, auto stop) { Reload(state, stop); });
        else if (input == Input::Confirm && view.selected > 0 && view.selected <= view.rows.size()) {
            view.action_id = view.rows[view.selected - 1].id; view.screen = Screen::Actions; view.selected = 0;
        }
    } else if (view.screen == Screen::Storage && input == Input::Confirm) {
        copy_internal = view.selected == 1; Sources();
    } else if (view.screen == Screen::Result && input == Input::Confirm) {
        view.screen = Screen::Manager; view.selected = 0;
    } else if (view.screen == Screen::Sources && input == Input::Confirm) {
        if (view.selected == 0) authorize = true;
        else if (view.selected <= view.browser.size()) Browse(view.browser[view.selected - 1].path);
    } else if (view.screen == Screen::Browser) {
        if (input == Input::Refresh && view.selected < view.browser.size() && !view.browser[view.selected].folder) {
            const auto& path = view.browser[view.selected].path;
            auto it = std::find(view.selected_files.begin(), view.selected_files.end(), path);
            if (it != view.selected_files.end()) view.selected_files.erase(it);
            else if (view.selected_files.size() < 64) view.selected_files.push_back(path);
            return false;
        }
        if (input == Input::Confirm && view.selected == view.browser.size()) input = Input::Import;
        if (input == Input::Confirm && view.selected < view.browser.size()) {
            const auto item = view.browser[view.selected];
            if (item.folder) Browse(item.path);
            else {
                auto& list = view.selected_files;
                auto it = std::find(list.begin(), list.end(), item.path);
                if (relocating) {
                    relocating = false;
                    Start([this, path = item.path](auto& state, auto stop) {
                        auto it = std::find_if(state.catalog.revisions.begin(), state.catalog.revisions.end(), [&](const auto& r) { return r.id == state.action_id; });
                        if (it == state.catalog.revisions.end() || it->internal) { state.error = Error::Invalid; return; }
                        auto check = InspectPackage(*vfs, path, true, stop, [this](auto d, auto t) { done=d; total=t; });
                        const auto found = std::find_if(check.contents.begin(), check.contents.end(), [&](const auto& c) { return c.revision.id == it->id && c.error == Error::None; });
                        if (found == check.contents.end()) { state.error = check.error == Error::None ? Error::HashMismatch : check.error; return; }
                        it->source = path; it->source_size = found->revision.source_size; it->source_stamp = StorageFileStamp(path);
                        state.error = store.Save(state.catalog); state.screen = Screen::Manager; Reload(state, stop);
                    });
                    return false;
                }
                if (it == list.end()) {
                    if (list.size() >= 64) return false;
                    list.push_back(item.path);
                }
                // The normal single-file flow continues with A; Y optionally builds a batch.
                return Handle(Input::Import);
            }
        } else if (input == Input::Import && !view.selected_files.empty()) {
            Start([this](ContentSnapshot& state, std::stop_token stop) {
                state.preview.clear();
                for (const auto& path : state.selected_files) {
                    if (stop.stop_requested()) { state.error = Error::Cancelled; return; }
                    auto inspected = InspectPackage(*vfs, path, false, stop);
                    if (inspected.error != Error::None) { state.error = inspected.error; continue; }
                    for (auto& content : inspected.contents) {
                        if (content.revision.kind != kind) continue;
                        if (application && content.revision.application != application) content.error = Error::WrongGame;
                        const auto plan = PlanImport(state.catalog.revisions, content.revision);
                        if (plan == ImportAction::Conflict) content.error = Error::Conflict;
                        state.preview.push_back(std::move(content));
                    }
                }
                state.checked.clear();
                for (const auto& item : state.preview) state.checked.push_back(item.error == Error::None);
                state.screen = Screen::Preview; state.selected = state.preview.size();
            });
        }
    } else if (view.screen == Screen::Preview) {
        if (input == Input::Confirm && view.selected == view.preview.size()) input = Input::Import;
        if (input == Input::Confirm && view.selected < view.checked.size() && view.preview[view.selected].error == Error::None)
            view.checked[view.selected] = !view.checked[view.selected];
        if (input == Input::Import && std::any_of(view.checked.begin(), view.checked.end(), [](bool b) { return b; })) {
            view.screen = Screen::ConfirmImport; view.selected = 0;
        }
    } else if (view.screen == Screen::ConfirmImport && input == Input::Confirm) {
        const bool destination_internal = copy_internal;
        Start([this, destination_internal](ContentSnapshot& state, std::stop_token stop) {
            state.completed.clear(); state.added = state.already_present = 0;
            state.screen = Screen::Result;
            for (size_t i = 0; i < state.preview.size(); ++i) {
                if (!state.checked[i]) continue;
                auto& content = state.preview[i];
                const bool have_base = std::find(known_bases.begin(), known_bases.end(), content.revision.application) != known_bases.end();
                const auto plan = PlanImport(state.catalog.revisions, content.revision);
                const auto error = ExecuteImport(store, state.catalog, *vfs, content, destination_internal, have_base, stop,
                    [this](auto d, auto t) { done = d; total = t; }, base_version);
                if (error != Error::None) { state.error = error; break; }
                if (plan == ImportAction::Duplicate) ++state.already_present; else ++state.added;
                const auto saved = std::find_if(state.catalog.revisions.begin(), state.catalog.revisions.end(),
                    [&](const auto& r) { return r.id == content.revision.id; });
                if (saved != state.catalog.revisions.end()) state.completed.push_back(*saved);
            }
            state.preview.clear(); state.checked.clear(); state.selected_files.clear();
            state.screen = Screen::Result; state.selected = 0; Reload(state, stop);
            // A later update in this batch may have deactivated an earlier one.
            // Report the final persisted selection, not the per-item intermediate state.
            for (auto& item : state.completed) {
                const auto saved = std::find_if(state.catalog.revisions.begin(), state.catalog.revisions.end(),
                    [&](const auto& r) { return r.id == item.id; });
                if (saved != state.catalog.revisions.end()) item = *saved;
            }
        });
    } else if (view.screen == Screen::Actions && input == Input::Confirm) {
        const auto action = view.selected;
        if (action == 3) { view.screen = Screen::ConfirmRemove; return false; }
        if (action == 2) { relocating = true; Sources(); return false; }
        Start([this, action](ContentSnapshot& state, std::stop_token stop) {
            auto it = std::find_if(state.catalog.revisions.begin(), state.catalog.revisions.end(),
                [&](const auto& r) { return r.id == state.action_id; });
            if (it == state.catalog.revisions.end()) { state.error = Error::Invalid; return; }
            if (action == 0) {
                if (!it->enabled) {
                    const auto effective = it->kind == Kind::Update ? it->version : EffectiveVersion(state.catalog.revisions, it->application, base_version);
                    if (!RequirementsSatisfied(*it, effective, SupportedSystemVersion())) { state.error = Error::Requirements; return; }
                    if (std::find(known_bases.begin(), known_bases.end(), it->application) == known_bases.end()) { state.error = Error::WrongGame; return; }
                    auto check = InspectPackage(*vfs, SourcePath(store, *it), true, stop, [this](auto d, auto t) { done=d; total=t; });
                    const auto found = std::find_if(check.contents.begin(), check.contents.end(), [&](const auto& c) { return c.revision.id == it->id && c.error == Error::None; });
                    if (found == check.contents.end()) { state.error = check.error == Error::None ? Error::HashMismatch : check.error; return; }
                }
                state.error = Select(state.catalog.revisions, it->id, !it->enabled);
            } else {
                auto check = InspectPackage(*vfs, SourcePath(store, *it), false, stop);
                auto found = std::find_if(check.contents.begin(), check.contents.end(), [&](const auto& c) { return c.revision.id == it->id; });
                state.error = check.error != Error::None ? check.error : found == check.contents.end() ? Error::Invalid :
                    VerifyContent(*found, stop, [this](auto d, auto t) { done = d; total = t; });
                if (state.error == Error::None) { it->verified = true; it->source_stamp = StorageFileStamp(it->source); }
            }
            if (state.error == Error::None) state.error = store.Save(state.catalog);
            state.screen = Screen::Manager; state.selected = 0; Reload(state, stop);
        });
    } else if (view.screen == Screen::ConfirmRemove && input == Input::Confirm) {
        Start([this](ContentSnapshot& state, std::stop_token stop) {
            state.error = store.Remove(state.catalog, state.action_id);
            state.screen = Screen::Manager; state.selected = 0; Reload(state, stop);
        });
    }
    return false;
}
} // namespace EdenXbox::Content
