// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>
#include "core/file_sys/mod_priority.h"

namespace {
using Dir = std::shared_ptr<std::string>;

std::vector<Dir> Dirs(std::vector<std::string> names) {
    std::vector<Dir> out;
    for (auto& name : names) out.push_back(std::make_shared<std::string>(std::move(name)));
    return out;
}

std::vector<std::string> Sorted(std::vector<Dir> dirs, const std::vector<std::string>& priority) {
    FileSys::SortByModPriority(dirs, priority, [](const Dir& dir) { return *dir; });
    std::vector<std::string> out;
    for (const auto& dir : dirs) out.push_back(*dir);
    return out;
}

void Check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        std::fflush(stdout);
        std::abort();
    }
}
} // namespace

int main() {
    using V = std::vector<std::string>;
    // No list: the plain name order mods always had (byte order, digits before letters).
    Check(Sorted(Dirs({"b", "A", "01", "a"}), {}) == V{"01", "A", "a", "b"}, "name order");
    // Named folders first, in list order; the rest follow by name.
    Check(Sorted(Dirs({"a", "b", "c", "d"}), {"c", "a"}) == V{"c", "a", "b", "d"}, "priority");
    // Names in the list with no folder are ignored.
    Check(Sorted(Dirs({"x", "y"}), {"gone", "y"}) == V{"y", "x"}, "stale names");
    // Missing roots (no SD card, no load folder) are dropped instead of dereferenced.
    auto with_null = Dirs({"b", "a"});
    with_null.insert(with_null.begin() + 1, nullptr);
    Check(Sorted(with_null, {}) == V{"a", "b"}, "null roots");
    Check(Sorted({}, {"a"}).empty(), "empty");
    std::puts("mod-priority PASS");
}
