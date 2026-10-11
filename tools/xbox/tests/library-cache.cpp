// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
// Build with src/eden_uwp/library_cache.cpp and /DEDEN_XBOX_NO_WINRT.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include "eden_uwp/library_cache.h"

int main() {
    using namespace EdenXbox;
    auto check = [](bool ok) { if (!ok) std::abort(); };
    const auto root = std::filesystem::temp_directory_path() / "eden-library-cache-test";
    std::filesystem::remove_all(root);
    const LibraryCache cache{root / "cache"};

    const std::string game = "eden-usb:/eden-games-abc/sub/Game.nsp";
    LibraryCacheRecord record{L"Título ñ", L"Estudio", std::vector<unsigned char>(5000, 0x5A),
                              0x0100ABCD00010000ULL, {0x55, 0xEE, 0x1A, 0x69, 0x14, 0x5D, 0xE0, 0x4E}};
    record.display_version = L"1.0.0";
    check(!cache.Load(game, 7, 14, true));
    check(cache.Store(game, 7, 14, record));

    // Hit with and without the icon; any change of stamp, language or path misses.
    const auto full = cache.Load(game, 7, 14, true);
    check(full && full->name == record.name && full->developer == record.developer && full->icon == record.icon);
    check(full->display_version == L"1.0.0");
    check(full->program_id == record.program_id && full->build_id == record.build_id);
    const auto texts = cache.Load(game, 7, 14, false);
    check(texts && texts->name == record.name && texts->icon.empty());
    check(texts->display_version == L"1.0.0");
    check(texts->program_id == record.program_id && texts->build_id == record.build_id);
    check(!cache.Load(game, 8, 14, true));
    check(!cache.Load(game, 7, 1, true));
    check(!cache.Load(game + "x", 7, 14, true));

    // Replacing a record leaves no temporary file behind.
    record.name = L"Otro";
    check(cache.Store(game, 7, 14, record) && cache.Load(game, 7, 14, false)->name == L"Otro");
    unsigned files = 0;
    for (const auto& item : std::filesystem::directory_iterator{root / "cache"}) {
        check(item.path().extension() == ".bin");
        ++files;
    }
    check(files == 1);

    // An update changes the catalog stamp: never reuse the old base version.
    record.display_version = L"2.0.0";
    check(cache.Store(game, 8, 14, record));
    check(!cache.Load(game, 7, 14, false));
    check(cache.Load(game, 8, 14, false)->display_version == L"2.0.0");
    // Explicit downgrade and unavailable metadata must replace the version too.
    record.display_version = L"1.0.0";
    check(cache.Store(game, 9, 14, record));
    check(cache.Load(game, 9, 14, false)->display_version == L"1.0.0");
    record.display_version.clear();
    check(cache.Store(game, 7, 14, record));
    check(cache.Load(game, 7, 14, false)->display_version.empty());
    // Old-format records are a miss and will be read again from the game.
    for (const auto& item : std::filesystem::directory_iterator{root / "cache"}) {
        std::fstream out{item.path(), std::ios::in | std::ios::out | std::ios::binary};
        const std::uint32_t old_version = 2;
        out.seekp(4); out.write(reinterpret_cast<const char*>(&old_version), sizeof(old_version));
    }
    check(!cache.Load(game, 7, 14, false));
    check(cache.Store(game, 7, 14, record));

    // A truncated record is a miss, not a crash.
    for (const auto& item : std::filesystem::directory_iterator{root / "cache"})
        std::filesystem::resize_file(item.path(), 40);
    check(!cache.Load(game, 7, 14, true));

    // Stamps follow the file's size and write time and are never 0 for an existing file.
    const auto file = root / "game.nsp";
    { std::ofstream{file} << "abc"; }
    const auto stamp = LibraryCache::StampFor(file);
    check(stamp != 0 && stamp == LibraryCache::StampFor(file));
    { std::ofstream{file} << "abcd"; }
    check(LibraryCache::StampFor(file) != stamp);
    check(LibraryCache::StampFor(root / "missing.nsp") == 0);

    check(cache.Clear() && !std::filesystem::exists(root / "cache"));
    std::filesystem::remove_all(root);
    std::puts("library cache: hits, identity, misses, replacement, truncation, stamps and clear PASS");
}
