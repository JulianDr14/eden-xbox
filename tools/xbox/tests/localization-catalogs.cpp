// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
// Build with src/eden_uwp/localization.cpp and src/eden_uwp/locales/*.cpp.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include "eden_uwp/localization.h"

int main() {
    using namespace EdenXbox::I18n;
    auto check = [](bool ok) { if (!ok) std::abort(); };
    const auto languages = UiLanguages();
    check(languages.size() >= 2 && languages.front().tag == L"en");

    // Negotiation: exact, shortened, case/underscore-insensitive, in preference order, English last.
    auto negotiate = [&](std::vector<std::wstring> preferences) {
        return NegotiateUiLanguage(languages, preferences).tag;
    };
    check(negotiate({L"es-CO"}) == L"es");
    check(negotiate({L"es-419"}) == L"es");
    check(negotiate({L"ES_mx"}) == L"es");
    check(negotiate({L"fr-FR", L"es-ES", L"en-US"}) == L"es");
    check(negotiate({L"de-DE"}) == L"en");
    check(negotiate({}) == L"en");

    // Every catalog is complete (MakeCatalog enforces it at compile time; this guards the registry).
    for (const auto& language : languages)
        for (std::size_t i = 0; i < TextCount; ++i) check(!(*language.catalog)[i].empty());

    SetUiLanguage(*FindUiLanguage(L"es"));
    check(Tr(Text::Play) == L"Jugar");
    check(Plural(Text::GamesCountOne, Text::GamesCountOther, 1) == L"1 juego");
    check(Plural(Text::GamesCountOne, Text::GamesCountOther, 0) == L"0 juegos");
    check(Format(Text::Importing, {L"3", L"9"}) == L"Importando 3 / 9 archivos...");
    SetUiLanguage(*FindUiLanguage(L"en"));
    check(Plural(Text::GamesCountOne, Text::GamesCountOther, 2) == L"2 games");
    check(ToUtf8(L"Ñ€😀") == "\xC3\x91\xE2\x82\xAC\xF0\x9F\x98\x80");

    std::puts("localization: negotiation, catalogs, plurals, placeholders and UTF-8 PASS");
}
