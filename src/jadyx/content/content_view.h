// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "jadyx/content/content_session.h"
#include "jadyx/localization.h"
namespace Jadyx { class UiPainter; }
namespace Jadyx::Content {
I18n::Text ErrorText(Error error);
void DrawContentPanel(UiPainter& painter, const ContentSession& session, std::wstring_view game);
void DrawMissingContent(UiPainter& painter);
} // namespace Jadyx::Content
