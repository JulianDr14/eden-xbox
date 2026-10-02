// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cassert>
#include <chrono>
#include <fstream>
#include <iostream>
#include "eden_uwp/game_library.h"

int main() {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() /
        ("eden-library-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directory(root);
    struct Cleanup { fs::path root; ~Cleanup() { fs::remove_all(root); } } cleanup{root};
    assert(EdenXbox::ScanGameLibrary(root).entries.empty());
    assert(!EdenXbox::ScanGameLibrary(root / "missing").error.empty());
    fs::create_directory(root / "nested");
    for (const auto& name : {"b.XCI", "a.nsp", "ignored.nca", "partial.nsp.tmp"})
        std::ofstream{root / name};
    std::ofstream{root / "nested" / "homebrew.NRO"};
    const auto scan = EdenXbox::ScanGameLibrary(root);
    assert(scan.entries.size() == 3 && scan.error.empty() && !scan.limited);
    assert(scan.entries[0].relative_path == "a.nsp");
    assert(scan.entries[1].relative_path == "b.XCI");
    assert(scan.entries[2].relative_path == fs::path{"nested"} / "homebrew.NRO");
    std::stop_source source;
    source.request_stop();
    assert(EdenXbox::ScanGameLibrary(root, source.get_token()).entries.empty());
    auto deep = root;
    for (int i = 0; i < 6; ++i) { deep /= "level"; fs::create_directory(deep); }
    std::ofstream{deep / "too-deep.nsp"};
    assert(EdenXbox::ScanGameLibrary(root).entries.size() == 3);
    std::error_code ec;
    fs::create_directory_symlink(root / "nested", root / "link", ec);
    if (!ec) assert(EdenXbox::ScanGameLibrary(root).entries.size() == 3);
    std::cout << "PASS library extensions, relative paths, nesting, cancellation, empty/missing and links\n";
}
