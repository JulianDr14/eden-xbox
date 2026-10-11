// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <memory>
#include "eden_uwp/content/content_session.h"
#include <string_view>
#include "eden_uwp/cheats/cheat_session.h"
#include "eden_uwp/cheats/cheats_page.h"
#include "eden_uwp/game_library.h"
#include "eden_uwp/game_options_page.h"
#include "eden_uwp/mods/mod_session.h"
#include "eden_uwp/mods/mods_page.h"
#include "eden_uwp/system_page.h"
#include "eden_uwp/uwp_input.h"
#include "eden_uwp/uwp_controllers.h"
#include "eden_uwp/uwp_file_manager.h"
namespace EdenXbox {
/// The selected game's options page, when open.
struct GameOptionsView {
    const GameOptionsPage& page;
    const LibraryEntry* game;            ///< nullptr when the page is closed.
    const Cheats::CheatSession* cheats;  ///< The open cheats page's.
    const Cheats::CheatsPage& cheats_page;
    std::wstring_view notice;
    const Content::ContentSession* content{};
    const Mods::ModSession* mods{};      ///< The open mods page's.
    const Mods::ModsPage* mods_page{};
};
/// Everything one frame of the library shows.
struct LibraryView {
    const LibraryScan& scan;
    std::size_t selected;
    bool loading;
    bool controller_panel;
    unsigned controller_row;
    const ControllerOptions& options;
    const std::wstring& notice;
    const ControllerPanel& panel;
    const ConfigurationPanel& configuration;
    const SystemPage& system;
    const GameOptionsView& game_options;
    bool missing_content{};
};
class LibraryCanvas {
public:
    LibraryCanvas(void* window, unsigned width, unsigned height);
    ~LibraryCanvas();
    /// Drops decoded covers, e.g. after the library cache is cleared.
    void InvalidateCovers();
    void Draw(const LibraryView& view);
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
