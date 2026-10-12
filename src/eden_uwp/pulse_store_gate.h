// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>
#include <string>

namespace EdenXbox {

struct PulseStoreGateOptions {
    std::function<void(const std::string&)> diag;
    /// Ends this process without unwinding and starts the app again (CoreApplication restart).
    /// Must not return; the gate resumes from its stage file on the next launch.
    std::function<void()> restart;
};

enum class PulseStoreGateResult {
    Pass, ///< every stage passed, on this launch or across the earlier ones
    Fail,
};

/// Pulse phase-1 gate (boot.cfg "pulse_store=1"): the Store in the package's own LocalState, inside
/// the AppContainer. One launch runs publications, injected faults, quota, cancellation,
/// catalogue, restore/consume and discard. Then, launch by launch, it ends the process at each
/// durable publication and consumption boundary (no unwinding, as a crash or a power loss would)
/// and checks on the next launch that the previous generation survived and the orphan was
/// recovered. Progress lives in a stage file next to the store, so the gate resumes after each
/// restart.
PulseStoreGateResult RunPulseStoreGate(const PulseStoreGateOptions& options);

} // namespace EdenXbox
