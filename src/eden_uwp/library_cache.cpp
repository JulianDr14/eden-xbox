// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#include "eden_uwp/library_cache.h"

#include <fstream>
#include <system_error>

// Host-side tests build this file without WinRT.
#ifndef EDEN_XBOX_NO_WINRT
#include <winrt/Windows.Storage.h>
#endif

namespace EdenXbox {
namespace {
constexpr std::uint32_t Magic = 0x434C5845; // "EXLC"
constexpr std::uint32_t Version = 1;
constexpr std::uint32_t MaxText = 512;      // UTF-16 units; NACP names are at most 512 bytes.

struct Header {
    std::uint32_t magic;
    std::uint32_t version;
    std::uint64_t stamp;
    std::uint32_t language;
    std::uint32_t name_length;
    std::uint32_t developer_length;
    std::uint32_t icon_size;
};
static_assert(sizeof(Header) == 32);

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

void WriteText(std::ofstream& out, std::wstring_view text) {
    std::vector<char16_t> units(text.begin(), text.end());
    out.write(reinterpret_cast<const char*>(units.data()),
              static_cast<std::streamsize>(units.size() * sizeof(char16_t)));
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
        header.developer_length > MaxText || header.icon_size > MaxIcon)
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
    if (!ReadText(in, header.name_length, record.name) ||
        !ReadText(in, header.developer_length, record.developer))
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
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    const auto path = RecordPath(launch_path);
    auto temporary = path;
    temporary += ".tmp";
    {
        std::ofstream out{temporary, std::ios::binary | std::ios::trunc};
        if (!out) return false;
        const std::wstring_view name = std::wstring_view{record.name}.substr(0, MaxText);
        const std::wstring_view developer = std::wstring_view{record.developer}.substr(0, MaxText);
        const Header header{Magic, Version, stamp, language,
                            static_cast<std::uint32_t>(name.size()),
                            static_cast<std::uint32_t>(developer.size()),
                            static_cast<std::uint32_t>(record.icon.size())};
        const auto path_size = static_cast<std::uint32_t>(launch_path.size());
        out.write(reinterpret_cast<const char*>(&header), sizeof(header));
        out.write(reinterpret_cast<const char*>(&path_size), sizeof(path_size));
        out.write(launch_path.data(), static_cast<std::streamsize>(launch_path.size()));
        WriteText(out, name);
        WriteText(out, developer);
        out.write(reinterpret_cast<const char*>(record.icon.data()),
                  static_cast<std::streamsize>(record.icon.size()));
        if (!out.flush()) {
            out.close();
            std::filesystem::remove(temporary, ec);
            return false;
        }
    }
    std::filesystem::rename(temporary, path, ec);
    if (ec) std::filesystem::remove(temporary, ec);
    return !ec;
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
