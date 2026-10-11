// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
// Build with src/eden_uwp/mods/mod_archive.cpp, src/eden_uwp/mods/mod_layout.cpp and libarchive's
// reader (externals/libarchive) with zlib, zstd and liblzma. Run with a folder of libarchive's test
// archives (libarchive/test/*.uu decoded): the 7z, rar and zip samples at its top, and the volumes
// of test_rar_multivolume_single_file in multi4/ and of test_read_format_rar5_multiarchive_solid
// in multi5/.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include "eden_uwp/mods/mod_archive.h"
#include "stored_zip.h"

namespace {
using namespace EdenXbox::Mods;
using TestArchives::StoredZip;
namespace fs = std::filesystem;

void Check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        std::fflush(stdout);
        std::abort();
    }
}

using Volumes = std::vector<std::unique_ptr<ArchiveSource>>;

Volumes Open(const std::vector<fs::path>& paths) {
    Volumes volumes;
    for (const auto& path : paths) {
        auto source = OpenFileSource(path);
        Check(source != nullptr, "open sample");
        volumes.push_back(std::move(source));
    }
    return volumes;
}

/// Lists and extracts every file of the archive; returns the bytes written.
std::uint64_t ExtractAll(Volumes& volumes, const fs::path& to, const char* what) {
    const auto listing = ListArchive(volumes);
    if (listing.error != ArchiveError::None) std::printf("  %s: %s\n", what, listing.detail.c_str());
    Check(listing.error == ArchiveError::None && !listing.items.empty(), what);
    std::vector<LayoutFile> files;
    std::uint64_t total = 0;
    for (const auto& item : listing.items) {
        files.push_back({item.path, "x/" + std::to_string(files.size()), item.size});
        total += item.size;
    }
    fs::remove_all(to);
    std::string detail;
    const auto error = ExtractArchive(volumes, files, to, UINT64_MAX, {}, {}, &detail);
    if (error != ArchiveError::None) std::printf("  %s: %s\n", what, detail.c_str());
    Check(error == ArchiveError::None, what);
    std::uint64_t written = 0;
    for (const auto& entry : fs::recursive_directory_iterator(to))
        if (entry.is_regular_file()) written += entry.file_size();
    Check(written == total, "written size matches the listing");
    std::printf("  %-48s %-14s %zu files %llu bytes\n", what, listing.format.c_str(), files.size(),
                static_cast<unsigned long long>(written));
    return written;
}
} // namespace

int main(int argc, char** argv) {
    Check(argc == 2, "usage: mod-archive <samples folder>");
    const fs::path samples = argv[1];
    const auto out = fs::temp_directory_path() / "eden-mod-archive";

    for (const char* name : {"test_read_format_7zip_bcj2_lzma2_1.7z", "test_read_format_7zip_ppmd.7z",
                             "test_read_format_7zip_lzma2_arm64.7z", "test_read_format_7zip_solid_zstd.7z",
                             "test_read_format_7zip_deflate.7z", "test_read_format_rar.rar",
                             "test_read_format_rar5_compressed.rar", "test_read_format_rar5_solid.rar",
                             "test_read_format_rar5_arm.rar", "test_read_format_zip_7z_lzma.zip",
                             "test_read_format_zip_zip64a.zip"}) {
        auto volumes = Open({samples / name});
        ExtractAll(volumes, out, name);
    }
    {
        auto volumes = Open({samples / "multi4" / "test_rar_multivolume_single_file.part1.rar",
                             samples / "multi4" / "test_rar_multivolume_single_file.part2.rar",
                             samples / "multi4" / "test_rar_multivolume_single_file.part3.rar"});
        ExtractAll(volumes, out, "RAR4 in 3 volumes");
    }
    {
        std::vector<fs::path> parts;
        for (int i = 1; i <= 4; ++i)
            parts.push_back(samples / "multi5" /
                            ("test_read_format_rar5_multiarchive_solid.part0" + std::to_string(i) + ".rar"));
        auto volumes = Open(parts);
        ExtractAll(volumes, out, "RAR5 solid in 4 volumes");
    }
    for (const char* name : {"test_read_format_rar4_encrypted.rar", "test_read_format_zip_encryption_data.zip"}) {
        auto volumes = Open({samples / name});
        const auto listing = ListArchive(volumes);
        Check(listing.error == ArchiveError::Encrypted, "encrypted is reported as such");
    }
    {
        auto volumes = TestArchives::Volumes({'n', 'o', 't', ' ', 'a', 'n', ' ', 'a', 'r', 'c', 'h', 'i', 'v', 'e'});
        Check(ListArchive(volumes).error == ArchiveError::Unreadable, "not an archive");
    }
    {
        // Only the files a layout picked are written, under the destination, at their targets.
        auto volumes = TestArchives::Volumes(StoredZip({{"../evil.txt", "boom"}, {"mod/romfs/a.bin", "hello"},
                                         {"mod/readme.txt", "skip"}}));
        const auto listing = ListArchive(volumes);
        Check(listing.error == ArchiveError::None && listing.items.size() == 3, "hand zip lists");
        fs::remove_all(out);
        Check(ExtractArchive(volumes, {{"mod/romfs/a.bin", "romfs/a.bin", 5}}, out / "stage", 1000) ==
                  ArchiveError::None,
              "picked file");
        Check(fs::exists(out / "stage" / "romfs" / "a.bin") && fs::file_size(out / "stage" / "romfs" / "a.bin") == 5,
              "written where planned");
        Check(!fs::exists(out / "evil.txt") && !fs::exists(out / "stage" / "mod"), "nothing else written");
        Check(ExtractArchive(volumes, {{"../evil.txt", "../evil.txt", 4}}, out / "stage", 1000) ==
                  ArchiveError::Unreadable,
              "unsafe target refused");
        Check(ExtractArchive(volumes, {{"mod/romfs/a.bin", "romfs/a.bin", 5}}, out / "small", 4) ==
                  ArchiveError::TooLarge,
              "size limit");
        Check(ExtractArchive(volumes, {{"gone", "gone", 1}}, out / "stage", 1000) == ArchiveError::Unreadable,
              "missing entry");
    }
    fs::remove_all(out);
    std::puts("mod-archive PASS");
}
