// SPDX-FileCopyrightText: Copyright 2026 eden-xbox contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cstdint>
#include <cstdio>
#include <vector>
#include <windows.h>
#include "dynarmic/backend/x64/writable_code_ranges.h"

int main() {
    constexpr size_t page_size = 4096;
    constexpr size_t page_count = 16;
    auto* memory = static_cast<const std::uint8_t*>(
        VirtualAlloc(nullptr, page_count * page_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!memory) return 1;
    size_t cases = 0;
    for (size_t append = 0; append <= 12; ++append) {
        for (size_t mask = 0; mask < (size_t{1} << append); ++mask) {
            for (size_t tail : {size_t{0}, size_t{1}, size_t{3}}) {
                std::vector<const std::uint8_t*> pages;
                std::array<bool, page_count> expected{}, actual{};
                for (size_t p = 0; p < append; ++p) {
                    if ((mask >> p) & 1) {
                        pages.push_back(memory + p * page_size);
                        expected[p] = true;
                    }
                }
                for (size_t p = append; p < append + tail; ++p) expected[p] = true;
                size_t calls = 0;
                bool valid = true;
                Dynarmic::Backend::X64::RestoreWritableCodeRanges<std::uint8_t>(
                    pages, memory + append * page_size, memory + (append + tail) * page_size,
                    page_size, [&](const auto* begin, size_t size) {
                        ++calls;
                        const size_t offset = static_cast<size_t>(begin - memory);
                        valid &= size != 0 && offset % page_size == 0 && size % page_size == 0;
                        valid &= offset + size <= page_count * page_size;
                        if (!valid) return;
                        for (size_t p = offset / page_size; p < (offset + size) / page_size; ++p) {
                            valid &= !actual[p];
                            actual[p] = true;
                        }
                    });
                size_t expected_calls = 0;
                for (size_t p = 0; p < page_count; ++p)
                    expected_calls += expected[p] && (p == 0 || !expected[p-1]);
                if (!valid || actual != expected || calls != expected_calls) return 2;
                ++cases;
            }
        }
    }
    // Real Windows protection gate: separated patch pages, a touching pair and append window.
    DWORD old = 0;
    if (!VirtualProtect(const_cast<std::uint8_t*>(memory), page_count * page_size, PAGE_READONLY, &old)) return 3;
    std::vector<const std::uint8_t*> pages{memory + page_size, memory + 3*page_size,
                                         memory + 4*page_size, memory + 7*page_size};
    const auto protect = [&](const auto* begin, size_t size, DWORD flags) {
        return VirtualProtect(const_cast<std::uint8_t*>(begin), size, flags, &old) != 0;
    };
    for (const auto* page : pages) if (!protect(page, page_size, PAGE_READWRITE)) return 4;
    if (!protect(memory + 8*page_size, 2*page_size, PAGE_READWRITE)) return 4;
    bool success = true;
    Dynarmic::Backend::X64::RestoreWritableCodeRanges<std::uint8_t>(
        pages, memory + 8*page_size, memory + 10*page_size, page_size,
        [&](const auto* begin, size_t size) { success &= protect(begin, size, PAGE_EXECUTE_READ); });
    for (size_t p = 0; p < page_count; ++p) {
        MEMORY_BASIC_INFORMATION info{};
        const bool changed = p == 1 || p == 3 || p == 4 || p == 7 || p == 8 || p == 9;
        success &= VirtualQuery(memory + p*page_size, &info, sizeof(info)) != 0;
        success &= info.Protect == (changed ? PAGE_EXECUTE_READ : PAGE_READONLY);
    }
    VirtualFree(const_cast<std::uint8_t*>(memory), 0, MEM_RELEASE);
    if (!success) return 5;
    std::printf("PASS: %zu range cases; real Windows RX/unchanged-page protection gate\n", cases);
}
