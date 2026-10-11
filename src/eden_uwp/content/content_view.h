// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "eden_uwp/content/content_session.h"
#include "eden_uwp/localization.h"
namespace EdenXbox { class UiPainter; }
namespace EdenXbox::Content {
I18n::Text ErrorText(Error error);
void DrawContentPanel(UiPainter& painter, const ContentSession& session, std::wstring_view game);
void DrawMissingContent(UiPainter& painter);
} // namespace EdenXbox::Content
