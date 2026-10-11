// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <memory>
#include "eden_uwp/game_library.h"
#include "eden_uwp/system_page.h"
#include "eden_uwp/uwp_input.h"
#include "eden_uwp/uwp_controllers.h"
#include "eden_uwp/uwp_file_manager.h"
namespace EdenXbox {
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
