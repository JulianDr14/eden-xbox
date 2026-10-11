// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#include "eden_uwp/localization.h"

#include <atomic>
#include <cwctype>

#include "eden_uwp/utf8.h"

namespace EdenXbox::I18n {
namespace {
std::atomic<const UiLanguage*> current{nullptr};

bool SameTag(std::wstring_view a, std::wstring_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto x = a[i] == L'_' ? L'-' : std::towlower(a[i]);
        const auto y = b[i] == L'_' ? L'-' : std::towlower(b[i]);
        if (x != y) return false;
    }
    return true;
}

const UiLanguage* Find(std::span<const UiLanguage> available, std::wstring_view tag) {
    for (const auto& language : available)
        if (SameTag(language.tag, tag)) return &language;
    return nullptr;
}
} // namespace

const UiLanguage* FindUiLanguage(std::wstring_view tag) {
    return Find(UiLanguages(), tag);
}

const UiLanguage& NegotiateUiLanguage(std::span<const UiLanguage> available,
                                      std::span<const std::wstring> preferences) {
    for (std::wstring_view tag : preferences) {
        // Drop trailing subtags one at a time: "es-Latn-CO" -> "es-Latn" -> "es".
        while (!tag.empty()) {
            if (const auto* found = Find(available, tag)) return *found;
            const auto dash = tag.find_last_of(L"-_");
            if (dash == std::wstring_view::npos) break;
            tag = tag.substr(0, dash);
        }
    }
    return available.front();
}

void SetUiLanguage(const UiLanguage& language) {
    current.store(&language, std::memory_order_release);
}

const UiLanguage& CurrentUiLanguage() {
    const auto* language = current.load(std::memory_order_acquire);
    return language ? *language : UiLanguages().front();
}

std::wstring_view Tr(Text id) {
    const auto index = static_cast<std::size_t>(id);
    if (index >= TextCount) return {};
    const auto text = (*CurrentUiLanguage().catalog)[index];
    return text.empty() ? (*UiLanguages().front().catalog)[index] : text;
}

std::wstring Format(Text id, std::initializer_list<std::wstring_view> args) {
    const auto pattern = Tr(id);
    std::wstring result;
    result.reserve(pattern.size() + 16);
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        // "{n}" with a single digit; anything else is literal text.
        if (pattern[i] == L'{' && i + 2 < pattern.size() && pattern[i + 2] == L'}' &&
            pattern[i + 1] >= L'0' && pattern[i + 1] <= L'9') {
            const auto arg = static_cast<std::size_t>(pattern[i + 1] - L'0');
            if (arg < args.size()) result += *(args.begin() + arg);
            i += 2;
            continue;
        }
        result += pattern[i];
    }
    return result;
}

std::wstring Plural(Text one, Text other, std::uint64_t count) {
    const auto form = CurrentUiLanguage().plural(count);
    const auto number = std::to_wstring(count);
    return Format(form == PluralForm::One ? one : other, {number});
}

std::string ToUtf8(std::wstring_view text) {
    return Utf16ToUtf8(text);
}

} // namespace EdenXbox::I18n
