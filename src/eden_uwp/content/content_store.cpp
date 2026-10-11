// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#include "eden_uwp/content/content_store.h"
#include <fstream>
#include <set>
#include <nlohmann/json.hpp>
#ifdef _WIN32
#include <windows.h>
#include <fileapifromapp.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace EdenXbox::Content {
namespace {
using Json = nlohmann::json;
constexpr std::uintmax_t MaxCatalogBytes = 8 * 1024 * 1024;
bool Replace(const std::filesystem::path& temporary, const std::filesystem::path& destination) {
#ifdef _WIN32
    if (GetFileAttributesW(destination.c_str()) == INVALID_FILE_ATTRIBUTES) {
        if (GetLastError() != ERROR_FILE_NOT_FOUND && GetLastError() != ERROR_PATH_NOT_FOUND) return false;
        return MoveFileFromAppW(temporary.c_str(), destination.c_str()) != 0;
    }
    return ReplaceFileFromAppW(destination.c_str(), temporary.c_str(), nullptr, 0, nullptr, nullptr) != 0;
#else
    std::error_code ec; std::filesystem::rename(temporary, destination, ec);
    if (ec) return false;
    const int fd = open(destination.parent_path().c_str(), O_RDONLY | O_DIRECTORY);
    if (fd < 0) return false;
    const bool ok = fsync(fd) == 0; close(fd); return ok;
#endif
}
bool Write(const std::filesystem::path& path, const std::string& data) {
    if (data.size() > MaxCatalogBytes) return false;
    {
        std::ofstream out{path, std::ios::binary | std::ios::trunc};
        if (!out || !out.write(data.data(), data.size()).flush()) return false;
    }
#ifdef _WIN32
    const HANDLE handle = CreateFile2FromAppW(path.c_str(), GENERIC_WRITE, 0, OPEN_EXISTING, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    const bool ok = FlushFileBuffers(handle) != 0; CloseHandle(handle); return ok;
#else
    const int fd = open(path.c_str(), O_WRONLY);
    if (fd < 0) return false;
    const bool ok = fsync(fd) == 0; close(fd); return ok;
#endif
}
Json Encode(const Catalog& catalog) {
    Json entries = Json::array();
    for (const auto& r : catalog.revisions) entries.push_back({
        {"id", r.id}, {"source", r.source}, {"name", r.name}, {"display_version", r.display_version},
        {"application", r.application}, {"title", r.title}, {"version", r.version},
        {"required_application", r.required_application}, {"required_system", r.required_system},
        {"bytes", r.bytes}, {"source_size", r.source_size}, {"source_stamp", r.source_stamp}, {"kind", r.kind == Kind::Update ? "update" : "dlc"},
        {"internal", r.internal}, {"verified", r.verified}, {"enabled", r.enabled}});
    return {{"schema", 1}, {"generation", catalog.generation}, {"entries", entries}, {"managed_applications", catalog.managed_applications}};
}
bool Read(const std::filesystem::path& path, Catalog& output) {
    std::error_code ec;
    if (std::filesystem::file_size(path, ec) > MaxCatalogBytes || ec) return false;
    std::ifstream in{path, std::ios::binary};
    if (!in) return false;
    try {
        const auto json = Json::parse(in);
        if (json.at("schema") != 1 || !json.at("entries").is_array() || json["entries"].size() > 4096) return false;
        Catalog catalog;
        catalog.generation = json.at("generation").get<std::uint64_t>();
        catalog.managed_applications = json.value("managed_applications", std::vector<std::uint64_t>{});
        if (catalog.managed_applications.size() > 4096) return false;
        for (auto app : catalog.managed_applications)
            if (!app || (app & 0x1fff)) return false;
        std::set<std::string> ids;
        std::set<std::pair<std::uint64_t, Kind>> active_updates;
        std::set<std::uint64_t> active_dlc;
        for (const auto& item : json["entries"]) {
            Revision r;
            r.id = item.at("id"); r.source = item.at("source"); r.name = item.at("name");
            r.display_version = item.at("display_version");
            r.application = item.at("application"); r.title = item.at("title"); r.version = item.at("version");
            r.required_application = item.at("required_application"); r.required_system = item.at("required_system");
            r.bytes = item.at("bytes"); r.source_size = item.at("source_size"); r.source_stamp = item.value("source_stamp", std::uint64_t{});
            const std::string kind = item.at("kind");
            if (kind != "update" && kind != "dlc") return false;
            r.kind = kind == "update" ? Kind::Update : Kind::Dlc;
            r.internal = item.at("internal"); r.verified = item.at("verified"); r.enabled = item.at("enabled");
            if (!ValidId(r.id) || !ids.insert(r.id).second || r.source.size() > 4096 ||
                r.source.find('\0') != std::string::npos || r.name.size() > 1024 || r.display_version.size() > 128 ||
                !r.application || (r.title & 0xffffffffffffe000ULL) != r.application ||
                (r.kind == Kind::Update && r.title != (r.application | 0x800)) ||
                (r.enabled && !r.verified)) return false;
            if (r.enabled && (r.kind == Kind::Update ? !active_updates.emplace(r.application, r.kind).second :
                                                       !active_dlc.insert(r.title).second)) return false;
            catalog.revisions.push_back(std::move(r));
        }
        output = std::move(catalog); return true;
    } catch (const std::exception&) { return false; }
}
} // namespace

std::filesystem::path ContentStore::Object(const std::string& id) const {
    return ValidId(id) ? root / "objects" / id : std::filesystem::path{};
}
std::filesystem::path ContentStore::Temporary(const std::string& id) const {
    return ValidId(id) ? root / "pending" / id : std::filesystem::path{};
}
Error ContentStore::Load(Catalog& catalog) const {
    const auto path = root / "catalog.json";
    std::error_code ec;
    if (Read(path, catalog) || Read(root / "catalog.previous.json", catalog)) return Error::None;
    if (std::filesystem::exists(path, ec) || std::filesystem::exists(root / "catalog.previous.json", ec))
        return Error::Registry;
    catalog = {}; return Error::None;
}
Error ContentStore::Save(Catalog& catalog) const {
    try {
        std::filesystem::create_directories(root);
        Catalog next = catalog; ++next.generation;
        const auto bytes = Encode(next).dump(2);
        const auto tmp = root / "catalog.next.json";
        if ((failure && failure(PublishStep::WriteNext)) || !Write(tmp, bytes)) return Error::Io;
        Catalog verified;
        if ((failure && failure(PublishStep::ValidateNext)) || !Read(tmp, verified)) return Error::Registry;
        Catalog previous;
        if (Read(root / "catalog.json", previous)) {
            const auto backup = root / "catalog.backup.tmp";
            if ((failure && failure(PublishStep::WriteBackup)) || !Write(backup, Encode(previous).dump(2)) ||
                (failure && failure(PublishStep::ReplaceBackup)) || !Replace(backup, root / "catalog.previous.json"))
                return Error::Io;
        }
        if ((failure && failure(PublishStep::ReplaceCatalog)) || !Replace(tmp, root / "catalog.json")) return Error::Io;
        catalog = std::move(next); return Error::None;
    } catch (const std::exception&) { return Error::Io; }
}
Error ContentStore::Remove(Catalog& catalog, const std::string& id) const {
    Catalog next = catalog;
    std::erase_if(next.revisions, [&](const auto& r) { return r.id == id; });
    if (next.revisions.size() == catalog.revisions.size()) return Error::Invalid;
    if (auto error = Save(next); error != Error::None) return error;
    catalog = std::move(next);
    // The backup may still reference an object. Cleanup protects both generations.
    Cleanup(catalog);
    return Error::None;
}
void ContentStore::Cleanup(const Catalog& catalog) const {
    std::set<std::string> keep;
    for (const auto& r : catalog.revisions) if (r.internal) keep.insert(r.id);
    Catalog previous;
    if (Read(root / "catalog.previous.json", previous))
        for (const auto& r : previous.revisions) if (r.internal) keep.insert(r.id);
    for (const auto& folder : {root / "pending", root / "objects"}) {
        std::error_code ec;
        for (std::filesystem::directory_iterator it{folder, ec}, end; !ec && it != end; it.increment(ec)) {
            const auto name = it->path().filename().string();
            if (!ValidId(name) || it->is_symlink(ec)) continue;
            if (folder.filename() == "pending" || !keep.contains(name)) {
                // Paths are generated 64-hex children, never taken from source/JSON paths.
                std::filesystem::remove_all(it->path(), ec);
            }
        }
    }
}
} // namespace EdenXbox::Content
