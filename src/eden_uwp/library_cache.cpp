// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#include "eden_uwp/library_cache.h"

#include <fstream>
#include <system_error>

#include "eden_uwp/atomic_file.h"

// Host-side tests build this file without WinRT.
#ifndef EDEN_XBOX_NO_WINRT
#include <winrt/Windows.Storage.h>
#endif

namespace EdenXbox {
namespace {
constexpr std::uint32_t Magic = 0x434C5845; // "EXLC"
constexpr std::uint32_t Version = 3;
constexpr std::uint32_t MaxText = 512;      // UTF-16 units; NACP names are at most 512 bytes.

struct Header {
    std::uint32_t magic;
    std::uint32_t version;
    std::uint64_t stamp;
    std::uint32_t language;
    std::uint32_t name_length;
    std::uint32_t developer_length;
    std::uint32_t display_version_length;
    std::uint32_t icon_size;
    std::uint64_t program_id;
    std::array<std::uint8_t, 8> build_id;
};
static_assert(sizeof(Header) == 56);

std::uint64_t Fnv1a(std::string_view text) {
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    for (const unsigned char c : text) {
        hash ^= c;
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

bool ReadText(std::ifstream& in, std::uint32_t length, std::wstring& out) {
    std::vector<char16_t> units(length);
    if (length && !in.read(reinterpret_cast<char*>(units.data()), length * sizeof(char16_t)))
        return false;
    out.assign(units.begin(), units.end());
    return true;
}

void Append(std::string& out, const void* data, std::size_t size) {
    out.append(static_cast<const char*>(data), size);
}

void AppendText(std::string& out, std::wstring_view text) {
    for (const wchar_t c : text) {
        const auto unit = static_cast<char16_t>(c);
        Append(out, &unit, sizeof(unit));
    }
}
} // namespace

LibraryCache::LibraryCache(std::filesystem::path directory_) : directory{std::move(directory_)} {}

std::filesystem::path LibraryCache::RecordPath(std::string_view launch_path) const {
    static constexpr char hex[] = "0123456789abcdef";
    auto hash = Fnv1a(launch_path);
    std::string name(16, '0');
    for (auto it = name.rbegin(); it != name.rend(); ++it, hash >>= 4) *it = hex[hash & 15];
    return directory / (name + ".bin");
}

std::optional<LibraryCacheRecord> LibraryCache::Load(std::string_view launch_path,
                                                     std::uint64_t stamp, std::uint32_t language,
                                                     bool with_icon) const {
    if (directory.empty()) return std::nullopt;
    std::ifstream in{RecordPath(launch_path), std::ios::binary};
    Header header{};
    if (!in || !in.read(reinterpret_cast<char*>(&header), sizeof(header))) return std::nullopt;
    if (header.magic != Magic || header.version != Version || header.stamp != stamp ||
        header.language != language || header.name_length > MaxText ||
        header.developer_length > MaxText || header.display_version_length > MaxText || header.icon_size > MaxIcon)
        return std::nullopt;
    // Two games whose paths hash alike: the record also stores its path, compared here.
    std::string stored_path(launch_path.size(), '\0');
    std::uint32_t path_size = 0;
    if (!in.read(reinterpret_cast<char*>(&path_size), sizeof(path_size)) ||
        path_size != launch_path.size() ||
        !in.read(stored_path.data(), static_cast<std::streamsize>(path_size)) ||
        stored_path != launch_path)
        return std::nullopt;
    LibraryCacheRecord record;
    record.program_id = header.program_id;
    record.build_id = header.build_id;
    if (!ReadText(in, header.name_length, record.name) ||
        !ReadText(in, header.developer_length, record.developer) ||
        !ReadText(in, header.display_version_length, record.display_version))
        return std::nullopt;
    if (with_icon && header.icon_size) {
        record.icon.resize(header.icon_size);
        if (!in.read(reinterpret_cast<char*>(record.icon.data()), header.icon_size)) return std::nullopt;
    }
    return record;
}

bool LibraryCache::Store(std::string_view launch_path, std::uint64_t stamp, std::uint32_t language,
                         const LibraryCacheRecord& record) const {
    if (directory.empty() || record.icon.size() > MaxIcon) return false;
    const std::wstring_view name = std::wstring_view{record.name}.substr(0, MaxText);
    const std::wstring_view developer = std::wstring_view{record.developer}.substr(0, MaxText);
    const auto display_version = std::wstring_view{record.display_version}.substr(0, MaxText);
    const Header header{Magic,
                        Version,
                        stamp,
                        language,
                        static_cast<std::uint32_t>(name.size()),
                        static_cast<std::uint32_t>(developer.size()),
                        static_cast<std::uint32_t>(display_version.size()),
                        static_cast<std::uint32_t>(record.icon.size()),
                        record.program_id,
                        record.build_id};
    const auto path_size = static_cast<std::uint32_t>(launch_path.size());
    std::string bytes;
    bytes.reserve(sizeof(header) + sizeof(path_size) + launch_path.size() +
                  (name.size() + developer.size() + display_version.size()) * sizeof(char16_t) + record.icon.size());
    Append(bytes, &header, sizeof(header));
    Append(bytes, &path_size, sizeof(path_size));
    Append(bytes, launch_path.data(), launch_path.size());
    AppendText(bytes, name);
    AppendText(bytes, developer);
    AppendText(bytes, display_version);
    Append(bytes, record.icon.data(), record.icon.size());
    return WriteFileAtomically(RecordPath(launch_path), bytes);
}

bool LibraryCache::Clear() const {
    if (directory.empty()) return true;
    std::error_code ec;
    std::filesystem::remove_all(directory, ec);
    return !ec;
}

std::uint64_t LibraryCache::StampFor(const std::filesystem::path& file) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(file, ec);
    if (ec) return 0;
    const auto time = std::filesystem::last_write_time(file, ec);
    if (ec) return 0;
    const auto ticks = static_cast<std::uint64_t>(time.time_since_epoch().count());
    // Mix both so a same-size replacement or a touched file still changes the stamp; never 0.
    return (Fnv1a(std::string_view{reinterpret_cast<const char*>(&ticks), sizeof(ticks)}) ^
            (size * 0x9E3779B97F4A7C15ULL)) | 1;
}

std::filesystem::path DefaultLibraryCacheDirectory() {
#ifndef EDEN_XBOX_NO_WINRT
    try {
        return std::filesystem::path{
            winrt::Windows::Storage::ApplicationData::Current().LocalCacheFolder().Path().c_str()} /
               L"library";
    } catch (const winrt::hresult_error&) {}
#endif
    return {};
}

} // namespace EdenXbox
