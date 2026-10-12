// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Pulse checkpoints of am (pulse_service.h). The application's interfaces hold no state of their
// own: each references the appletOE root, which stores the application applet's state, exports
// the applet events they hand out and rebuilds them (IApplicationProxyService::PulseRestore).

#include "core/hle/service/pulse_service.h"

namespace Service::AM {

// Writes the reference to the appletOE root, so it is captured and restored first.
void PulseReferenceRoot(Core::System& system, Pulse::HandlerCapture& capture);

} // namespace Service::AM
