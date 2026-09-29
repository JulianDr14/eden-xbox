// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/common_types.h"

/// A timeline of the frame chain for a few vsyncs: which host thread did what, and when. The perf
/// counters sum each link over 300 frames, which cannot tell who waits for whom when waits of
/// several threads overlap. Start() arms it; every Mark() until the given number of vsyncs is
/// stored, then the whole timeline goes to the log at once.
namespace VideoCore::FrameTrace {

enum class Event : u8 {
    Vsync,           ///< a = vsyncs skipped before this one
    VsyncComposed,   ///< a = 1 when a new game frame was composed
    ComposeWaitEnd,  ///< a = microseconds the VSyncThread waited for the GPU thread
    Composite,       ///< the renderer composing; a = microseconds since the request
    QueueBuffer,     ///< the game queued a frame; a = slot, b = swap interval
    AcquireBuffer,   ///< Nvnflinger acquired it; a = slot, b = frame number
    ReleaseBuffer,   ///< Nvnflinger released it; a = slot, b = frame number
    DequeueWait,     ///< game waits; a = queue size, b = acquired/dequeued/max packed in bytes
    DequeueWaitEnd,  ///< a = microseconds it waited
    GpuFenceWait,    ///< the game waits for a syncpoint; a = syncpoint, b = value
    GpuFenceSignal,  ///< that wait was signalled; a = syncpoint, b = microseconds waited
    GpuSubmit,       ///< the GPU thread submitted a D3D12 command list
    GpuIdleEnd,      ///< the GPU thread got work after 200 us or more; a = microseconds idle
};

/// Records the next `vsyncs` vsyncs (ignored while a trace is running).
void Start(u32 vsyncs);

[[nodiscard]] bool Active();

void Mark(Event event, u64 a = 0, u64 b = 0);

} // namespace VideoCore::FrameTrace
