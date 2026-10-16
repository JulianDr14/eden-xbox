// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <mutex>
#include <limits>
#include <string>
#include <tuple>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include "common/cpu_features.h"
#include "common/cpu_wait.h"
#include "common/logging.h"

#ifdef _WIN32
#include "common/windows/precise_timer.h"
#include "common/windows/timer_resolution.h"
#include "common/windows/timed_wait_profile.h"
#endif

#include "common/settings.h"
#include "core/core_timing.h"
#include "core/hardware_properties.h"

namespace Core::Timing {

constexpr s64 MAX_SLICE_LENGTH = 10000;

constexpr u32 CPU_CLOCK_BASE_MHZ = 1020;
constexpr u32 CPU_CLOCK_BOOST_MHZ = 1734;
constexpr u32 CPU_CLOCK_OVERCLOCK_MHZ = 2040;

constexpr u32 CpuClockTargetMhz(Settings::CpuClock clock) {
    switch (clock) {
    case Settings::CpuClock::Boost:
        return CPU_CLOCK_BOOST_MHZ;
    case Settings::CpuClock::Overclock:
        return CPU_CLOCK_OVERCLOCK_MHZ;
    default:
        return CPU_CLOCK_BASE_MHZ;
    }
}

namespace {
// Every event type ever created, by name; expired entries are dropped as the registry grows.
std::mutex live_events_lock;
std::unordered_multimap<std::string, std::weak_ptr<EventType>> live_events;
size_t live_events_sweep_at = 256;
} // namespace

std::shared_ptr<EventType> CreateEvent(std::string name, TimedCallback&& callback) {
    auto type = std::make_shared<EventType>(std::move(callback), std::move(name));
    std::scoped_lock lock{live_events_lock};
    if (live_events.size() >= live_events_sweep_at) {
        std::erase_if(live_events, [](const auto& entry) { return entry.second.expired(); });
        live_events_sweep_at = std::max<size_t>(256, live_events.size() * 2);
    }
    live_events.emplace(type->name, type);
    return type;
}

std::shared_ptr<EventType> FindLiveEvent(const std::string& name) {
    std::scoped_lock lock{live_events_lock};
    std::shared_ptr<EventType> found;
    const auto [begin, end] = live_events.equal_range(name);
    for (auto it = begin; it != end; ++it) {
        auto type = it->second.lock();
        if (!type) continue;
        if (found) return {};
        found = std::move(type);
    }
    return found;
}

struct CoreTiming::Event {
    s64 time;
    u64 fifo_order;
    std::weak_ptr<EventType> type;
    s64 reschedule_time;
    heap_t::handle_type handle{};

    // Sort by time, unless the times are the same, in which case sort by
    // the order added to the queue
    friend bool operator>(const Event& left, const Event& right) {
        return std::tie(left.time, left.fifo_order) > std::tie(right.time, right.fifo_order);
    }

    friend bool operator<(const Event& left, const Event& right) {
        return std::tie(left.time, left.fifo_order) < std::tie(right.time, right.fifo_order);
    }
};

CoreTiming::CoreTiming() = default;
CoreTiming::~CoreTiming() {
    Reset();
}

void CoreTiming::Initialize(std::function<void()>&& on_thread_init_) {
    Reset();
    checkpoint_ns_origin = 0;
    checkpoint_clock_origin = 0;
    checkpoint_gpu_origin = 0;
    checkpoint_resume_pending = false;
    on_thread_init = std::move(on_thread_init_);
    event_fifo_id = 0;
    cpu_ticks = 0;
    if (is_multicore) {
        const auto& clock = Common::g_wall_clock;
#ifdef ARCHITECTURE_x86_64
#ifdef _WIN32
        const char* const sleeps =
            Common::Windows::PreciseTimer::IsSupported()
                ? (Common::IsPreciseTimerSleeps() ? "high-resolution timer"
                                                  : "OS timer ticks (high-resolution timer off)")
                : "OS timer ticks";
#else
        const char* const sleeps = "condition variable";
#endif
        LOG_INFO(Core_Timing, "Host clock: {}, CPU waits: {}, sleeps: {}",
                 clock.IsNative() ? fmt::format("TSC at {:.1f} MHz", clock.rdtsc_frequency / 1e6)
                                  : std::string{"steady_clock"},
                 Common::CpuWaitMethodName(Common::BestCpuWaitMethod()), sleeps);
#endif
        timer_thread = std::jthread([this](std::stop_token stop_token) {
            Common::SetCurrentThreadName("HostTiming");
#ifdef _WIN32
            Common::Windows::TimedWaitProfile::ActivateThread();
#endif
            Common::SetCurrentThreadPriority(Common::ThreadPriority::VeryHigh);
            Common::SetCurrentThreadToPerformanceCores();
            on_thread_init();
            has_started = true;

            // base frequency in MHz: 1ns (10^-9) = 1GHz (10^9)
            while (!stop_token.stop_requested()) {
                while (!paused && !stop_token.stop_requested()) {
                    paused_set = false;
                    if (auto const next_time = Advance(); next_time) {
                        // There are more events left in the queue, wait until the next event.
                        auto const wait_time = *next_time - GetGlobalTimeNs().count();
                        if (wait_time > 0) {
                            event.WaitFor(std::chrono::nanoseconds(wait_time));
                        }
                    } else {
                        // Queue is empty, wait until another event is scheduled and signals us to
                        // continue.
                        wait_set = true;
                        event.Wait();
                    }
                    wait_set = false;
                }
                paused_set = true;
                pause_event.Wait();
            }
        });
    }
}

void CoreTiming::ClearPendingEvents() {
    std::scoped_lock lock{advance_lock, basic_lock};
    event_queue.clear();
    event.Set();
}

void CoreTiming::Pause(bool is_paused) {
    if (!is_paused && checkpoint_resume_pending) {
        // Reconstruction can take seconds. Exclude that host time before waking any callback.
        checkpoint_ns_origin += Common::g_wall_clock.GetTimeNS().count() - checkpoint_restore_ns;
        checkpoint_clock_origin += Common::g_wall_clock.GetCNTPCT() - checkpoint_restore_clock;
        checkpoint_gpu_origin += Common::g_wall_clock.GetGPUTick() - checkpoint_restore_gpu;
        checkpoint_resume_pending = false;
    }
    paused = is_paused;
    pause_event.Set();

    if (!is_paused) {
        pause_end_time = GetGlobalTimeNs().count();
    }
}

void CoreTiming::SyncPause(bool is_paused) {
    if (is_paused == paused && paused_set == paused) {
        return;
    }

    Pause(is_paused);
    if (timer_thread.joinable()) {
        if (!is_paused) {
            pause_event.Set();
        }
        event.Set();
        while (paused_set != is_paused)
            ;
    }

    if (!is_paused) {
        pause_end_time = GetGlobalTimeNs().count();
    }
}

bool CoreTiming::IsRunning() const {
    return !paused_set;
}

bool CoreTiming::HasPendingEvents() const {
    std::scoped_lock lock{basic_lock};
    return !(wait_set && event_queue.empty());
}

void CoreTiming::ScheduleEvent(std::chrono::nanoseconds ns_into_future,
                               const std::shared_ptr<EventType>& event_type, bool absolute_time) {
    {
        std::scoped_lock scope{basic_lock};
        const auto next_time{absolute_time ? ns_into_future : GetGlobalTimeNs() + ns_into_future};

        auto h{event_queue.emplace(Event{next_time.count(), event_fifo_id++, event_type, 0})};
        (*h).handle = h;
    }

    event.Set();
}

void CoreTiming::ScheduleLoopingEvent(std::chrono::nanoseconds start_time,
                                      std::chrono::nanoseconds resched_time,
                                      const std::shared_ptr<EventType>& event_type,
                                      bool absolute_time) {
    {
        std::scoped_lock scope{basic_lock};
        const auto next_time{absolute_time ? start_time : GetGlobalTimeNs() + start_time};

        auto h{event_queue.emplace(
            Event{next_time.count(), event_fifo_id++, event_type, resched_time.count()})};
        (*h).handle = h;
    }

    event.Set();
}

void CoreTiming::UnscheduleEvent(const std::shared_ptr<EventType>& event_type,
                                 UnscheduleEventType type) {
    {
        std::scoped_lock lk{basic_lock};

        std::vector<heap_t::handle_type> to_remove;
        for (auto itr = event_queue.begin(); itr != event_queue.end(); itr++) {
            const Event& e = *itr;
            if (e.type.lock().get() == event_type.get()) {
                to_remove.push_back(itr->handle);
            }
        }

        for (auto& h : to_remove) {
            event_queue.erase(h);
        }

        event_type->sequence_number++;
    }

    // Force any in-progress events to finish
    if (type == UnscheduleEventType::Wait) {
        std::scoped_lock lk{advance_lock};
    }
}

static u64 GetNextTickCount(u64 next_ticks) {
    if (Settings::values.use_custom_cpu_ticks.GetValue()) {
        return Settings::values.cpu_ticks.GetValue();
    }
    return next_ticks;
}

void CoreTiming::AddTicks(u64 ticks_to_add) {
    const u64 ticks = GetNextTickCount(ticks_to_add);
    cpu_ticks += ticks;
    downcount -= static_cast<s64>(ticks);
}

void CoreTiming::Idle() {
    AddTicks(1000U);
}

void CoreTiming::ResetTicks() {
    downcount = MAX_SLICE_LENGTH;
}

u64 CoreTiming::GetClockTicks() const {
    u64 fres = is_multicore ? Common::g_wall_clock.GetCNTPCT() - checkpoint_clock_origin : Common::WallClock::CPUTickToCNTPCT(cpu_ticks);
    if (const u32 target = CpuClockTargetMhz(Settings::values.cpu_clock.GetValue());
        target != CPU_CLOCK_BASE_MHZ) {
        fres = fres * target / CPU_CLOCK_BASE_MHZ;
    }
    if (::Settings::values.sync_core_speed.GetValue()) {
        auto const ticks = f64(fres);
        auto const speed_limit = f64(Settings::SpeedLimit()) * 0.01;
        return u64(ticks / speed_limit);
    }
    return fres;
}

u64 CoreTiming::GetGPUTicks() const {
    return is_multicore
        ? Common::g_wall_clock.GetGPUTick() - checkpoint_gpu_origin
        : Common::WallClock::CPUTickToGPUTick(cpu_ticks);
}

std::optional<s64> CoreTiming::Advance() {
    std::scoped_lock lock{advance_lock, basic_lock};
    global_timer = GetGlobalTimeNs().count();

    while (!event_queue.empty() && event_queue.top().time <= global_timer) {
        const Event& evt = event_queue.top();

        if (const auto event_type{evt.type.lock()}) {
            const auto evt_time = evt.time;
            const auto evt_sequence_num = event_type->sequence_number;

            if (evt.reschedule_time == 0) {
                event_queue.pop();

                basic_lock.unlock();

                event_type->callback(
                    evt_time, std::chrono::nanoseconds{GetGlobalTimeNs().count() - evt_time});

                basic_lock.lock();
            } else {
                basic_lock.unlock();

                const auto new_schedule_time{event_type->callback(
                    evt_time, std::chrono::nanoseconds{GetGlobalTimeNs().count() - evt_time})};

                basic_lock.lock();

                if (evt_sequence_num != event_type->sequence_number) {
                    // Heap handle is invalidated after external modification.
                    continue;
                }

                const auto next_schedule_time{new_schedule_time.has_value()
                                                  ? new_schedule_time.value().count()
                                                  : evt.reschedule_time};

                // If this event was scheduled into a pause, its time now is going to be way
                // behind. Re-set this event to continue from the end of the pause.
                auto next_time{evt.time + next_schedule_time};
                if (evt.time < pause_end_time) {
                    next_time = pause_end_time + next_schedule_time;
                }

                event_queue.update(evt.handle, Event{next_time, event_fifo_id++, evt.type,
                                                     next_schedule_time, evt.handle});
            }
        }

        global_timer = GetGlobalTimeNs().count();
    }

    if (!event_queue.empty()) {
        return event_queue.top().time;
    } else {
        return std::nullopt;
    }
}

void CoreTiming::Reset() {
    paused = true;
    pause_event.Set();
    event.Set();
    if (timer_thread.joinable()) {
        timer_thread.request_stop();
        timer_thread.join();
    }
    has_started = false;
}

/// @brief Returns current time in nanoseconds.
std::chrono::nanoseconds CoreTiming::GetGlobalTimeNs() const noexcept {
    return is_multicore
        ? Common::g_wall_clock.GetTimeNS() - std::chrono::nanoseconds{checkpoint_ns_origin}
        : std::chrono::nanoseconds{Common::WallClock::CPUTickToNS(cpu_ticks)};
}

/// @brief Returns current time in microseconds.
std::chrono::microseconds CoreTiming::GetGlobalTimeUs() const noexcept {
    return is_multicore
        ? std::chrono::duration_cast<std::chrono::microseconds>(GetGlobalTimeNs())
        : std::chrono::microseconds{Common::WallClock::CPUTickToUS(cpu_ticks)};
}

#ifdef _WIN32
void CoreTiming::SetTimerResolutionNs(std::chrono::nanoseconds ns) {
    timer_resolution_ns = ns.count();
}
#endif

namespace {
constexpr size_t MaxCheckpointEvents = 65536;

void ValidateBindings(std::span<const CheckpointEventBinding> bindings) {
    if (bindings.size() > MaxCheckpointEvents) {
        throw std::invalid_argument("Pulse: too many timing bindings");
    }
    std::unordered_set<u64> ids;
    std::unordered_set<const EventType*> types;
    for (const auto& binding : bindings) {
        if (binding.id == 0 || !binding.type || !ids.insert(binding.id).second ||
            !types.insert(binding.type.get()).second) {
            throw std::invalid_argument("Pulse: invalid or duplicate timing binding");
        }
    }
}
} // namespace

TimingCheckpoint CoreTiming::CaptureCheckpoint(
    std::span<const CheckpointEventBinding> bindings) {
    std::scoped_lock lock{advance_lock, basic_lock};
    if (!paused || (timer_thread.joinable() && !paused_set)) {
        throw std::logic_error("Pulse: timing must be synchronously paused");
    }
    ValidateBindings(bindings);
    if (event_queue.size() > MaxCheckpointEvents) {
        throw std::invalid_argument("Pulse: too many timing events");
    }
    std::unordered_map<const EventType*, u64> ids;
    for (const auto& binding : bindings) {
        ids.emplace(binding.type.get(), binding.id);
    }
    TimingCheckpoint state{};
    state.multicore = is_multicore;
    state.time_ns = GetGlobalTimeNs().count();
    state.clock_ticks = Common::g_wall_clock.GetCNTPCT() - checkpoint_clock_origin;
    state.gpu_ticks = Common::g_wall_clock.GetGPUTick() - checkpoint_gpu_origin;
    state.cpu_ticks = cpu_ticks;
    state.downcount = downcount;
    state.pause_end_time = pause_end_time;
    state.next_fifo_id = event_fifo_id;
    state.events.reserve(event_queue.size());
    for (const auto& pending : event_queue) {
        const auto type = pending.type.lock();
        if (!type || !ids.contains(type.get())) {
            throw std::invalid_argument("Pulse: timing event has no reconstructible owner");
        }
        state.events.push_back({ids.at(type.get()), pending.time, pending.fifo_order,
                                pending.reschedule_time});
    }
    return state;
}

std::vector<std::shared_ptr<EventType>> CoreTiming::CheckpointQueuedEventTypes() {
    std::scoped_lock lock{advance_lock, basic_lock};
    if (!paused || (timer_thread.joinable() && !paused_set)) {
        throw std::logic_error("Pulse: timing must be synchronously paused");
    }
    if (event_queue.size() > MaxCheckpointEvents) {
        throw std::invalid_argument("Pulse: too many timing events");
    }
    std::vector<std::shared_ptr<EventType>> types;
    std::unordered_set<const EventType*> seen;
    for (const auto& pending : event_queue) {
        auto type = pending.type.lock();
        if (!type) {
            throw std::invalid_argument("Pulse: timing event without a live owner");
        }
        if (seen.insert(type.get()).second) {
            types.push_back(std::move(type));
        }
    }
    return types;
}

void CoreTiming::RestoreCheckpoint(const TimingCheckpoint& state,
                                  std::span<const CheckpointEventBinding> bindings) {
    std::scoped_lock lock{advance_lock, basic_lock};
    if (!paused || (timer_thread.joinable() && !paused_set)) {
        throw std::logic_error("Pulse: timing must be synchronously paused");
    }
    ValidateBindings(bindings);
    if (state.multicore != is_multicore || state.time_ns < 0 ||
        state.pause_end_time < 0 || state.pause_end_time > state.time_ns ||
        state.next_fifo_id == std::numeric_limits<u64>::max() ||
        state.events.size() > MaxCheckpointEvents) {
        throw std::invalid_argument("Pulse: invalid timing checkpoint");
    }
    std::unordered_map<u64, std::shared_ptr<EventType>> types;
    for (const auto& binding : bindings) {
        types.emplace(binding.id, binding.type);
    }
    std::unordered_set<u64> orders;
    heap_t rebuilt;
    for (const auto& pending : state.events) {
        if (!types.contains(pending.binding_id) || pending.period_ns < 0 ||
            pending.time_ns < 0 ||
            pending.period_ns > std::numeric_limits<s64>::max() -
                                    std::max(pending.time_ns, state.time_ns) ||
            pending.fifo_order >= state.next_fifo_id ||
            !orders.insert(pending.fifo_order).second) {
            throw std::invalid_argument("Pulse: invalid timing event reference or order");
        }
        auto handle = rebuilt.emplace(Event{pending.time_ns, pending.fifo_order,
                                            types.at(pending.binding_id), pending.period_ns});
        (*handle).handle = handle;
    }
    // Publication follows all allocations and validation. Heap handles remain attached to their
    // nodes across swap; no captured host handle or callback is reused.
    std::swap(event_queue, rebuilt);
    checkpoint_restore_ns = Common::g_wall_clock.GetTimeNS().count();
    checkpoint_restore_clock = Common::g_wall_clock.GetCNTPCT();
    checkpoint_restore_gpu = Common::g_wall_clock.GetGPUTick();
    checkpoint_ns_origin = checkpoint_restore_ns - state.time_ns;
    checkpoint_clock_origin = checkpoint_restore_clock - state.clock_ticks;
    checkpoint_gpu_origin = checkpoint_restore_gpu - state.gpu_ticks;
    checkpoint_resume_pending = true;
    cpu_ticks = state.cpu_ticks;
    downcount = state.downcount;
    global_timer = state.time_ns;
    pause_end_time = state.pause_end_time;
    event_fifo_id = state.next_fifo_id;
}

} // namespace Core::Timing
