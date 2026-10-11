// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#include "eden_uwp/content/content_service.h"
#include <mutex>
#include <fstream>
#include "eden_uwp/uwp_rom_storage.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "core/file_sys/common_funcs.h"
#include "core/file_sys/content_archive.h"
#include "core/file_sys/control_metadata.h"
#include "core/file_sys/romfs.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/loader/nso.h"
#include "core/hle/api_version.h"
#include "core/core.h"
#include "core/hle/service/filesystem/filesystem.h"

namespace EdenXbox::Content {
namespace {
std::mutex launch_mutex;
std::string allow_missing;
std::string Utf8(const std::filesystem::path& path) {
    const auto s = path.u8string(); return {s.begin(), s.end()};
}
std::uint32_t BaseVersion(FileSys::ManualContentProvider& provider, u64 app) {
    auto meta = provider.GetEntry(app, FileSys::ContentRecordType::Meta);
    if (!meta) return 0;
    for (const auto& dir : meta->GetSubdirectories()) for (const auto& file : dir->GetFiles()) {
        if (file->GetExtension() != "cnmt") continue;
        FileSys::CNMT cnmt{file};
        if (cnmt.IsValid() && cnmt.GetTitleID() == app) return cnmt.GetTitleVersion();
    }
    return 0;
}
} // namespace
std::uint32_t SupportedSystemVersion() {
    // CNMT uses HOS title-version encoding, not Atmosphere's target-firmware encoding.
    return (u32{HLE::ApiVersion::HOS_VERSION_MAJOR} << 26) |
           (u32{HLE::ApiVersion::HOS_VERSION_MINOR} << 20) |
           (u32{HLE::ApiVersion::HOS_VERSION_MICRO} << 16);
}
std::uint32_t ReportedSystemVersion(Core::System& system) {
    const auto* cache = system.GetFileSystemController().GetSystemNANDContents();
    return cache ? cache->GetEntryVersion(0x0100000000000809ULL).value_or(SupportedSystemVersion()) : SupportedSystemVersion();
}
std::uint32_t PackageBaseVersion(FileSys::VfsFilesystem& vfs, const std::string& path, std::uint64_t application) {
    FileSys::ManualContentProvider provider;
    const auto file = vfs.OpenFile(path, FileSys::OpenMode::Read);
    return file && provider.AddEntriesFromContainer(file) ? BaseVersion(provider, application) : 0;
}
ContentStore DefaultStore() {
    return ContentStore{Common::FS::GetEdenPath(Common::FS::EdenPath::NANDDir).parent_path() / "content"};
}
std::string SourcePath(const ContentStore& store, const Revision& revision) {
    return revision.internal ? Utf8(store.Object(revision.id)) : revision.source;
}
void AllowMissingForNextLaunch(std::string path) {
    std::scoped_lock lock{launch_mutex}; allow_missing = std::move(path);
}
bool ConsumeAllowMissing(const std::string& path) {
    std::scoped_lock lock{launch_mutex};
    const bool allowed = allow_missing == path; allow_missing.clear(); return allowed;
}
Error ExecuteImport(ContentStore& store, Catalog& catalog, FileSys::VfsFilesystem& vfs,
                    InspectedContent& content, bool copy_internal, bool have_base,
                    std::stop_token stop, Progress progress, std::uint32_t base_version) {
    if (content.error != Error::None) return content.error;
    const auto action = PlanImport(catalog.revisions, content.revision);
    if (action == ImportAction::Duplicate) return Error::None;
    if (action == ImportAction::Conflict) return Error::Conflict;
    if (auto error = VerifyContent(content, stop, progress); error != Error::None) return error;
    Revision added = content.revision;
    added.source_stamp = StorageFileStamp(added.source);
    if (copy_internal) {
        const auto temporary = store.Temporary(added.id), final = store.Object(added.id);
        if (temporary.empty() || final.empty()) return Error::Invalid;
        std::error_code ec;
        std::filesystem::create_directories(store.Root(), ec);
        const auto space = std::filesystem::space(store.Root(), ec);
        if (ec || space.available < added.bytes + 16 * 1024 * 1024) return Error::Space;
        // Only generated, validated id paths are removable. Never the source path.
        if (std::filesystem::exists(final)) return Error::Conflict;
        std::filesystem::remove_all(temporary, ec);
        std::filesystem::create_directories(temporary, ec);
        if (ec) return Error::Io;
        u64 done{};
        Error copy_error = Error::Io;
        const auto copy = [&](const FileSys::VirtualFile& source, const FileSys::VirtualFile& target, size_t) {
            if (!source || !target || !target->Resize(source->GetSize())) return false;
            std::vector<u8> buffer(1024 * 1024);
            for (size_t offset = 0; offset < source->GetSize();) {
                if (stop.stop_requested()) { copy_error = Error::Cancelled; return false; }
                const auto bytes = std::min(buffer.size(), source->GetSize() - offset);
                if (source->Read(buffer.data(), bytes, offset) != bytes || target->Write(buffer.data(), bytes, offset) != bytes)
                    return false;
                offset += bytes; done += bytes;
                if (progress) progress(done, added.bytes);
            }
            const auto real = std::static_pointer_cast<FileSys::RealVfsFile>(target);
            return real && real->Commit();
        };
        {
            FileSys::RealVfsFilesystem disk;
            auto dir = disk.OpenDirectory(Utf8(temporary), FileSys::OpenMode::ReadWrite);
            if (!dir) return Error::Io;
            FileSys::RegisteredCache cache{dir};
            std::vector<FileSys::VirtualFile> files;
            for (const auto& item : content.files) files.push_back(item.file);
            if (cache.InstallContentSet(*content.cnmt, content.meta, files, copy) != FileSys::InstallResult::Success)
                return copy_error;
            for (const auto& ticket : content.tickets) {
                // Ticket names come from RightsId, never from a USB filename.
                const auto value = Core::Crypto::Ticket::Read(ticket);
                if (!value.IsValid()) return Error::Invalid;
                std::string name;
                constexpr char digits[] = "0123456789abcdef";
                for (auto b : value.GetData().rights_id) { name += digits[b >> 4]; name += digits[b & 15]; }
                if (!copy(ticket, dir->CreateFile(name + ".tik"), 0)) return copy_error;
            }
        }
        // Reread the destination through the same readers and hash all its referenced payloads.
        auto checked = InspectPackage(vfs, Utf8(temporary), true, stop, progress);
        const auto found = std::find_if(checked.contents.begin(), checked.contents.end(),
            [&](const auto& c) { return c.revision.id == added.id && c.error == Error::None; });
        if (checked.error != Error::None || found == checked.contents.end()) return Error::HashMismatch;
        checked.contents.clear();
        if (stop.stop_requested()) return Error::Cancelled;
        std::filesystem::create_directories(final.parent_path(), ec);
        std::filesystem::rename(temporary, final, ec);
        if (ec) return Error::Io;
        added.internal = true;
        added.source.clear(); // The generated object id is the only internal location.
    }
    if (stop.stop_requested()) return Error::Cancelled;
    Catalog next = catalog;
    const auto effective = added.kind == Kind::Update ? added.version : EffectiveVersion(next.revisions, added.application, base_version);
    ActivateNew(next.revisions, added, have_base && RequirementsSatisfied(added, effective, SupportedSystemVersion()));
    if (std::find(next.managed_applications.begin(), next.managed_applications.end(), added.application) == next.managed_applications.end())
        next.managed_applications.push_back(added.application);
    next.revisions.push_back(std::move(added));
    if (auto error = store.Save(next); error != Error::None) return error;
    catalog = std::move(next);
    store.Cleanup(catalog);
    LOG_INFO(Frontend, "Content: imported title={:016X} version={} storage={} verified=true",
             content.revision.title, content.revision.version, copy_internal ? "internal" : "reference");
    return Error::None;
}

LaunchResolution ResolveLaunchContent(FileSys::VfsFilesystem& vfs, const std::string& base_path,
                                     FileSys::ManualContentProvider& provider,
                                     bool omit_unavailable, std::stop_token stop, std::uint32_t system_version) {
    using namespace FileSys;
    LaunchResolution result;
    ManualContentProvider base;
    const auto file = vfs.OpenFile(base_path, OpenMode::Read);
    if (!file || !base.AddEntriesFromContainer(file)) return result; // NRO/deconstructed loader retains its path.
    const auto programs = base.ListEntriesFilter(TitleType::Application, ContentRecordType::Program, {});
    if (programs.empty()) return result;
    result.application = GetBaseTitleID(programs.front().title_id);
    result.version = BaseVersion(base, result.application);
    const auto store = DefaultStore(); Catalog catalog;
    result.error = store.Load(catalog);
    if (result.error != Error::None) return result;
    result.managed = std::find(catalog.managed_applications.begin(), catalog.managed_applications.end(), result.application) != catalog.managed_applications.end() || std::any_of(catalog.revisions.begin(), catalog.revisions.end(),
        [&](const auto& r) { return r.application == result.application; });
    provider.ClearAllEntries();
    if (!result.managed) provider.AddEntriesFromContainer(file);
    else {
        for (const auto& entry : base.ListEntriesFilter(TitleType::Application, {}, {}))
            provider.AddEntry(TitleType::Application, entry.type, entry.title_id, base.GetEntryRaw(entry.title_id, entry.type));
        // Resolve update first; an omitted update cannot satisfy a DLC's minimum version.
        auto ordered = catalog.revisions;
        std::stable_sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) {
            return a.kind == Kind::Update && b.kind == Kind::Dlc;
        });
        for (const auto& r : ordered) {
            if (r.application != result.application || !r.enabled) continue;
            if (stop.stop_requested()) { result.error = Error::Cancelled; return result; }
            auto inspected = InspectPackage(vfs, SourcePath(store, r), false, stop);
            auto found = std::find_if(inspected.contents.begin(), inspected.contents.end(),
                [&](const auto& c) { return c.revision.id == r.id && c.error == Error::None &&
                    (r.internal || c.revision.source_size == r.source_size); });
            if (inspected.error != Error::None || found == inspected.contents.end() || !r.verified) {
                result.unavailable.push_back(r); continue;
            }
            if (!r.internal && (r.source_stamp == 0 || StorageFileStamp(r.source) != r.source_stamp)) {
                if (VerifyContent(*found, stop) != Error::None) { result.unavailable.push_back(r); continue; }
            }
            if (!RequirementsSatisfied(r, result.version, system_version)) {
                result.error = Error::Requirements; return result;
            }
            RegisterContent(provider, *found);
            if (r.kind == Kind::Update) result.version = r.version;
        }
        if (!result.unavailable.empty() && !omit_unavailable) {
            result.error = Error::Unavailable; return result;
        }
    }
    auto main_nca = provider.GetEntry(GetUpdateTitleID(programs.front().title_id), ContentRecordType::Program);
    const bool uses_update = main_nca && main_nca->GetExeFS();
    const auto control_title = uses_update ? GetUpdateTitleID(programs.front().title_id) : programs.front().title_id;
    const auto control_nca = provider.GetEntry(control_title, ContentRecordType::Control);
    if (control_nca && control_nca->GetRomFS()) {
        const auto directory = ExtractRomFS(control_nca->GetRomFS());
        const auto control = directory ? directory->GetFile("control.nacp") : nullptr;
        if (control && control->GetSize() >= sizeof(RawNACP)) result.display_version = NACP{control}.GetVersionString();
    }
    // A missing update Control must not label the updated executable with the base version.
    if (!uses_update) main_nca = provider.GetEntry(programs.front().title_id, ContentRecordType::Program);
    if (main_nca && main_nca->GetExeFS()) {
        const auto main = main_nca->GetExeFS()->GetFile("main");
        Loader::NSOHeader header{};
        if (main && main->ReadObject(&header) == sizeof(header))
            std::copy_n(header.build_id.begin(), result.build_id.size(), result.build_id.begin());
    }
    LOG_INFO(Frontend, "Content: launch application={:016X} version={} managed={} missing={}",
             result.application, result.version, result.managed, result.unavailable.size());
    return result;
}
} // namespace EdenXbox::Content
