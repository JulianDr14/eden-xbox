// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

#include "common/alignment.h"
#include "common/common_types.h"
#include "common/polyfill_thread.h"
#include "common/spsc_wake_queue.h"
#include "common/thread.h"

// Records commands on one thread and executes them, in order, on a worker thread: the model of the
// Vulkan backend's scheduler (Record, CommandChunk and its worker). No device here, so ordering,
// draining, errors and shutdown are covered by tools/xbox/tests/command-recorder.cpp.
namespace D3D12 {

/// A fixed-size block of commands, each a callable stored in place and followed by the array it
/// reads (barriers, viewports...), so recording allocates nothing. A command is run once and
/// destroyed, or only destroyed when the chunk is discarded.
template <typename Context>
class CommandChunk {
public:
    static constexpr size_t CAPACITY = 64 * 1024;

    CommandChunk() = default;
    ~CommandChunk() {
        Discard();
    }

    CommandChunk(const CommandChunk&) = delete;
    CommandChunk& operator=(const CommandChunk&) = delete;

    /// Stores func, called later as func(context). False when it does not fit.
    template <typename Func>
    [[nodiscard]] bool Record(Func&& func) {
        return Emplace<void>(std::forward<Func>(func), {});
    }

    /// Stores a copy of data and func, called later as func(context, copied data pointer).
    template <typename T, typename Func>
    [[nodiscard]] bool Record(std::span<const T> data, Func&& func) {
        static_assert(std::is_trivially_copyable_v<T>, "Recorded arrays are copied bytewise");
        return Emplace<T>(std::forward<Func>(func), std::as_bytes(data));
    }

    /// Runs every command in order and leaves the chunk empty. When one throws, the rest are
    /// destroyed unrun and the exception propagates.
    void Execute(Context& context) {
        size_t offset = 0;
        try {
            while (offset < used) {
                Header* const header = HeaderAt(offset);
                offset += header->size;
                header->run(header, &context);
            }
        } catch (...) {
            DestroyFrom(offset);
            used = 0;
            throw;
        }
        used = 0;
    }

    /// Destroys every command without running it.
    void Discard() noexcept {
        DestroyFrom(0);
        used = 0;
    }

    [[nodiscard]] size_t Size() const noexcept { return used; }

    [[nodiscard]] bool Empty() const noexcept {
        return used == 0;
    }

private:
    struct Header {
        /// Runs the command when context is not null, then destroys it.
        void (*run)(Header* header, Context* context);
        u32 size;        ///< header, callable and data, up to the next header
        u32 data_offset; ///< from the header; 0 without data
    };

    template <typename Func, typename T>
    static void Run(Header* header, Context* context) {
        std::byte* const base = reinterpret_cast<std::byte*>(header);
        Func* const func = std::launder(reinterpret_cast<Func*>(
            base + Common::AlignUp(sizeof(Header), alignof(Func))));
        struct Destroy {
            Func* func;
            ~Destroy() {
                func->~Func();
            }
        } destroy{func};
        if (context == nullptr) {
            return;
        }
        if constexpr (std::is_void_v<T>) {
            (*func)(*context);
        } else {
            (*func)(*context, reinterpret_cast<const T*>(base + header->data_offset));
        }
    }

    template <typename T, typename Callable>
    bool Emplace(Callable&& func, std::span<const std::byte> data) {
        using Func = std::decay_t<Callable>;
        static_assert(alignof(Func) <= alignof(std::max_align_t), "Over-aligned command");
        constexpr size_t data_align = [] {
            if constexpr (std::is_void_v<T>) {
                return size_t{1};
            } else {
                return alignof(T);
            }
        }();
        const size_t start = used;
        const size_t func_offset = Common::AlignUp(sizeof(Header), alignof(Func));
        const size_t data_offset = data.empty()
                                       ? 0
                                       : Common::AlignUp(func_offset + sizeof(Func), data_align);
        const size_t end = data.empty() ? func_offset + sizeof(Func) : data_offset + data.size();
        const size_t size = Common::AlignUp(end, alignof(std::max_align_t));
        if (size > CAPACITY - start) {
            return false;
        }
        std::byte* const base = storage.data() + start;
        if (!data.empty()) {
            std::memcpy(base + data_offset, data.data(), data.size());
        }
        new (base + func_offset) Func(std::forward<Callable>(func));
        new (base) Header{.run = &Run<Func, T>,
                          .size = static_cast<u32>(size),
                          .data_offset = static_cast<u32>(data_offset)};
        used = start + size;
        return true;
    }

    Header* HeaderAt(size_t offset) noexcept {
        return std::launder(reinterpret_cast<Header*>(storage.data() + offset));
    }

    void DestroyFrom(size_t offset) noexcept {
        while (offset < used) {
            Header* const header = HeaderAt(offset);
            offset += header->size;
            header->run(header, nullptr);
        }
    }

    alignas(std::max_align_t) std::array<std::byte, CAPACITY> storage;
    size_t used = 0;
};

/// Hands recorded commands to a worker thread, which executes them on context in the order they
/// were recorded. Without a worker (threaded = false) every command runs as it is recorded, on the
/// recording thread: the fallback, with no copy and no handoff.
///
/// One recording thread. A command that throws on the worker stops it: the commands after it are
/// discarded, on_error runs on the worker (to release whoever waits on their results) and the
/// exception is rethrown on the recording thread by the next RethrowError or WaitIdle, never by
/// Record, so errors surface where the caller expects them.
template <typename Context>
class CommandRecorder {
public:
    using Chunk = CommandChunk<Context>;

    /// Chunks dispatched and not yet executed before the recording thread waits: 8 MiB.
    static constexpr size_t QUEUE_CHUNKS = 128;
    /// Executed chunks kept for reuse.
    static constexpr size_t SPARE_CHUNKS = 32;

    struct Options {
        bool threaded = false;
        bool profile = false; ///< Per-chunk elapsed measurements; no clocks when disabled.
        const char* thread_name = "CommandRecorder";
        Common::ThreadPriority priority = Common::ThreadPriority::High;
        /// The worker spins this long before blocking; adaptive, so long gaps cost no CPU.
        Common::SpinPolicy spin{Common::SpinPolicy::Mode::Adaptive, std::chrono::microseconds{50}};
    };

    CommandRecorder(Context context_, Options options, std::function<void()> on_error_ = {})
        : context{std::move(context_)}, on_error{std::move(on_error_)}, threaded{options.threaded},
          work{options.spin}, profile{options.profile} {
        if (!threaded) {
            return;
        }
        chunk = std::make_unique<Chunk>();
        worker = std::jthread{[this, options](std::stop_token stop) {
            Common::SetCurrentThreadName(options.thread_name);
            Common::SetCurrentThreadPriority(options.priority);
            WorkerLoop(stop);
        }};
    }

    ~CommandRecorder() {
        Shutdown();
    }

    CommandRecorder(const CommandRecorder&) = delete;
    CommandRecorder& operator=(const CommandRecorder&) = delete;

    [[nodiscard]] bool IsThreaded() const noexcept {
        return threaded;
    }

    struct Statistics {
        u64 chunks{}, bytes{}, handoff_ns{}, execute_ns{}, drain_ns{}, max_pending{};
    };
    [[nodiscard]] Statistics ReadStatistics() const noexcept {
        return {stat_chunks.load(), stat_bytes.load(), stat_handoff_ns.load(),
                stat_execute_ns.load(), stat_drain_ns.load(), stat_max_pending.load()};
    }

    template <typename Func>
    void Record(Func&& func) {
        if (!threaded) {
            func(context);
            return;
        }
        if (!chunk->Record(func)) {
            Dispatch();
            if (!chunk->Record(std::forward<Func>(func))) {
                throw std::length_error("D3D12: recorded command larger than a chunk");
            }
        }
    }

    /// func(context, data pointer); the worker reads a copy of data.
    template <typename T, typename Func>
    void Record(std::span<const T> data, Func&& func) {
        if (!threaded) {
            func(context, data.data());
            return;
        }
        if (!chunk->Record(data, func)) {
            Dispatch();
            if (!chunk->Record(data, std::forward<Func>(func))) {
                throw std::length_error("D3D12: recorded array larger than a chunk");
            }
        }
    }

    /// Hands what was recorded so far to the worker. Never throws: once the worker has failed,
    /// what was recorded is discarded, and the error waits for RethrowError or WaitIdle.
    void Dispatch() {
        if (!threaded || chunk->Empty()) {
            return;
        }
        if (failed.load(std::memory_order_acquire)) {
            chunk->Discard();
            return;
        }
        const size_t bytes = profile ? chunk->Size() : 0;
        const auto start = profile ? std::chrono::steady_clock::now()
                                   : std::chrono::steady_clock::time_point{};
        work.EmplaceWait(std::move(chunk));
        if (profile) {
            stat_chunks.fetch_add(1, std::memory_order_relaxed);
            stat_bytes.fetch_add(bytes, std::memory_order_relaxed);
            stat_handoff_ns.fetch_add(ElapsedNs(start), std::memory_order_relaxed);
            // The consumer may finish before dispatched is published; never subtract unsigned
            // executed from an older dispatched value without checking it.
            const u64 issued = dispatched.load(std::memory_order_relaxed) + 1;
            const u64 done = executed.load(std::memory_order_acquire);
            const u64 pending = issued > done ? issued - done : 0;
            u64 previous = stat_max_pending.load(std::memory_order_relaxed);
            while (previous < pending && !stat_max_pending.compare_exchange_weak(
                       previous, pending, std::memory_order_relaxed)) {}
        }
        dispatched.store(dispatched.load(std::memory_order_relaxed) + 1,
                         std::memory_order_release);
        chunk = TakeSpareChunk();
    }

    /// Dispatches and waits until the worker has executed everything.
    void WaitIdle() {
        if (!threaded) {
            return;
        }
        Dispatch();
        const auto start = profile ? std::chrono::steady_clock::now()
                                   : std::chrono::steady_clock::time_point{};
        const u64 target = dispatched.load(std::memory_order_relaxed);
        for (u64 done = executed.load(std::memory_order_acquire); done < target;
             done = executed.load(std::memory_order_acquire)) {
            executed.wait(done, std::memory_order_acquire);
        }
        if (profile) stat_drain_ns.fetch_add(ElapsedNs(start), std::memory_order_relaxed);
        RethrowError();
    }

    /// Everything recorded up to now has been executed once IsExecuted(RecordPoint()) holds.
    /// Without a worker every command is executed as it is recorded.
    [[nodiscard]] u64 RecordPoint() const noexcept {
        return threaded ? dispatched.load(std::memory_order_acquire) + 1 : 0;
    }
    [[nodiscard]] bool IsExecuted(u64 point) const noexcept {
        return point <= executed.load(std::memory_order_acquire);
    }

    /// Rethrows, once, the exception a command threw on the worker.
    void RethrowError() {
        if (!failed.load(std::memory_order_acquire)) {
            return;
        }
        std::exception_ptr pending;
        {
            std::scoped_lock lock{error_mutex};
            pending = std::exchange(error, nullptr);
        }
        if (pending) {
            std::rethrow_exception(pending);
        }
    }

    /// Drops what was recorded and not dispatched yet, unrun.
    void DiscardRecorded() noexcept {
        if (threaded) {
            chunk->Discard();
        }
    }

    /// Executes what is left and stops the worker. Errors are dropped: this runs on teardown.
    void Shutdown() noexcept {
        if (!worker.joinable()) {
            return;
        }
        Dispatch();
        worker.request_stop();
        worker.join();
    }

private:
    static u64 ElapsedNs(std::chrono::steady_clock::time_point start) {
        return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start).count());
    }
    void WorkerLoop(std::stop_token stop) {
        std::unique_ptr<Chunk> next;
        // PopWait only reports a stop once the queue is empty, so everything dispatched runs.
        while (work.PopWait(next, stop)) {
            const auto start = profile ? std::chrono::steady_clock::now()
                                       : std::chrono::steady_clock::time_point{};
            if (!failed.load(std::memory_order_relaxed)) {
                try {
                    next->Execute(context);
                } catch (...) {
                    {
                        std::scoped_lock lock{error_mutex};
                        error = std::current_exception();
                    }
                    failed.store(true, std::memory_order_release);
                    if (on_error) {
                        on_error();
                    }
                }
            } else {
                next->Discard();
            }
            if (profile) stat_execute_ns.fetch_add(ElapsedNs(start), std::memory_order_relaxed);
            ReturnSpareChunk(std::move(next));
            executed.fetch_add(1, std::memory_order_release);
            executed.notify_all();
        }
    }

    std::unique_ptr<Chunk> TakeSpareChunk() {
        {
            std::scoped_lock lock{spare_mutex};
            if (!spare.empty()) {
                std::unique_ptr<Chunk> reused = std::move(spare.back());
                spare.pop_back();
                return reused;
            }
        }
        return std::make_unique<Chunk>();
    }

    void ReturnSpareChunk(std::unique_ptr<Chunk> used_chunk) {
        std::scoped_lock lock{spare_mutex};
        if (spare.size() < SPARE_CHUNKS) {
            spare.push_back(std::move(used_chunk));
        }
    }

    Context context;
    std::function<void()> on_error;
    const bool threaded;

    std::unique_ptr<Chunk> chunk; ///< being recorded (recording thread)
    Common::SPSCWakeQueue<std::unique_ptr<Chunk>, QUEUE_CHUNKS> work;
    alignas(64) std::atomic<u64> dispatched{0};
    alignas(64) std::atomic<u64> executed{0};

    std::mutex spare_mutex;
    std::vector<std::unique_ptr<Chunk>> spare;

    std::atomic<bool> failed{false};
    std::mutex error_mutex;
    std::exception_ptr error;

    const bool profile;
    std::atomic<u64> stat_chunks{}, stat_bytes{}, stat_handoff_ns{}, stat_execute_ns{};
    std::atomic<u64> stat_drain_ns{}, stat_max_pending{};

    std::jthread worker; ///< last: stopped before the members it uses are destroyed
};

} // namespace D3D12
