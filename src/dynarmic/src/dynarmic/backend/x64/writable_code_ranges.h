/* This file is part of the dynarmic project.
 * Copyright (c) 2026 eden-xbox contributors
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <cstddef>
#include <span>

namespace Dynarmic::Backend::X64 {

// Patch pages are sorted, unique and below the append window. Merge only touching
// ranges: never include an unchanged page, even when the gap is just one page.
template<typename Byte, typename Protect>
void RestoreWritableCodeRanges(std::span<const Byte* const> pages, const Byte* append_begin,
                               const Byte* append_end, std::size_t page_size, Protect&& protect) {
    const Byte* begin = nullptr;
    const Byte* end = nullptr;
    for (const Byte* page : pages) {
        if (begin && page != end) {
            protect(begin, static_cast<std::size_t>(end - begin));
            begin = nullptr;
        }
        if (!begin) {
            begin = page;
        }
        end = page + page_size;
    }
    if (append_begin < append_end) {
        if (begin && end == append_begin) {
            end = append_end;
        } else {
            if (begin) {
                protect(begin, static_cast<std::size_t>(end - begin));
            }
            begin = append_begin;
            end = append_end;
        }
    }
    if (begin) {
        protect(begin, static_cast<std::size_t>(end - begin));
    }
}

} // namespace Dynarmic::Backend::X64
