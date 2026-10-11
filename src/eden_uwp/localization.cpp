// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#include "eden_uwp/localization.h"

#include <atomic>
#include <cwctype>

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

wchar_t FoldAccent(wchar_t c) {
    switch (c) {
    case L'Á': case L'À': case L'Â': case L'Ä': case L'á': case L'à': case L'â': case L'ä': return L'A';
    case L'É': case L'È': case L'Ê': case L'Ë': case L'é': case L'è': case L'ê': case L'ë': return L'E';
    case L'Í': case L'Ì': case L'Î': case L'Ï': case L'í': case L'ì': case L'î': case L'ï': return L'I';
    case L'Ó': case L'Ò': case L'Ô': case L'Ö': case L'ó': case L'ò': case L'ô': case L'ö': return L'O';
    case L'Ú': case L'Ù': case L'Û': case L'Ü': case L'ú': case L'ù': case L'û': case L'ü': return L'U';
    case L'Ñ': case L'ñ': return L'N';
    case L'Ç': case L'ç': return L'C';
    default: return c;
    }
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
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        char32_t c = text[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < text.size() && text[i + 1] >= 0xDC00 &&
            text[i + 1] <= 0xDFFF) {
            c = 0x10000 + ((c - 0xD800) << 10) + (text[++i] - 0xDC00);
        } else if (c >= 0xD800 && c <= 0xDFFF) {
            c = 0xFFFD; // Lone surrogate.
        }
        if (c < 0x80) {
            out += static_cast<char>(c);
        } else if (c < 0x800) {
            out += static_cast<char>(0xC0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += static_cast<char>(0xE0 | (c >> 12));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (c >> 18));
            out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    return out;
}

std::string ToOverlayText(std::wstring_view text) {
    std::string out;
    out.reserve(text.size());
    for (const wchar_t raw : text) {
        wchar_t c = FoldAccent(raw);
        if (c >= L'a' && c <= L'z') c = static_cast<wchar_t>(c - L'a' + L'A');
        const bool drawable = (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') ||
                              std::wstring_view{L" /.%-:<>"}.find(c) != std::wstring_view::npos;
        out += drawable ? static_cast<char>(c) : ' ';
    }
    return out;
}

} // namespace EdenXbox::I18n
