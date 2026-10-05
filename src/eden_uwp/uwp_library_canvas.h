// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <memory>
#include "eden_uwp/game_library.h"
#include "eden_uwp/uwp_input.h"
#include "eden_uwp/uwp_controllers.h"
#include "eden_uwp/uwp_file_manager.h"
namespace EdenXbox {
class LibraryCanvas {
public:
    LibraryCanvas(void* window, unsigned width, unsigned height);
    ~LibraryCanvas();
    void InvalidateCovers();
    void Draw(const LibraryScan&, size_t, bool, bool, unsigned,
              const ControllerOptions&, const std::wstring&, const ControllerPanel&,
              const ConfigurationPanel&);
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
