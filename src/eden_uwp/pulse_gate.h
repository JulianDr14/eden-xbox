// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>
#include <string>
#include "common/common_types.h"

namespace EdenXbox {

struct PulseGateOptions {
    u32 cycles{};      ///< boot.cfg "pulse_cycles=": capture/destroy/restore cycles to run
    u32 seed{1};       ///< "pulse_seed=": the random delays between captures
    void* core_window{};
    u32 width{1280};
    u32 height{720};
    std::function<void(const std::string&)> diag;
};

/// Pulse phase-2 gate for tools/xbox/pulse_nro: boots the homebrew, then repeatedly captures it at
/// a random moment, publishes the archive, destroys the System, restores into a fresh one and
/// checks the homebrew carries on from exactly where it was (its next heartbeat, no PULSE_FAIL).
/// Returns 0 when every cycle passed.
int RunPulseGate(const std::string& nro_path, const PulseGateOptions& options);

} // namespace EdenXbox
