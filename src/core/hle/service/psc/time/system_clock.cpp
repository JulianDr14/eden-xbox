// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/scope_exit.h"
#include "core/core.h"
#include "core/hle/service/cmif_serialization.h"
#include "core/hle/service/psc/time/system_clock.h"

namespace Service::PSC::Time {

SystemClock::SystemClock(Core::System& system_, SystemClockCore& clock_core, bool can_write_clock, bool can_write_uninitialized_clock)
    : ServiceFramework{system_, "ISystemClock"}
    , m_clock_core{clock_core}
    , m_can_write_clock{can_write_clock}
    , m_can_write_uninitialized_clock{can_write_uninitialized_clock}
{
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, D<&SystemClock::GetCurrentTime>, "GetCurrentTime"},
        {1, D<&SystemClock::SetCurrentTime>, "SetCurrentTime"},
        {2, D<&SystemClock::GetSystemClockContext>, "GetSystemClockContext"},
        {3, D<&SystemClock::SetSystemClockContext>, "SetSystemClockContext"},
        {4, D<&SystemClock::GetOperationEventReadableHandle>, "GetOperationEventReadableHandle"},
    };
    // clang-format on
    RegisterHandlers(functions);
}

Result SystemClock::GetCurrentTime(Out<s64> out_time) {
    SCOPE_EXIT {
        LOG_DEBUG(Service_Time, "called. out_time={}", *out_time);
    };

    R_UNLESS(m_can_write_uninitialized_clock || m_clock_core.IsInitialized(),
             ResultClockUninitialized);

    R_RETURN(m_clock_core.GetCurrentTime(out_time.Get()));
}

Result SystemClock::SetCurrentTime(s64 time) {
    LOG_DEBUG(Service_Time, "called. time={}", time);

    R_UNLESS(m_can_write_clock, ResultPermissionDenied);
    R_UNLESS(m_can_write_uninitialized_clock || m_clock_core.IsInitialized(),
             ResultClockUninitialized);

    R_RETURN(m_clock_core.SetCurrentTime(time));
}

Result SystemClock::GetSystemClockContext(Out<SystemClockContext> out_context) {
    SCOPE_EXIT {
        LOG_DEBUG(Service_Time, "called. out_context={}", *out_context);
    };

    R_UNLESS(m_can_write_uninitialized_clock || m_clock_core.IsInitialized(),
             ResultClockUninitialized);

    R_RETURN(m_clock_core.GetContext(*out_context));
}

Result SystemClock::SetSystemClockContext(const SystemClockContext& context) {
    LOG_DEBUG(Service_Time, "called. context={}", context);

    R_UNLESS(m_can_write_clock, ResultPermissionDenied);
    R_UNLESS(m_can_write_uninitialized_clock || m_clock_core.IsInitialized(),
             ResultClockUninitialized);

    R_RETURN(m_clock_core.SetContextAndWrite(context));
}

Result SystemClock::GetOperationEventReadableHandle(
    OutCopyHandle<Kernel::KReadableEvent> out_event) {
    LOG_DEBUG(Service_Time, "called.");

    R_TRY(OpenOperationEvent());
    *out_event = &m_operation_event->m_event->GetReadableEvent();
    R_SUCCEED();
}

Result SystemClock::OpenOperationEvent() {
    if (!m_operation_event) {
        m_operation_event = std::make_unique<OperationEvent>(system);
        R_UNLESS(m_operation_event != nullptr, ResultFailed);

        m_clock_core.LinkOperationEvent(*m_operation_event);
    }
    R_SUCCEED();
}

void SystemClock::PulseCapture(Pulse::HandlerCapture& capture) const {
    if (!pulse_clock) throw Pulse::ServiceRejected(false, "system clock of an unknown time service");
    capture.state.U32(pulse_clock);
    capture.state.Bool(m_operation_event != nullptr);
    if (m_operation_event) capture.ExportEvent(0, m_operation_event->m_event);
}

Kernel::KEvent* SystemClock::PulseEvent(u32 slot) {
    return slot == 0 && m_operation_event ? m_operation_event->m_event : nullptr;
}

} // namespace Service::PSC::Time
