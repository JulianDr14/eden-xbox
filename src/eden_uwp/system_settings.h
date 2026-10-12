// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The "System" settings: user name, interface language, game language and console region.
// Each choice is either explicit or follows the console/PC (Windows language and home region),
// which is the default. Pure logic only: persistence and OS queries are in uwp_system_settings.h.

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common/settings_enums.h"
#include "eden_uwp/localization.h"
#include "eden_uwp/upscaling.h"

namespace EdenXbox {

/// The Switch shows user names of up to 10 characters; games lay them out for that.
inline constexpr std::size_t MaxUserNameLength = 10;
inline constexpr std::wstring_view DefaultUserName = L"Eden";

struct SystemSettings {
    std::wstring ui_language;                         ///< BCP 47 tag; empty follows the system.
    std::optional<Settings::Language> game_language;  ///< nullopt follows the system.
    std::optional<Settings::Region> region;           ///< nullopt follows the system.
    std::wstring user_name;                           ///< Empty keeps the default name.
    bool show_performance_stats{}; ///< Persistent global HUD preference, off on first install.
    bool show_adult_mods{};                           ///< The mods store lists mods rated for adults.
    bool pulse_sessions{true}; ///< Back to home keeps the game where it is (pulse_sessions.h).
    Upscaling upscaling{Upscaling::Bilinear};         ///< Set from the in-game menu.

    bool operator==(const SystemSettings&) const = default;
};

/// What Windows reports: preferred languages in order ("es-CO", "en-US") and the home region as
/// an ISO 3166 code ("CO"), which may be empty.
struct SystemLocale {
    std::vector<std::wstring> languages;
    std::wstring region;
};

/// Settings with every "follow the system" choice resolved.
struct ResolvedSystem {
    const I18n::UiLanguage* ui_language{};
    Settings::Language game_language{Settings::Language::EnglishAmerican};
    Settings::Region region{Settings::Region::Usa};
};

namespace SystemDetail {
inline std::wstring Upper(std::wstring_view text) {
    std::wstring out{text};
    for (auto& c : out)
        if (c >= L'a' && c <= L'z') c = static_cast<wchar_t>(c - L'a' + L'A');
    return out;
}
inline std::wstring Lower(std::wstring_view text) {
    std::wstring out{text};
    for (auto& c : out)
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    return out;
}
/// Subtags of a BCP 47 tag: "zh-Hant-TW" -> {"zh", "hant", "tw"} (lower case).
inline std::vector<std::wstring> Subtags(std::wstring_view tag) {
    std::vector<std::wstring> parts;
    while (!tag.empty()) {
        const auto dash = tag.find_first_of(L"-_");
        parts.push_back(Lower(tag.substr(0, dash)));
        if (dash == std::wstring_view::npos) break;
        tag.remove_prefix(dash + 1);
    }
    return parts;
}
/// The region subtag of a tag ("es-CO" -> "CO", "es-419" -> "419"), or empty.
inline std::wstring TagRegion(std::wstring_view tag) {
    const auto parts = Subtags(tag);
    for (std::size_t i = 1; i < parts.size(); ++i)
        if (parts[i].size() == 2 || (parts[i].size() == 3 && parts[i][0] >= L'0' && parts[i][0] <= L'9'))
            return Upper(parts[i]);
    return {};
}
inline bool IsAmericas(std::wstring_view region) {
    static constexpr std::array<std::wstring_view, 36> americas{
        L"419", L"AG", L"AR", L"BB", L"BO", L"BR", L"BS", L"BZ", L"CA", L"CL", L"CO", L"CR",
        L"CU", L"DM", L"DO", L"EC", L"GD", L"GT", L"GY", L"HN", L"HT", L"JM", L"KN", L"LC",
        L"MX", L"NI", L"PA", L"PE", L"PR", L"PY", L"SR", L"SV", L"TT", L"US", L"UY", L"VE"};
    return std::find(americas.begin(), americas.end(), region) != americas.end();
}
} // namespace SystemDetail

/// The region to use: the home region, else the first language's region subtag.
inline std::wstring EffectiveRegionCode(const SystemLocale& locale) {
    if (!locale.region.empty()) return SystemDetail::Upper(locale.region);
    return locale.languages.empty() ? std::wstring{} : SystemDetail::TagRegion(locale.languages.front());
}

/// Switch region for a country: the Americas, Japan, China, Korea, Taiwan/Hong Kong and
/// Australia/New Zealand have their own; everywhere else uses Europe's, as on the console.
inline Settings::Region RegionForLocale(const SystemLocale& locale) {
    const auto code = EffectiveRegionCode(locale);
    if (code.empty() || SystemDetail::IsAmericas(code)) return Settings::Region::Usa;
    if (code == L"JP") return Settings::Region::Japan;
    if (code == L"CN") return Settings::Region::China;
    if (code == L"KR") return Settings::Region::Korea;
    if (code == L"TW" || code == L"HK" || code == L"MO") return Settings::Region::Taiwan;
    if (code == L"AU" || code == L"NZ") return Settings::Region::Australia;
    return Settings::Region::Europe;
}

/// Switch system language for the first Windows language, with the variant its region uses:
/// Spanish from Spain or Latin America, American or British English, and so on.
inline Settings::Language GameLanguageForLocale(const SystemLocale& locale) {
    using L = Settings::Language;
    const std::wstring_view tag = locale.languages.empty() ? std::wstring_view{} : locale.languages.front();
    const auto parts = SystemDetail::Subtags(tag);
    const std::wstring language = parts.empty() ? L"en" : parts.front();
    const auto tag_region = SystemDetail::TagRegion(tag);
    const auto region = tag_region.empty() ? EffectiveRegionCode(locale) : tag_region;
    const bool americas = region.empty() || SystemDetail::IsAmericas(region);
    if (language == L"es") return region == L"ES" ? L::Spanish : L::SpanishLatin;
    if (language == L"fr") return region == L"CA" ? L::FrenchCanadian : L::French;
    if (language == L"pt") return region == L"BR" ? L::PortugueseBrazilian : L::Portuguese;
    if (language == L"zh") {
        const bool traditional = std::find(parts.begin(), parts.end(), L"hant") != parts.end() ||
                                 region == L"TW" || region == L"HK" || region == L"MO";
        return traditional ? L::ChineseTraditional : L::ChineseSimplified;
    }
    if (language == L"ja") return L::Japanese;
    if (language == L"de") return L::German;
    if (language == L"it") return L::Italian;
    if (language == L"nl") return L::Dutch;
    if (language == L"ru") return L::Russian;
    if (language == L"ko") return L::Korean;
    if (language == L"pl") return L::Polish;
    if (language == L"th") return L::Thai;
    return americas ? L::EnglishAmerican : L::EnglishBritish;
}

inline ResolvedSystem ResolveSystem(const SystemSettings& settings, const SystemLocale& locale) {
    ResolvedSystem resolved;
    resolved.ui_language = settings.ui_language.empty() ? nullptr : I18n::FindUiLanguage(settings.ui_language);
    if (!resolved.ui_language)
        resolved.ui_language = &I18n::NegotiateUiLanguage(I18n::UiLanguages(), locale.languages);
    resolved.game_language = settings.game_language.value_or(GameLanguageForLocale(locale));
    resolved.region = settings.region.value_or(RegionForLocale(locale));
    return resolved;
}

/// The languages a player can pick, in the order of the Switch's own list. Eden's legacy
/// aliases (Chinese, Taiwanese) map to the same entries as the simplified/traditional ones.
inline std::span<const Settings::Language> SelectableGameLanguages() {
    using L = Settings::Language;
    static constexpr std::array<L, 18> languages{
        L::Japanese, L::EnglishAmerican, L::EnglishBritish, L::French, L::FrenchCanadian,
        L::German, L::Italian, L::Spanish, L::SpanishLatin, L::Dutch, L::Portuguese,
        L::PortugueseBrazilian, L::Russian, L::Korean, L::ChineseSimplified,
        L::ChineseTraditional, L::Polish, L::Thai};
    return languages;
}

inline std::span<const Settings::Region> SelectableRegions() {
    using R = Settings::Region;
    static constexpr std::array<R, 7> regions{R::Usa, R::Europe, R::Japan, R::Australia,
                                              R::China, R::Korea, R::Taiwan};
    return regions;
}

/// Each language by its own name, as the Switch lists them; no translation needed.
inline std::wstring_view GameLanguageName(Settings::Language language) {
    using L = Settings::Language;
    switch (language) {
    case L::Japanese:            return L"日本語";
    case L::EnglishAmerican:     return L"English (US)";
    case L::EnglishBritish:      return L"English (UK)";
    case L::French:              return L"Français";
    case L::FrenchCanadian:      return L"Français (Canada)";
    case L::German:              return L"Deutsch";
    case L::Italian:             return L"Italiano";
    case L::Spanish:             return L"Español (España)";
    case L::SpanishLatin:        return L"Español (Latinoamérica)";
    case L::Dutch:               return L"Nederlands";
    case L::Portuguese:          return L"Português";
    case L::PortugueseBrazilian: return L"Português (Brasil)";
    case L::Russian:             return L"Русский";
    case L::Korean:              return L"한국어";
    case L::Chinese:
    case L::ChineseSimplified:   return L"简体中文";
    case L::Taiwanese:
    case L::ChineseTraditional:  return L"繁體中文";
    case L::Polish:              return L"Polski";
    case L::Thai:                return L"ไทย";
    }
    return L"English (US)";
}

inline I18n::Text RegionText(Settings::Region region) {
    using R = Settings::Region;
    switch (region) {
    case R::Japan:     return I18n::Text::RegionJapan;
    case R::Usa:       return I18n::Text::RegionAmericas;
    case R::Europe:    return I18n::Text::RegionEurope;
    case R::Australia: return I18n::Text::RegionAustralia;
    case R::China:     return I18n::Text::RegionChina;
    case R::Korea:     return I18n::Text::RegionKorea;
    case R::Taiwan:    return I18n::Text::RegionTaiwan;
    }
    return I18n::Text::RegionAmericas;
}

/// Languages a game falls back to when it lacks the preferred one, closest first.
inline std::span<const Settings::Language> GameLanguageFallbacks(Settings::Language preferred) {
    using L = Settings::Language;
    static constexpr std::array<L, 1> latin{L::Spanish}, spain{L::SpanishLatin},
        british{L::EnglishAmerican}, american{L::EnglishBritish}, canadian{L::French},
        french{L::FrenchCanadian}, brazilian{L::Portuguese}, portuguese{L::PortugueseBrazilian},
        simplified{L::ChineseTraditional}, traditional{L::ChineseSimplified};
    switch (preferred) {
    case L::SpanishLatin:        return latin;
    case L::Spanish:             return spain;
    case L::EnglishBritish:      return british;
    case L::EnglishAmerican:     return american;
    case L::FrenchCanadian:      return canadian;
    case L::French:              return french;
    case L::PortugueseBrazilian: return brazilian;
    case L::Portuguese:          return portuguese;
    case L::Chinese:
    case L::ChineseSimplified:   return simplified;
    case L::Taiwanese:
    case L::ChineseTraditional:  return traditional;
    default:                     return {};
    }
}

/// The language to give a game that declares `supports`: the preferred one, its closest variant,
/// English, or else the first language it has. A game that declares nothing gets the preferred.
template <typename Supports>
Settings::Language PickGameLanguage(Settings::Language preferred, Supports&& supports) {
    using L = Settings::Language;
    bool any = false;
    for (const auto language : SelectableGameLanguages()) any = any || supports(language);
    if (!any || supports(preferred)) return preferred;
    for (const auto language : GameLanguageFallbacks(preferred))
        if (supports(language)) return language;
    for (const auto language : {L::EnglishAmerican, L::EnglishBritish})
        if (supports(language)) return language;
    for (const auto language : SelectableGameLanguages())
        if (supports(language)) return language;
    return preferred;
}

/// Steps an optional choice through "follow the system" (nullopt) and then every option.
template <typename T>
std::optional<T> StepChoice(std::optional<T> current, std::span<const T> options, bool back) {
    const std::size_t count = options.size() + 1; // Slot 0 is "follow the system".
    std::size_t slot = 0;
    if (current) {
        const auto found = std::find(options.begin(), options.end(), *current);
        slot = found == options.end() ? 0 : static_cast<std::size_t>(found - options.begin()) + 1;
    }
    slot = (slot + (back ? count - 1 : 1)) % count;
    return slot == 0 ? std::nullopt : std::optional<T>{options[slot - 1]};
}

/// Trims spaces, drops control characters and caps the length (counting UTF-16 units, so a
/// surrogate pair counts twice and is never split).
inline std::wstring SanitizeUserName(std::wstring_view name) {
    std::wstring out;
    for (const wchar_t c : name)
        if (c >= 0x20 && c != 0x7F) out += c;
    const auto first = out.find_first_not_of(L' ');
    if (first == std::wstring::npos) return {};
    out = out.substr(first, out.find_last_not_of(L' ') - first + 1);
    if (out.size() > MaxUserNameLength) {
        std::size_t cut = MaxUserNameLength;
        if (out[cut - 1] >= 0xD800 && out[cut - 1] <= 0xDBFF) --cut;
        out.resize(cut);
        while (!out.empty() && out.back() == L' ') out.pop_back();
    }
    return out;
}

} // namespace EdenXbox
