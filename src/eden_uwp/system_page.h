// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Settings > System: the page's rows, what each shows and how input changes them. The library
// loop routes input here and acts on the returned effect (save, clear the cache, ...); the canvas
// draws from the same state and the layout below, which pointer hit tests share.

#include <optional>
#include <string>

#include "eden_uwp/localization.h"
#include "eden_uwp/system_settings.h"
#include "eden_uwp/text_entry.h"
#include "eden_uwp/text_entry_panel.h"

namespace EdenXbox {

struct SystemPage {
    enum Row : std::size_t { UserName, InterfaceLanguage, GameLanguage, Region, AdultMods, PerformanceStats, PulseSessions, ClearCache, RowCount };

    bool open{};
    std::size_t selected{};
    SystemSettings settings; ///< As saved.
    SystemLocale locale;     ///< As Windows reports it, for the "Automatic (...)" values.
    std::wstring notice;
    std::optional<TextEntry> name_editor;
    std::wstring editor_notice;
};

enum class SystemPageEffect : std::uint8_t {
    None,
    Changed,    ///< Settings changed: save and apply them.
    EditName,   ///< The name editor opened.
    NameDone,   ///< The editor closed with a new name in settings: save and apply.
    ClearCache, ///< Clear the library cache.
};

enum class SystemInput : std::uint8_t { Up, Down, Left, Right, Confirm, Back };

namespace SystemPageDetail {
inline std::optional<std::wstring> StepUiLanguage(const std::wstring& current, bool back) {
    const auto languages = I18n::UiLanguages();
    const std::size_t count = languages.size() + 1;
    std::size_t slot = 0;
    for (std::size_t i = 0; i < languages.size(); ++i)
        if (languages[i].tag == current) slot = i + 1;
    slot = (slot + (back ? count - 1 : 1)) % count;
    return slot == 0 ? std::wstring{} : std::wstring{languages[slot - 1].tag};
}
} // namespace SystemPageDetail

/// Applies one input to the page (not to the name editor, which the library drives directly).
inline SystemPageEffect HandleSystemInput(SystemPage& page, SystemInput input) {
    using Row = SystemPage::Row;
    switch (input) {
    case SystemInput::Up:
        page.selected = (page.selected + Row::RowCount - 1) % Row::RowCount;
        return SystemPageEffect::None;
    case SystemInput::Down:
        page.selected = (page.selected + 1) % Row::RowCount;
        return SystemPageEffect::None;
    case SystemInput::Back:
        page.open = false;
        return SystemPageEffect::None;
    case SystemInput::Left:
    case SystemInput::Right:
    case SystemInput::Confirm:
        break;
    }
    const bool back = input == SystemInput::Left;
    auto& settings = page.settings;
    switch (static_cast<Row>(page.selected)) {
    case Row::UserName:
        if (input != SystemInput::Confirm) return SystemPageEffect::None;
        page.name_editor.emplace(settings.user_name, MaxUserNameLength);
        page.editor_notice.clear();
        return SystemPageEffect::EditName;
    case Row::InterfaceLanguage:
        settings.ui_language = *SystemPageDetail::StepUiLanguage(settings.ui_language, back);
        return SystemPageEffect::Changed;
    case Row::GameLanguage:
        settings.game_language = StepChoice(settings.game_language, SelectableGameLanguages(), back);
        return SystemPageEffect::Changed;
    case Row::Region:
        settings.region = StepChoice(settings.region, SelectableRegions(), back);
        return SystemPageEffect::Changed;
    case Row::AdultMods:
        settings.show_adult_mods = !settings.show_adult_mods;
        return SystemPageEffect::Changed;
    case Row::PerformanceStats:
        settings.show_performance_stats = !settings.show_performance_stats;
        return SystemPageEffect::Changed;
    case Row::PulseSessions:
        settings.pulse_sessions = !settings.pulse_sessions;
        return SystemPageEffect::Changed;
    case Row::ClearCache:
        return input == SystemInput::Confirm ? SystemPageEffect::ClearCache : SystemPageEffect::None;
    case Row::RowCount:
        break;
    }
    return SystemPageEffect::None;
}

/// Closes the name editor; `accept` keeps its text. An empty name is refused with a notice.
inline SystemPageEffect FinishNameEditor(SystemPage& page, bool accept) {
    if (!page.name_editor) return SystemPageEffect::None;
    if (!accept) {
        page.name_editor.reset();
        return SystemPageEffect::None;
    }
    auto name = SanitizeUserName(page.name_editor->Value());
    if (name.empty()) {
        page.editor_notice = I18n::Tr(I18n::Text::NameEmpty);
        return SystemPageEffect::None;
    }
    page.name_editor.reset();
    // The default name is stored as "no name": it then follows any future default.
    page.settings.user_name = name == DefaultUserName ? std::wstring{} : std::move(name);
    return SystemPageEffect::NameDone;
}

inline I18n::Text SystemRowTitle(SystemPage::Row row) {
    using I18n::Text;
    switch (row) {
    case SystemPage::UserName:          return Text::UserName;
    case SystemPage::InterfaceLanguage: return Text::InterfaceLanguage;
    case SystemPage::GameLanguage:      return Text::GameLanguage;
    case SystemPage::Region:            return Text::ConsoleRegion;
    case SystemPage::PerformanceStats:  return Text::PerformanceStats;
    case SystemPage::AdultMods:         return Text::ModsAdult;
    case SystemPage::PulseSessions:     return Text::PulseSessions;
    default:                            return Text::ClearLibraryCache;
    }
}

inline I18n::Text SystemRowHint(SystemPage::Row row) {
    using I18n::Text;
    switch (row) {
    case SystemPage::UserName:          return Text::UserNameHint;
    case SystemPage::InterfaceLanguage: return Text::InterfaceLanguageHint;
    case SystemPage::GameLanguage:      return Text::GameLanguageHint;
    case SystemPage::Region:            return Text::ConsoleRegionHint;
    case SystemPage::PerformanceStats:  return Text::PerformanceStatsHint;
    case SystemPage::AdultMods:         return Text::ModsAdultHint;
    case SystemPage::PulseSessions:     return Text::PulseSessionsHint;
    default:                            return Text::ClearLibraryCacheHint;
    }
}

/// The row's current value; choices that follow the system read "Automatic · <what it is>".
inline std::wstring SystemRowValue(const SystemPage& page, SystemPage::Row row) {
    using I18n::Text;
    const auto& settings = page.settings;
    const auto automatic = [](std::wstring_view value) {
        return I18n::Format(Text::AutomaticValue, {value});
    };
    switch (row) {
    case SystemPage::UserName:
        return settings.user_name.empty() ? std::wstring{DefaultUserName} : settings.user_name;
    case SystemPage::InterfaceLanguage:
        if (const auto* language = I18n::FindUiLanguage(settings.ui_language))
            return std::wstring{language->native_name};
        return automatic(I18n::NegotiateUiLanguage(I18n::UiLanguages(), page.locale.languages).native_name);
    case SystemPage::GameLanguage:
        if (settings.game_language) return std::wstring{GameLanguageName(*settings.game_language)};
        return automatic(GameLanguageName(GameLanguageForLocale(page.locale)));
    case SystemPage::Region:
        if (settings.region) return std::wstring{I18n::Tr(RegionText(*settings.region))};
        return automatic(I18n::Tr(RegionText(RegionForLocale(page.locale))));
    case SystemPage::PerformanceStats:
        return std::wstring{I18n::Tr(settings.show_performance_stats ? Text::Yes : Text::No)};
    case SystemPage::AdultMods:
        return std::wstring{I18n::Tr(settings.show_adult_mods ? Text::ModsAdultShown : Text::ModsAdultHidden)};
    case SystemPage::PulseSessions:
        return std::wstring{I18n::Tr(settings.pulse_sessions ? Text::Yes : Text::No)};
    default:
        return {};
    }
}

/// Geometry shared by the canvas and pointer hit tests (1280x720 layout units).
struct SystemPageLayout {
    static constexpr float PanelX = 140, PanelY = 72, PanelWidth = 1000, PanelHeight = 612;
    static constexpr float RowX = 180, RowWidth = 920, RowTop = 172, RowStride = 52, RowHeight = 50;
    static constexpr float Notice = 594, Footer = 630;
    /// The name editor's keyboard panel (text_entry_panel.h), centered on the screen.
    static constexpr float EditorX = (1280 - TextEntryPanelLayout::Width) / 2;
    static constexpr float EditorY = (720 - TextEntryPanelLayout::Height) / 2;
};

} // namespace EdenXbox
