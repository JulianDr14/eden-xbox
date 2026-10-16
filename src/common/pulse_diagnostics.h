// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Pulse's deep diagnostics: GPU VA and shader code region tracking, the write watch on its DRAM
// pages, the restored page hashes and the GPU mapping checks around a save and a resume. They
// found the restored VA allocations being handed out again; kept for the next bug of the kind.
// Off by default (some scan every page of the session): boot.cfg "pulse_diag=1" turns them on
// before boot.

#include <atomic>

namespace Common::Pulse {

inline std::atomic<bool> g_diagnostics{false};

[[nodiscard]] inline bool Diagnostics() {
    return g_diagnostics.load(std::memory_order_relaxed);
}

inline void SetDiagnostics(bool enabled) {
    g_diagnostics.store(enabled, std::memory_order_relaxed);
}

} // namespace Common::Pulse
