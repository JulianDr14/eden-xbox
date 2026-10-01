// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/scope_exit.h"
#include "common/settings.h"
#include "common/thread_cpu_time.h"
#include "core/core.h"
#include "core/arm/cpu_profile.h"
#include "dynarmic/interface/jit_profile.h"
#include "core/debugger/debugger.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/physical_core.h"
#include "core/hle/kernel/svc.h"
#include "video_core/perf_counters.h"
#include "video_core/frame_trace.h"

namespace Kernel {

PhysicalCore::PhysicalCore(KernelCore& kernel, std::size_t core_index)
    : m_core_index{core_index}
{
    m_is_single_core = !kernel.IsMulticore();
}
PhysicalCore::~PhysicalCore() = default;

void PhysicalCore::RunThread(KernelCore& kernel, Kernel::KThread* thread) {
    auto* process = thread->GetOwnerProcess();
    auto& system = kernel.System();
    auto* interface = process->GetArmInterface(m_core_index);

    interface->Initialize();

    const auto EnterContext = [&]() {
        // Lock the core context.
        std::scoped_lock lk{m_guard};

        // Check if we are already interrupted. If we are, we can just stop immediately.
        if (m_is_interrupted) {
            return false;
        }

        // Mark that we are running.
        m_arm_interface = interface;
        m_current_thread = thread;

        // Acquire the lock on the thread parameters.
        // This allows us to force synchronization with Interrupt.
        interface->LockThread(thread);

        return true;
    };

    const auto ExitContext = [&]() {
        // Save the JIT context back to the thread so the debugger can
        // read the current register state. Debug halt paths (step,
        // breakpoint, watchpoint) return from RunThread without going
        // through the scheduler's Unload/SaveContext.
        if (system.DebuggerEnabled()) {
            interface->GetContext(thread->GetContext());
        }

        // Unlock the thread.
        interface->UnlockThread(thread);

        // Lock the core context.
        std::scoped_lock lk{m_guard};

        // On exit, we no longer are running.
        m_arm_interface = nullptr;
        m_current_thread = nullptr;
    };

    while (true) {
        // If the thread is scheduled for termination, exit.
        if (thread->HasDpc() && thread->IsTerminationRequested()) {
            thread->Exit(kernel);
        }

        // Notify the debugger and go to sleep if a step was performed
        // and this thread has been scheduled again.
        if (thread->GetStepState() == StepState::StepPerformed) {
            system.GetDebugger().NotifyThreadStopped(thread);
            thread->RequestSuspend(kernel, SuspendType::Debug);
            return;
        }

        // Otherwise, run the thread.
        Core::HaltReason hr{};
        {
            // If we were interrupted, exit immediately.
            if (!EnterContext()) {
                return;
            }

            if (thread->GetStepState() == StepState::StepPending) {
                hr = interface->StepThread(thread);

                if (True(hr & Core::HaltReason::StepThread)) {
                    thread->SetStepState(StepState::StepPerformed);
                }
            } else {
                const u64 trace_context = (thread->GetThreadId() << 8) | static_cast<u64>(m_core_index);
                const auto compile_begin = Dynarmic::JitProfile::ReadLocalCompileNs();
                const auto flush_begin = Core::CpuProfile::ReadLocalFlushNs();
                const bool tracing = VideoCore::FrameTrace::Active();
                thread_local u32 cpu_sample_cursor{};
                const bool sample_cpu = tracing && (++cpu_sample_cursor & 15) == 0;
                // Windows CPU accounting may advance in ~15.6ms quanta. Aggregate 100ms
                // windows instead of pretending an individual short Run has precise CPU time.
                if (sample_cpu) {
                    struct CpuWindow {
                        u32 capture{};
                        std::chrono::steady_clock::time_point wall;
                        std::optional<u64> cpu;
                    };
                    thread_local CpuWindow window;
                    const auto now = std::chrono::steady_clock::now();
                    const auto capture = VideoCore::FrameTrace::GetCaptureStatus().id;
                    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                        now - window.wall).count();
                    if (capture != window.capture || elapsed >= 100000) {
                        const auto cpu = Common::CurrentThreadCpuTimeNs();
                        if (capture == window.capture)
                            VideoCore::FrameTrace::Mark(VideoCore::FrameTrace::Event::GuestHostCpuWindow,
                                cpu && window.cpu && *cpu >= *window.cpu ?
                                    (*cpu - *window.cpu) / 1000 : UINT64_MAX,
                                (static_cast<u64>(m_core_index) << 56) | static_cast<u64>(elapsed));
                        window = {capture, now, cpu};
                    }
                }
                const auto callbacks_begin = tracing ? Core::CpuProfile::ReadLocalCounters() :
                                                      Core::CpuProfile::Snapshot{};
                VideoCore::FrameTrace::ScopedSpan trace_run{
                    VideoCore::FrameTrace::Event::GuestRunLong, trace_context};
                hr = interface->RunThread(thread);
                if (trace_run.Finish() >= 200) {
                    const u64 compile_us =
                        (Dynarmic::JitProfile::ReadLocalCompileNs() - compile_begin) / 1000;
                    const u64 flush_us =
                        (Core::CpuProfile::ReadLocalFlushNs() - flush_begin) / 1000;
                    if (compile_us != 0) {
                        VideoCore::FrameTrace::Mark(VideoCore::FrameTrace::Event::GuestRunCompile,
                                                    compile_us, trace_context);
                    }
                    if (flush_us != 0) {
                        VideoCore::FrameTrace::Mark(VideoCore::FrameTrace::Event::GuestRunFlush,
                                                    flush_us, trace_context);
                    }
                    Kernel::Svc::ThreadContext context{};
                    interface->GetContext(context);
                    VideoCore::FrameTrace::Mark(VideoCore::FrameTrace::Event::GuestRunPc,
                                                context.pc, trace_context);
                    const u64 svc = True(hr & Core::HaltReason::SupervisorCall) ?
                                        interface->GetSvcNumber() : UINT32_MAX;
                    VideoCore::FrameTrace::Mark(VideoCore::FrameTrace::Event::GuestRunStop,
                        static_cast<u64>(hr) | (svc << 32), trace_context);
                    const auto callbacks_end = Core::CpuProfile::ReadLocalCounters();
                    const auto Delta = [&](Core::CpuProfile::Counter counter) {
                        return Core::CpuProfile::Get(callbacks_end, counter) -
                               Core::CpuProfile::Get(callbacks_begin, counter);
                    };
                    const u64 clock_us = Delta(Core::CpuProfile::Counter::ClockNs) / 1000;
                    const u64 icache_us = Delta(Core::CpuProfile::Counter::IcacheNs) / 1000;
                    if (clock_us != 0) {
                        VideoCore::FrameTrace::Mark(VideoCore::FrameTrace::Event::GuestRunClock,
                                                    clock_us, trace_context);
                    }
                    if (icache_us != 0) {
                        VideoCore::FrameTrace::Mark(VideoCore::FrameTrace::Event::GuestRunIcache,
                                                    icache_us, trace_context);
                    }
                    const u64 reads = std::min<u64>(Delta(Core::CpuProfile::Counter::Reads), UINT32_MAX);
                    const u64 writes = std::min<u64>(Delta(Core::CpuProfile::Counter::Writes), UINT32_MAX);
                    VideoCore::FrameTrace::Mark(VideoCore::FrameTrace::Event::GuestRunMemory,
                                                reads | (writes << 32), trace_context);
                }
            }

            ExitContext();
        }

        // Determine why we stopped.
        // If a step completed successfully, skip other halt reason handlers
        // the step takes priority (e.g. step may also set InstructionBreakpoint
        // if the next instruction happens to be a breakpoint).
        const bool step_completed = True(hr & Core::HaltReason::StepThread)
                                    && thread->GetStepState() == StepState::StepPerformed;
        const bool supervisor_call = !step_completed && True(hr & Core::HaltReason::SupervisorCall);
        const bool prefetch_abort = !step_completed && True(hr & Core::HaltReason::PrefetchAbort);
        const bool breakpoint = !step_completed && True(hr & Core::HaltReason::InstructionBreakpoint);
        const bool data_abort = !step_completed && True(hr & Core::HaltReason::DataAbort);
        const bool interrupt = !step_completed && True(hr & Core::HaltReason::BreakLoop);

        // Since scheduling may occur here, we cannot use any cached
        // state after returning from calls we make.

        // Notify the debugger and go to sleep if a breakpoint was hit,
        // or if the thread is unable to continue for any reason.
        if (breakpoint || prefetch_abort) {
            if (breakpoint) {
                interface->RewindBreakpointInstruction();
                // RewindBreakpointInstruction sets the JIT state to the
                // saved breakpoint context. Update the thread context to
                // match, since ExitContext already saved the post-execution
                // state.
                interface->GetContext(thread->GetContext());
            }
            if (system.DebuggerEnabled()) {
                system.GetDebugger().NotifyThreadStopped(thread);
            } else {
                interface->LogBacktrace(process);
            }
            thread->RequestSuspend(kernel, SuspendType::Debug);
            return;
        }

        // Notify the debugger and go to sleep on data abort.
        if (data_abort) {
            if (system.DebuggerEnabled()) {
                system.GetDebugger().NotifyThreadWatchpoint(thread, *interface->HaltedWatchpoint());
            }
            thread->RequestSuspend(kernel, SuspendType::Debug);
            return;
        }

        // Handle system calls.
        if (supervisor_call) {
            // Perform call.
            const auto svc_number = interface->GetSvcNumber();
            const bool trace_wait = VideoCore::FrameTrace::Active() &&
                (svc_number == static_cast<u32>(Svc::SvcId::WaitSynchronization) ||
                 svc_number == static_cast<u32>(Svc::SvcId::SendSyncRequest) ||
                 svc_number == static_cast<u32>(Svc::SvcId::SendSyncRequestWithUserBuffer));
            const bool trace_long_wait = VideoCore::FrameTrace::Active() &&
                (svc_number == static_cast<u32>(Svc::SvcId::SleepThread) ||
                 svc_number == static_cast<u32>(Svc::SvcId::ArbitrateLock) ||
                 svc_number == static_cast<u32>(Svc::SvcId::WaitProcessWideKeyAtomic) ||
                 svc_number == static_cast<u32>(Svc::SvcId::WaitForAddress));
            // Keep IPC edges, but compact frequent sleep/lock calls into completed long spans.
            // A blocking SVC may migrate its fiber: retain the original guest ID and capture.
            const u64 trace_thread = trace_wait || trace_long_wait ? thread->GetThreadId() : 0;
            VideoCore::FrameTrace::ScopedSpan wait_span{
                VideoCore::FrameTrace::Event::GuestSvcLong,
                (trace_thread << 8) | svc_number, trace_long_wait};
            if (trace_wait) {
                VideoCore::FrameTrace::Mark(VideoCore::FrameTrace::Event::GuestSvcBegin,
                                            trace_thread, svc_number);
            }
            Svc::Call(system, svc_number);
            wait_span.Finish();
            if (trace_wait) {
                VideoCore::FrameTrace::Mark(VideoCore::FrameTrace::Event::GuestSvcEnd,
                                            trace_thread, svc_number);
            }
            return;
        }

        // Handle external interrupt sources.
        if (interrupt || m_is_single_core) {
            return;
        }
    }
}

void PhysicalCore::LoadContext(const KThread* thread) {
    auto* const process = thread->GetOwnerProcess();
    if (!process) {
        // Kernel threads do not run on emulated CPU cores.
        return;
    }

    auto* interface = process->GetArmInterface(m_core_index);
    if (interface) {
        interface->SetContext(thread->GetContext());
        interface->SetTpidrroEl0(GetInteger(thread->GetTlsAddress()));
        interface->SetWatchpointArray(&process->GetWatchpoints());
    }
}

void PhysicalCore::LoadSvcArguments(const KProcess& process, std::span<const uint64_t, 8> args) {
    process.GetArmInterface(m_core_index)->SetSvcArguments(args);
}

void PhysicalCore::SaveContext(KThread* thread) const {
    auto* const process = thread->GetOwnerProcess();
    if (!process) {
        // Kernel threads do not run on emulated CPU cores.
        return;
    }

    auto* interface = process->GetArmInterface(m_core_index);
    if (interface) {
        interface->GetContext(thread->GetContext());
    }
}

void PhysicalCore::SaveSvcArguments(KProcess& process, std::span<uint64_t, 8> args) const {
    process.GetArmInterface(m_core_index)->GetSvcArguments(args);
}

void PhysicalCore::CloneFpuStatus(KThread* dst) const {
    auto* process = dst->GetOwnerProcess();

    Svc::ThreadContext ctx{};
    process->GetArmInterface(m_core_index)->GetContext(ctx);

    dst->GetContext().fpcr = ctx.fpcr;
    dst->GetContext().fpsr = ctx.fpsr;
}

void PhysicalCore::LogBacktrace(KernelCore& kernel) {
    auto* process = GetCurrentProcessPointer(kernel);
    if (!process) {
        return;
    }

    auto* interface = process->GetArmInterface(m_core_index);
    if (interface) {
        interface->LogBacktrace(process);
    }
}

void PhysicalCore::Idle() {
    // Frame chain counters: an emulated core with no guest thread to run.
    VideoCore::Perf::ScopedTimer timer{VideoCore::Perf::Counter::GuestCoreIdleUs};
    std::unique_lock lk{m_guard};
    m_on_interrupt.wait(lk, [this] { return m_is_interrupted; });
}

bool PhysicalCore::IsInterrupted() const {
    return m_is_interrupted;
}

void PhysicalCore::Interrupt() {
    // Lock core context.
    std::scoped_lock lk{m_guard};

    // Load members.
    auto* arm_interface = m_arm_interface;
    auto* thread = m_current_thread;

    // Add interrupt flag.
    m_is_interrupted = true;

    // Interrupt ourselves.
    m_on_interrupt.notify_one();

    // If there is no thread running, we are done.
    if (arm_interface == nullptr) {
        return;
    }

    // Interrupt the CPU.
    arm_interface->SignalInterrupt(thread);
}

void PhysicalCore::ClearInterrupt() {
    std::scoped_lock lk{m_guard};
    m_is_interrupted = false;
}

} // namespace Kernel
