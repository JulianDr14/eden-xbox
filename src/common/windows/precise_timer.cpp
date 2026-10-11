// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <windows.h>

#include "common/windows/precise_timer.h"

// Older Windows 10 SDKs lack it.
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace Common::Windows {

namespace {

HANDLE CreateHighResolutionTimer() {
    return CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                  TIMER_ALL_ACCESS);
}

} // Anonymous namespace

PreciseTimer::PreciseTimer() {
    timer = CreateHighResolutionTimer();
    if (timer == nullptr) {
        return;
    }
    // Auto-reset: a wake ends one sleep.
    wake_event = CreateEventExW(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
    if (wake_event == nullptr) {
        CloseHandle(timer);
        timer = nullptr;
    }
}

PreciseTimer::~PreciseTimer() {
    if (timer != nullptr) {
        CloseHandle(timer);
    }
    if (wake_event != nullptr) {
        CloseHandle(wake_event);
    }
}

bool PreciseTimer::IsSupported() {
    static const bool supported = [] {
        const HANDLE probe = CreateHighResolutionTimer();
        if (probe == nullptr) {
            return false;
        }
        CloseHandle(probe);
        return true;
    }();
    return supported;
}

bool PreciseTimer::Sleep(std::chrono::nanoseconds duration) {
    // Relative due times are negative, in 100 ns units; setting the timer also unsignals it.
    LARGE_INTEGER due{};
    due.QuadPart = -std::max<LONGLONG>(1, duration.count() / 100);
    if (!SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0)) {
        return false;
    }
    const HANDLE handles[] = {timer, wake_event};
    return WaitForMultipleObjects(2, handles, FALSE, INFINITE) == WAIT_OBJECT_0 + 1;
}

void PreciseTimer::Wake() {
    SetEvent(wake_event);
}

} // namespace Common::Windows
