// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#include "eden_uwp/uwp_rom_storage.h"
#include "eden_uwp/storage_path.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <mutex>
#include <thread>
#include <windows.h>
#include <objbase.h>
#include <fileapifromapp.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.AccessCache.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Storage.Search.h>
#include "common/logging.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/loader/nro.h"

namespace EdenXbox {
namespace {
using namespace winrt::Windows::Storage;
using namespace winrt::Windows::Storage::Streams;
using winrt::Windows::Storage::AccessCache::StorageApplicationPermissions;
constexpr unsigned PageSize = 64;
constexpr unsigned MaxSources = 16;
constexpr unsigned MaxEntries = 10000;
constexpr unsigned ReadChunk = 1024 * 1024;

void EnsureStorageApartment() {
    // Core filesystem workers are ordinary std::threads. Initialize their MTA
    // lazily, once per thread; never block a picker/UI STA on a WinRT .get().
    struct Apartment {
        Apartment() { winrt::init_apartment(winrt::apartment_type::multi_threaded); }
        ~Apartment() { winrt::uninit_apartment(); }
    };
    thread_local Apartment apartment;
}

std::string ErrorText(const winrt::hresult_error& e) {
    return "USB/carpeta no disponible. Conectala o vuelve a agregarla. " +
           winrt::to_string(e.message());
}
void AddError(LibraryScan& scan, std::string_view error) {
    // Bound diagnostic/UI text even if every source is unavailable.
    if (scan.error.size() < 1500) {
        if (!scan.error.empty()) scan.error += " | ";
        scan.error += error;
    }
}
StorageFolder ResolveFolder(const StoragePath& path, std::string_view relative) {
    auto folder = StorageApplicationPermissions::FutureAccessList().GetFolderAsync(
        winrt::to_hstring(path.token)).get();
    while (!relative.empty()) {
        const auto slash = relative.find('/');
        folder = folder.GetFolderAsync(winrt::to_hstring(relative.substr(0, slash))).get();
        if (slash == std::string_view::npos) break;
        relative.remove_prefix(slash + 1);
    }
    return folder;
}

class StorageDirectory;
FileSys::VirtualDir MakeDirectory(StorageFolder folder, std::string path);

class StorageGameFile final : public FileSys::VfsFile {
public:
    StorageGameFile(StorageFile file, FileSys::VirtualDir parent, std::string path,
                    bool force_stream = false)
        : item(std::move(file)), parent(std::move(parent)), path(std::move(path)),
          name(winrt::to_string(item.Name())) {
        if (!force_stream && !item.Path().empty()) {
            handle = CreateFile2FromAppW(item.Path().c_str(), GENERIC_READ, FILE_SHARE_READ,
                                        OPEN_EXISTING, nullptr);
            if (handle == INVALID_HANDLE_VALUE) {
                LOG_INFO(Frontend, "ROM storage: direct open failed error={}, trying WinRT: {}",
                         GetLastError(), this->path);
            } else {
                LARGE_INTEGER bytes{};
                if (!GetFileSizeEx(handle, &bytes) || bytes.QuadPart < 0) {
                    CloseHandle(handle);
                    handle = INVALID_HANDLE_VALUE;
                } else size = static_cast<size_t>(bytes.QuadPart);
            }
        }
        if (handle == INVALID_HANDLE_VALUE) {
            stream = item.OpenReadAsync().get();
            size = static_cast<size_t>(stream.Size());
        }
        LOG_INFO(Frontend, "ROM storage: opened backend={} size={} path={}",
                 handle != INVALID_HANDLE_VALUE ? "FromApp" : "WinRT", size, this->path);
    }
    ~StorageGameFile() override {
        if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
        if (stream) { try { stream.Close(); } catch (...) {} }
    }
    std::string GetName() const override { return name; }
    std::string GetFullPath() const override { return path; }
    size_t GetSize() const override { return size; }
    FileSys::VirtualDir GetContainingDirectory() const override { return parent; }
    bool IsReadable() const override { return true; }
    bool IsDirect() const { return handle != INVALID_HANDLE_VALUE; }
    bool IsWritable() const override { return false; }
    bool Resize(size_t) override { return false; }
    bool Rename(std::string_view) override { return false; }
    size_t Write(const u8*, size_t, size_t) override { return 0; }
    size_t Read(u8* data, size_t length, size_t offset) const override {
        if (offset >= size || !length || !data) return 0;
        length = std::min(length, size - offset);
        std::scoped_lock lock{mutex}; // Serialize seek+read on this file, not the whole VFS.
        size_t done = 0;
        try {
            if (handle != INVALID_HANDLE_VALUE) {
                LARGE_INTEGER position{};
                position.QuadPart = static_cast<LONGLONG>(offset);
                if (!SetFilePointerEx(handle, position, nullptr, FILE_BEGIN)) {
                    LogReadFailure(GetLastError());
                    return 0;
                }
            } else {
                EnsureStorageApartment();
                stream.Seek(offset);
            }
            // At most 1MiB scratch on the brokered route, independent of dump size.
            Buffer buffer{nullptr};
            if (handle == INVALID_HANDLE_VALUE)
                buffer = Buffer{static_cast<unsigned>(std::min<size_t>(ReadChunk, length))};
            while (done < length) {
                const auto count = static_cast<unsigned>(std::min<size_t>(ReadChunk, length - done));
                unsigned received{};
                if (handle != INVALID_HANDLE_VALUE) {
                    DWORD read{};
                    if (!ReadFile(handle, data + done, count, &read, nullptr)) {
                        LogReadFailure(GetLastError());
                        break;
                    }
                    received = read;
                } else {
                    buffer.Length(0);
                    auto result = stream.ReadAsync(buffer, count, InputStreamOptions::None).get();
                    received = result.Length();
                    if (received > count) break;
                    std::memcpy(data + done, result.data(), received);
                }
                done += received;
                if (!received) break;
            }
        } catch (const winrt::hresult_error& e) {
            LogReadFailure(static_cast<unsigned>(e.code().value));
        }
        return done;
    }
private:
    void LogReadFailure(unsigned error) const {
        if (!failed.exchange(true))
            LOG_ERROR(Frontend, "ROM storage: read failed error={} path={} (USB removed/permission?)",
                      error, path);
    }
    StorageFile item;
    FileSys::VirtualDir parent;
    std::string path, name;
    HANDLE handle{INVALID_HANDLE_VALUE};
    IRandomAccessStreamWithContentType stream{nullptr};
    size_t size{};
    mutable std::mutex mutex;
    mutable std::atomic<bool> failed{};
};

class StorageDirectory final : public FileSys::ReadOnlyVfsDirectory {
public:
    StorageDirectory(StorageFolder folder, std::string path)
        : folder(std::move(folder)), path(std::move(path)), name(winrt::to_string(this->folder.Name())) {}
    std::string GetName() const override { return name; }
    std::string GetFullPath() const override { return path; }
    FileSys::VirtualDir GetParentDirectory() const override {
        const auto parsed = ParseStoragePath(path);
        if (!parsed || parsed->relative.empty()) return {};
        const auto slash = parsed->relative.rfind('/');
        const auto parent = slash == std::string::npos ? "" : parsed->relative.substr(0, slash);
        try {
            EnsureStorageApartment();
            return MakeDirectory(ResolveFolder(*parsed, parent), MakeStoragePath(parsed->token, parent));
        }
        catch (const winrt::hresult_error&) { return {}; }
    }
    FileSys::VirtualFile GetFileRelative(std::string_view relative) const override {
        if (relative.empty() || !ValidStorageRelative(relative)) return {};
        try {
            EnsureStorageApartment();
            const auto slash = relative.rfind('/');
            auto containing = folder;
            std::string parent_path = path;
            if (slash != std::string_view::npos) {
                auto directory = GetDirectoryRelative(relative.substr(0, slash));
                if (!directory) return {};
                return directory->GetFile(relative.substr(slash + 1));
            }
            auto file = containing.GetFileAsync(winrt::to_hstring(relative)).get();
            return std::make_shared<StorageGameFile>(file, MakeDirectory(containing, parent_path),
                                                     path + "/" + std::string{relative});
        } catch (const winrt::hresult_error& e) {
            LOG_WARNING(Frontend, "ROM storage: file unavailable {}: {}", path, winrt::to_string(e.message()));
            return {};
        }
    }
    FileSys::VirtualDir GetDirectoryRelative(std::string_view relative) const override {
        if (!ValidStorageRelative(relative)) return {};
        try {
            EnsureStorageApartment();
            auto next = folder;
            auto remaining = relative;
            while (!remaining.empty()) {
                const auto slash = remaining.find('/');
                next = next.GetFolderAsync(winrt::to_hstring(remaining.substr(0, slash))).get();
                if (slash == std::string_view::npos) break;
                remaining.remove_prefix(slash + 1);
            }
            return MakeDirectory(next, path + (relative.empty() ? "" : "/" + std::string{relative}));
        } catch (const winrt::hresult_error&) { return {}; }
    }
    std::vector<FileSys::VirtualFile> GetFiles() const override {
        std::vector<FileSys::VirtualFile> result;
        try {
            EnsureStorageApartment();
            for (unsigned start = 0; start < MaxEntries; start += PageSize) {
                auto files = folder.GetFilesAsync(Search::CommonFileQuery::DefaultQuery, start, PageSize).get();
                for (const auto& file : files)
                    result.push_back(std::make_shared<StorageGameFile>(file, MakeDirectory(folder, path),
                                      path + "/" + winrt::to_string(file.Name())));
                if (files.Size() < PageSize) break;
            }
        } catch (const winrt::hresult_error&) {}
        return result;
    }
    std::vector<FileSys::VirtualDir> GetSubdirectories() const override {
        std::vector<FileSys::VirtualDir> result;
        try {
            EnsureStorageApartment();
            for (unsigned start = 0; start < MaxEntries; start += PageSize) {
                auto folders = folder.GetFoldersAsync(Search::CommonFolderQuery::DefaultQuery, start, PageSize).get();
                for (const auto& child : folders)
                    result.push_back(MakeDirectory(child, path + "/" + winrt::to_string(child.Name())));
                if (folders.Size() < PageSize) break;
            }
        } catch (const winrt::hresult_error&) {}
        return result;
    }
private:
    StorageFolder folder;
    std::string path, name;
};
FileSys::VirtualDir MakeDirectory(StorageFolder folder, std::string path) {
    return std::make_shared<StorageDirectory>(std::move(folder), std::move(path));
}

class UwpFilesystem final : public FileSys::RealVfsFilesystem {
public:
    FileSys::VirtualFile OpenFile(std::string_view path, FileSys::OpenMode mode) override {
        if (!IsStoragePath(path)) return RealVfsFilesystem::OpenFile(path, mode);
        const auto parsed = ParseStoragePath(path);
        if (!parsed || parsed->relative.empty() || mode != FileSys::OpenMode::Read) return {};
        try {
            EnsureStorageApartment();
            if (!StorageApplicationPermissions::FutureAccessList().ContainsItem(winrt::to_hstring(parsed->token))) return {};
            const auto slash = parsed->relative.rfind('/');
            const auto parent = slash == std::string::npos ? "" : parsed->relative.substr(0, slash);
            const auto name = slash == std::string::npos ? parsed->relative : parsed->relative.substr(slash + 1);
            auto folder = ResolveFolder(*parsed, parent);
            auto file = folder.GetFileAsync(winrt::to_hstring(name)).get();
            return std::make_shared<StorageGameFile>(file, MakeDirectory(folder, MakeStoragePath(parsed->token, parent)),
                                                     std::string{path});
        } catch (const winrt::hresult_error& e) {
            LOG_ERROR(Frontend, "ROM storage: open failed {}: {}", path, winrt::to_string(e.message()));
            return {};
        }
    }
    FileSys::VirtualDir OpenDirectory(std::string_view path, FileSys::OpenMode mode) override {
        if (!IsStoragePath(path)) return RealVfsFilesystem::OpenDirectory(path, mode);
        const auto parsed = ParseStoragePath(path);
        if (!parsed || mode != FileSys::OpenMode::Read) return {};
        try {
            EnsureStorageApartment();
            return MakeDirectory(ResolveFolder(*parsed, parsed->relative), std::string{path});
        }
        catch (const winrt::hresult_error&) { return {}; }
    }
    FileSys::VfsEntryType GetEntryType(std::string_view path) const override {
        if (!IsStoragePath(path)) return RealVfsFilesystem::GetEntryType(path);
        const auto parsed = ParseStoragePath(path);
        if (!parsed) return FileSys::VfsEntryType::None;
        try {
            EnsureStorageApartment();
            if (parsed->relative.empty()) {
                ResolveFolder(*parsed, "");
                return FileSys::VfsEntryType::Directory;
            }
            const auto slash = parsed->relative.rfind('/');
            const auto parent = slash == std::string::npos ? "" : parsed->relative.substr(0, slash);
            const auto name = slash == std::string::npos ? parsed->relative : parsed->relative.substr(slash + 1);
            auto item = ResolveFolder(*parsed, parent).TryGetItemAsync(winrt::to_hstring(name)).get();
            if (!item) return FileSys::VfsEntryType::None;
            return item.IsOfType(StorageItemTypes::Folder) ? FileSys::VfsEntryType::Directory : FileSys::VfsEntryType::File;
        } catch (const winrt::hresult_error&) { return FileSys::VfsEntryType::None; }
    }
};

void ScanFolder(StorageFolder folder, const std::string& token, const std::wstring& source,
                std::string relative, unsigned depth, unsigned& visited,
                LibraryScan& scan, std::stop_token stop) {
    for (unsigned start = 0; !stop.stop_requested(); start += PageSize) {
        auto items = folder.GetItemsAsync(start, PageSize).get();
        for (const auto& item : items) {
            if (stop.stop_requested()) return;
            if (++visited > MaxEntries) { scan.limited = true; return; }
            const auto name = winrt::to_string(item.Name());
            const auto child = relative.empty() ? name : relative + "/" + name;
            if (!ValidStorageRelative(child)) continue;
            if (item.IsOfType(StorageItemTypes::Folder)) {
                if (depth < 4) {
                    try { ScanFolder(item.as<StorageFolder>(), token, source, child, depth + 1, visited, scan, stop); }
                    catch (const winrt::hresult_error& e) { AddError(scan, ErrorText(e)); }
                }
            } else {
                const auto p = std::filesystem::path{winrt::to_hstring(child).c_str()};
                auto ext = p.extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (ext != ".nsp" && ext != ".xci" && ext != ".nro") continue;
                LibraryEntry entry{p, p.stem().wstring()};
                entry.launch_path = MakeStoragePath(token, child);
                entry.source_name = source;
                scan.entries.push_back(std::move(entry));
            }
            if (scan.limited) return;
        }
        if (items.Size() < PageSize) break;
    }
}
} // namespace

std::string RememberGameFolder(const StorageFolder& folder) {
    EnsureStorageApartment();
    auto list = StorageApplicationPermissions::FutureAccessList();
    unsigned count = 0;
    std::string token;
    const auto path = folder.Path();
    for (const auto& entry : list.Entries()) {
        const auto existing = winrt::to_string(entry.Token);
        if (!existing.starts_with("eden-games-")) continue;
        ++count;
        // Reauthorizing the same path updates its token rather than duplicating games.
        try {
            if (!path.empty() && list.GetFolderAsync(entry.Token).get().Path() == path) token = existing;
        } catch (const winrt::hresult_error&) {
            if (!path.empty() && entry.Metadata == path) token = existing;
        }
    }
    if (token.empty()) {
        if (count >= MaxSources) throw std::runtime_error("Limite de 16 carpetas externas alcanzado.");
        GUID guid{};
        winrt::check_hresult(CoCreateGuid(&guid));
        token = "eden-games-";
        const auto* bytes = reinterpret_cast<const unsigned char*>(&guid);
        constexpr char hex[] = "0123456789abcdef";
        for (size_t i = 0; i < sizeof(guid); ++i) { token += hex[bytes[i] >> 4]; token += hex[bytes[i] & 15]; }
    }
    list.AddOrReplace(winrt::to_hstring(token), folder, path);
    return token;
}

LibraryScan ScanExternalGameFolders(std::stop_token stop) {
    EnsureStorageApartment();
    LibraryScan result;
    unsigned visited = 0, sources = 0;
    auto list = StorageApplicationPermissions::FutureAccessList();
    for (const auto& entry : list.Entries()) {
        if (stop.stop_requested()) break;
        const auto token = winrt::to_string(entry.Token);
        if (!token.starts_with("eden-games-")) continue;
        if (++sources > MaxSources) { result.limited = true; break; }
        try {
            auto folder = list.GetFolderAsync(entry.Token).get();
            ScanFolder(folder, token, std::wstring{folder.Name()}, "", 0, visited, result, stop);
        } catch (const winrt::hresult_error& e) { AddError(result, ErrorText(e)); }
        if (result.limited) break;
    }
    return result;
}

FileSys::VirtualFilesystem MakeUwpFilesystem() { return std::make_shared<UwpFilesystem>(); }

std::string CheckExternalGame(std::string_view path) {
    if (!IsStoragePath(path)) return {};
    try {
        auto fs = MakeUwpFilesystem();
        auto file = fs->OpenFile(path, FileSys::OpenMode::Read);
        if (!file) return "No se pudo abrir el juego. Conecta el USB o vuelve a agregar su carpeta.";
        std::array<u8, 64> bytes{};
        const auto size = file->GetSize();
        if (!size || file->Read(bytes.data(), std::min(bytes.size(), size), 0) != std::min(bytes.size(), size))
            return "No se pudo leer el juego desde su carpeta.";
        // Exercise 64-bit offsets before spending time constructing the guest.
        if (size > (u64{1} << 32) + bytes.size() &&
            file->Read(bytes.data(), bytes.size(), (u64{1} << 32) + 1) != bytes.size())
            return "No se pudo leer el juego por encima de 4 GiB.";
        const auto count = std::min(bytes.size(), size);
        if (file->Read(bytes.data(), count, size - count) != count ||
            file->Read(bytes.data(), 1, size) != 0) return "No se pudo leer el final del juego.";
        LOG_INFO(Frontend, "ROM storage: preflight passed size={} path={}", size, path);
        return {};
    } catch (const winrt::hresult_error& e) { return ErrorText(e); }
      catch (const std::exception& e) { return e.what(); }
}

bool RunRomStorageGate(bool restore, const std::function<void(std::string)>& diagnostic) {
    try {
        auto require = [](bool success, const char* message) {
            if (!success) throw std::runtime_error(message);
        };
        const auto settings = ApplicationData::Current().LocalSettings().Values();
        constexpr auto setting = L"rom-storage-gate-token";
        auto folder = ApplicationData::Current().LocalFolder().GetFolderAsync(L"rom-storage-gate").get();
        std::string token;
        if (restore) {
            require(settings.HasKey(setting), "missing gate token from preceding launch");
            token = winrt::to_string(winrt::unbox_value<winrt::hstring>(settings.Lookup(setting)));
            folder = StorageApplicationPermissions::FutureAccessList().GetFolderAsync(winrt::to_hstring(token)).get();
        } else {
            token = RememberGameFolder(folder);
            settings.Insert(setting, winrt::box_value(winrt::to_hstring(token)));
        }
        require(RememberGameFolder(folder) == token, "reauthorization duplicated the source");
        const auto path = MakeStoragePath(token, "fixture.bin");
        const auto item = folder.GetFileAsync(L"fixture.bin").get();
        const auto parent = MakeDirectory(folder, MakeStoragePath(token));
        auto direct = std::make_shared<StorageGameFile>(item, parent, path);
        auto brokered = std::make_shared<StorageGameFile>(item, parent, path, true);
        require(direct->IsDirect(), "fixture direct FromApp open failed");
        const size_t expected_size = (u64{1} << 32) + 2 * ReadChunk + 257;
        require(direct->GetSize() == expected_size && brokered->GetSize() == expected_size, "64-bit file size mismatch");
        auto verify = [&](const FileSys::VirtualFile& file, size_t offset, size_t length) {
            std::vector<u8> bytes(length, 0);
            const auto expected = offset >= expected_size ? 0 : std::min(length, expected_size - offset);
            require(file->Read(bytes.data(), length, offset) == expected, "read length/EOF mismatch");
            for (size_t i = 0; i < expected; ++i) {
                const u64 position = offset + i;
                const auto value = static_cast<u8>((position * 17) ^ (position >> 16) ^ 0x5a);
                require(bytes[i] == value, "reference bytes mismatch");
            }
        };
        for (const FileSys::VirtualFile& file : {FileSys::VirtualFile{direct}, FileSys::VirtualFile{brokered}}) {
            verify(file, 0, 2 * ReadChunk + 257);
            verify(file, 13, ReadChunk + 101); // crosses the bounded read chunk
            verify(file, (u64{1} << 32) - 97, 512);
            verify(file, (u64{1} << 32) + 1, 64);
            verify(file, expected_size - 64, 128);
            verify(file, expected_size, 64);
            verify(file, SIZE_MAX, 64);
            require(!file->Resize(0) && !file->Rename("changed.bin") &&
                    file->Write(nullptr, 1, 0) == 0, "external VFS allowed mutation");
        }
        std::atomic<bool> concurrent_ok{true};
        {
            std::array<std::jthread, 4> workers;
            for (unsigned worker = 0; worker < workers.size(); ++worker) {
                workers[worker] = std::jthread([&, worker] {
                    // Intentionally no apartment: exercise the ordinary core worker path.
                    try {
                        for (unsigned i = 0; i < 32; ++i) {
                            const size_t offset = i % 2 ? (u64{1} << 32) + 1 : 13 + worker * 29;
                            verify(direct, offset, 64);
                            verify(brokered, offset, 64);
                        }
                    } catch (...) { concurrent_ok = false; }
                });
            }
        }
        require(concurrent_ok, "concurrent offset reads mismatch");
        auto fs = MakeUwpFilesystem();
        require(fs->GetEntryType(path) == FileSys::VfsEntryType::File, "token VFS entry type mismatch");
        auto virtual_file = fs->OpenFile(path, FileSys::OpenMode::Read);
        require(bool(virtual_file), "token VFS open failed");
        verify(virtual_file, (u64{1} << 32) + 1, 64);
        require(bool(virtual_file->GetContainingDirectory()->GetFile("fixture.bin")), "sibling open failed");
        require(!fs->OpenFile(path, FileSys::OpenMode::ReadWrite), "writable external open allowed");
        require(!fs->OpenFile(MakeStoragePath(token, "../fixture.bin"), FileSys::OpenMode::Read), "traversal allowed");
        require(!fs->OpenFile(MakeStoragePath("eden-games-missing", "fixture.bin"), FileSys::OpenMode::Read), "missing token opened");
        require(CheckExternalGame(path).empty(), "preflight failed");
        const auto scan = ScanExternalGameFolders({});
        unsigned catalog_entries = 0;
        bool nested_a = false, nested_b = false;
        for (const auto& entry : scan.entries) {
            const auto parsed = ParseStoragePath(entry.launch_path);
            if (!parsed || parsed->token != token) continue;
            ++catalog_entries;
            nested_a |= parsed->relative == "a/same.nro";
            nested_b |= parsed->relative == "b/same.nro";
        }
        require(catalog_entries == 72 && nested_a && nested_b, "paged/nested catalog identity mismatch");
        const auto nested = fs->OpenDirectory(MakeStoragePath(token, "a"), FileSys::OpenMode::Read);
        require(bool(nested), "nested directory open failed");
        require(nested->GetParentDirectory()->GetFullPath() == MakeStoragePath(token), "directory parent escaped the source");
        auto homebrew = nested->GetFile("same.nro");
        require(homebrew && Loader::AppLoader_NRO::IdentifyType(homebrew) == Loader::FileType::NRO,
                "core loader cannot identify token-scoped homebrew");
        auto homebrew_item = folder.GetFolderAsync(L"a").get().GetFileAsync(L"same.nro").get();
        auto streamed_homebrew = std::make_shared<StorageGameFile>(homebrew_item, nested,
            MakeStoragePath(token, "a/same.nro"), true);
        require(Loader::AppLoader_NRO::IdentifyType(streamed_homebrew) == Loader::FileType::NRO,
                "core loader cannot identify brokered homebrew");
        Loader::AppLoader_NRO direct_loader{homebrew}, stream_loader{streamed_homebrew};
        std::string direct_title, stream_title;
        require(direct_loader.ReadTitle(direct_title) == stream_loader.ReadTitle(stream_title) &&
                direct_title == stream_title, "core metadata differs between backends");
        if (restore) {
            StorageApplicationPermissions::FutureAccessList().Remove(winrt::to_hstring(token));
            settings.Remove(setting);
            require(!fs->OpenFile(path, FileSys::OpenMode::Read), "revoked token opened a new file");
        }
        diagnostic(std::string("ROM storage gate PASS phase=") + (restore ? "restore" : "record") +
                   " size=" + std::to_string(expected_size) +
                   " direct+forced-WinRT reference/EOF/chunks/concurrency/token/sibling/readonly/catalog72/loader OK");
        return true;
    } catch (const winrt::hresult_error& e) {
        diagnostic("ROM storage gate FAIL: " + winrt::to_string(e.message()));
    } catch (const std::exception& e) { diagnostic(std::string("ROM storage gate FAIL: ") + e.what()); }
    return false;
}
} // namespace EdenXbox
