// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#include "eden_uwp/locales/catalogs.h"

namespace EdenXbox::I18n {
namespace {
// CLDR: English and Spanish use "one" for exactly 1 and "other" for everything else.
PluralForm OneOrOther(std::uint64_t count) {
    return count == 1 ? PluralForm::One : PluralForm::Other;
}

// English first: it is the fallback for any language and any missing text.
const std::array<UiLanguage, 2> languages{{
    {L"en", L"English", &EnglishCatalog, &OneOrOther},
    {L"es", L"Español", &SpanishCatalog, &OneOrOther},
}};
} // namespace

std::span<const UiLanguage> UiLanguages() {
    return languages;
}
} // namespace EdenXbox::I18n
