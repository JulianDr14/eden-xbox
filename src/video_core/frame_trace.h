// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <chrono>

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
    GuestSvcBegin,   ///< a = guest thread ID, b = blocking SVC ID (IPC or WaitSynchronization)
    GuestSvcEnd,     ///< a = original guest thread ID, b = SVC ID; includes blocked elapsed time
    GuestSvcLong, ///< a = elapsed us >=200, b = original guest ID << 8 | SVC; same capture only
    GuestThreadReady, ///< a = guest thread ID, b = priority; raw state became Runnable
    GuestDispatch, ///< a = guest thread ID, b = emulated core; actual scheduler switch
    GuestRunCompile, ///< a = nested Compile us in that Run on same host thread, b = thread/core
    GuestRunFlush, ///< a = nested CPU cached-area flush-check us in that Run, b = thread/core
    GuestRunLong, ///< a = elapsed microseconds, b = guest thread ID << 8 | emulated core
    GuestHostCpuWindow, ///< a = OS CPU us(UINT64_MAX unavailable), b = wall us low56/core high8
    GuestRunPc, ///< a = guest PC after Run, b = thread/core; endpoint, not a hot-PC sample
    GuestRunStop, ///< a = halt mask low32/SVC high32(UINT32_MAX if none), b = thread/core
    GuestRunClock, ///< a = nested clock callback us during T, b = thread/core
    GuestRunIcache, ///< a = nested icache callback us during T, b = thread/core
    GuestRunMemory, ///< a = slow read count low32/write count high32, b = thread/core
    TextureGcBudget, ///< a = actual app headroom bytes (UINT64_MAX unknown), b = pressure level0..3
    TextureGcPressure, ///< a = cache accounting bytes, b = critical threshold bytes
    TextureGcLong, ///< a = elapsed microseconds, b = frame tick
    TextureEvict, ///< a = guest GPU address, b = guest bytes (bit 63 = download required)
    TextureCreate, ///< a = guest GPU address, b = guest bytes; address reuse is only a heuristic
    TextureUploadLong, ///< a = host elapsed microseconds, b = guest GPU address; not GPU time
    TextureHeapUsage, ///< a = heap bytes, b = reserved bytes including fence-pending resources
    TextureHeapFree, ///< a = total reusable free bytes, b = largest free range across heap classes
    TextureHeapPending, ///< a = bytes pending fence retirement, b = pinned GC readback bytes
    TextureGcReadback, ///< a = 0 queued, 1 ready, 2 stale, 3 sync fallback, 4 budget deferred; b = address
    TextureUploadStaging, ///< a = elapsed us >=200, b = guest GPU address
    TextureUploadRead, ///< guest-memory read/map elapsed; same payload as staging
    TextureUploadUnswizzle, ///< CPU unswizzle elapsed, may contain memory access
    TextureUploadConvert, ///< CPU format conversion elapsed
    TextureUploadBackend, ///< host backend upload elapsed; contains repack/record, not GPU time
    TextureUploadRepack, ///< CPU row repack elapsed; nested in backend, do not add
    TextureUploadInfo, ///< a = guest bytes, b = guest GPU address; all refreshes during T
    TextureUploadFormat, ///< a = PixelFormat enum, b = guest GPU address
    GuestIpcCommand, ///< a = command low32/type next16/name length high16, b = original guest ID
    GuestIpcName0, ///< a = first 8 service-name bytes little-endian, b = guest ID
    GuestIpcName1, ///< a = next 8 service-name bytes, b = guest ID
    GuestIpcName2, ///< a = last 8 bytes; names longer than 24 explicitly truncated
    GuestIpcLock, ///< a = service mutex acquisition elapsed us >=200, b = guest ID
    GuestIpcHandler, ///< a = handler/response elapsed us >=200, b = guest ID (may defer response)
    VsyncSignal,     ///< immediately before signalling the display's guest VSync event
};

/// Records the next `vsyncs` vsyncs (ignored while a trace is running).
void Start(u32 vsyncs);

[[nodiscard]] bool Active();

enum class CaptureState : u8 { Idle, Recording, Saving, Saved, Truncated };
struct CaptureStatus {
    CaptureState state;
    u32 id;
    u32 vsyncs_remaining;
};
/// UI-only snapshot. Does not read the clock, wait for writers, or consume trace events.
[[nodiscard]] CaptureStatus GetCaptureStatus();

void Mark(Event event, u64 a = 0, u64 b = 0);

/// Emits only completed spans >=200 us in the same capture. Elapsed time includes
/// preemption, translation and callbacks; it is not host CPU utilization or GPU duration.
/// Outside T, the timer does not read the clock or allocate.
class ScopedSpan {
public:
    ScopedSpan(Event event_, u64 context_, bool enabled = true)
        : event{event_}, context{context_}, active{enabled && Active()} {
        if (active) {
            id = GetCaptureStatus().id;
            start = std::chrono::steady_clock::now();
        }
    }
    ~ScopedSpan() { Finish(); }
    /// Returns elapsed us (zero for a span outside this capture). Idempotent.
    u64 Finish() {
        if (!active || !Active() || GetCaptureStatus().id != id) {
            active = false;
            return 0;
        }
        active = false;
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start).count();
        if (us >= 200) {
            Mark(event, static_cast<u64>(us), context);
        }
        return static_cast<u64>(us);
    }
    ScopedSpan(const ScopedSpan&) = delete;
    ScopedSpan& operator=(const ScopedSpan&) = delete;
private:
    Event event;
    u64 context;
    bool active;
    u32 id{};
    std::chrono::steady_clock::time_point start;
};

} // namespace VideoCore::FrameTrace
