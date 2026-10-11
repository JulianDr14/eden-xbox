// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later

// CommandRecorder gate (the D3D12 command list recording thread): commands and their arrays
// arrive once and in order across chunks, inline mode runs them at once, record points report
// what the worker executed, a throwing command stops the worker and is rethrown once, captured
// objects are destroyed exactly once, shutdown drains and DiscardRecorded drops the rest. Prints the recording cost per command.
// Run with build-uwp/diagnostics/command-recorder/run.bat.

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>
#include "common/windows/timer_resolution.h"
#include "video_core/renderer_d3d12/d3d12_command_recorder.h"

// common.lib asserts through these; the harness has no logging backend.
void AssertFatalImpl() { std::abort(); }
void AssertFailSoftImpl() { std::abort(); }
void AssertFailedAt(const char*) { std::abort(); }
void UnreachableAt(const char*) { std::abort(); }

namespace {

using namespace std::chrono_literals;

void Check(bool condition, const char* what) {
    if (!condition) {
        std::cerr << "FAIL: " << what << '\n';
        std::exit(1);
    }
}

struct Log {
    std::vector<u64> values;
    std::thread::id thread;
};

using Recorder = D3D12::CommandRecorder<Log*>;

Recorder::Options Threaded() {
    return {.threaded = true, .thread_name = "RecorderTest"};
}

void TestOrder() {
    Log log;
    Recorder recorder{&log, Threaded()};
    constexpr u64 COUNT = 300'000;
    u64 expected_values = 0;
    for (u64 i = 0; i < COUNT; ++i) {
        if (i % 7 == 0) {
            // Arrays of every size up to a few KiB, so records straddle chunk ends.
            std::vector<u64> data(1 + (i * 31) % 700);
            for (size_t k = 0; k < data.size(); ++k) {
                data[k] = i * 1000 + k;
            }
            recorder.Record(std::span<const u64>{data}, [i, n = data.size()](Log* l, const u64* d) {
                l->values.push_back(i);
                for (size_t k = 0; k < n; ++k) {
                    Check(d[k] == i * 1000 + k, "array copied intact");
                }
            });
        } else {
            recorder.Record([i](Log* l) { l->values.push_back(i); });
        }
        ++expected_values;
        if (i % 50'000 == 0) {
            recorder.Dispatch();
        }
    }
    recorder.WaitIdle();
    Check(log.values.size() == expected_values, "every command ran once");
    for (u64 i = 0; i < COUNT; ++i) {
        Check(log.values[i] == i, "commands run in recording order");
    }
    std::cout << "  order: " << COUNT << " commands PASS\n";
}

void TestInline() {
    Log log;
    Recorder recorder{&log, {}};
    Check(!recorder.IsThreaded(), "inline without a worker");
    recorder.Record([](Log* l) { l->thread = std::this_thread::get_id(); });
    Check(log.thread == std::this_thread::get_id(), "inline runs on the recording thread");
    const u64 values[] = {5, 6};
    recorder.Record(std::span<const u64>{values}, [&values](Log* l, const u64* d) {
        Check(d == values, "inline passes the caller's array, uncopied");
        l->values.push_back(d[1]);
    });
    Check(log.values.size() == 1 && log.values[0] == 6, "inline ran before Record returned");
    Check(recorder.IsExecuted(recorder.RecordPoint()), "inline: everything executed");
    std::cout << "  inline PASS\n";
}

void TestRecordPoint() {
    Log log;
    Recorder recorder{&log, Threaded()};
    std::atomic<bool> release{false};
    recorder.Record([&release](Log* l) {
        l->thread = std::this_thread::get_id();
        while (!release.load()) {
            std::this_thread::yield();
        }
    });
    const u64 point = recorder.RecordPoint();
    Check(!recorder.IsExecuted(point), "not executed while recorded only");
    recorder.Dispatch();
    std::this_thread::sleep_for(5ms);
    Check(!recorder.IsExecuted(point), "not executed while the worker runs it");
    release = true;
    recorder.WaitIdle();
    Check(recorder.IsExecuted(point), "executed after WaitIdle");
    Check(log.thread != std::this_thread::get_id(), "ran on the worker");
    Check(recorder.RecordPoint() > point, "later records get a later point");
    std::cout << "  record points PASS\n";
}

struct Tracked {
    explicit Tracked(std::atomic<int>& live_) : live{&live_} {
        ++*live;
    }
    Tracked(const Tracked& other) : live{other.live} {
        ++*live;
    }
    ~Tracked() {
        --*live;
    }
    std::atomic<int>* live;
};

void TestError() {
    Log log;
    std::atomic<int> errors{0};
    std::atomic<int> live{0};
    {
        Recorder recorder{&log, Threaded(), [&errors] { ++errors; }};
        recorder.Record([](Log* l) { l->values.push_back(1); });
        recorder.Record([](Log*) { throw std::runtime_error("command failed"); });
        for (int i = 0; i < 5000; ++i) {
            // Captures with a destructor, discarded unrun after the failure.
            recorder.Record([t = Tracked{live}](Log* l) { l->values.push_back(2); });
        }
        bool thrown = false;
        try {
            recorder.WaitIdle();
        } catch (const std::runtime_error& e) {
            thrown = std::string_view{e.what()} == "command failed";
        }
        Check(thrown, "the worker's exception reaches the recording thread");
        Check(errors == 1, "on_error ran once");
        Check(log.values.size() == 1, "commands after the failure did not run");
        Check(live == 0, "discarded captures were destroyed");
        recorder.WaitIdle(); // no second throw
    }
    std::cout << "  error PASS\n";
}

void TestShutdown() {
    Log log;
    std::atomic<int> live{0};
    {
        Recorder recorder{&log, Threaded()};
        for (int i = 0; i < 20'000; ++i) {
            recorder.Record([t = Tracked{live}](Log* l) { l->values.push_back(3); });
        }
        // Not dispatched: the destructor dispatches and drains.
    }
    Check(log.values.size() == 20'000, "shutdown executes what was recorded");
    Check(live == 0, "executed captures were destroyed");

    Log dropped;
    {
        Recorder recorder{&dropped, Threaded()};
        recorder.Record([](Log* l) { l->values.push_back(4); });
        recorder.Dispatch();
        recorder.Record([t = Tracked{live}](Log* l) { l->values.push_back(5); });
        recorder.DiscardRecorded();
    }
    Check(dropped.values.size() == 1 && dropped.values[0] == 4, "dispatched ran, discarded not");
    Check(live == 0, "discarded captures were destroyed");
    std::cout << "  shutdown PASS\n";
}

void TestTooLarge() {
    Log log;
    Recorder recorder{&log, Threaded()};
    std::vector<std::byte> huge(D3D12::CommandChunk<Log*>::CAPACITY);
    bool thrown = false;
    try {
        recorder.Record(std::span<const std::byte>{huge}, [](Log*, const std::byte*) {});
    } catch (const std::length_error&) {
        thrown = true;
    }
    Check(thrown, "a record larger than a chunk is refused");
    std::cout << "  too large PASS\n";
}

void Benchmark() {
    // Small commands as a draw records them: the recording thread's cost per command.
    Log log;
    log.values.reserve(4'000'000);
    Recorder recorder{&log, Threaded()};
    constexpr u64 COUNT = 4'000'000;
    const auto start = std::chrono::steady_clock::now();
    for (u64 i = 0; i < COUNT; ++i) {
        recorder.Record([i, a = i * 3, b = i * 5](Log* l) { l->values.push_back(i + a - b); });
    }
    recorder.Dispatch();
    const auto recorded = std::chrono::steady_clock::now();
    recorder.WaitIdle();
    const auto done = std::chrono::steady_clock::now();
    std::cout << "  record "
              << std::chrono::duration<double, std::nano>(recorded - start).count() / COUNT
              << " ns/command, worker done "
              << std::chrono::duration<double, std::milli>(done - recorded).count()
              << " ms after the last record\n";
}

} // Anonymous namespace

int main() {
    Common::Windows::SetCurrentTimerResolutionToMaximum();
    TestOrder();
    TestInline();
    TestRecordPoint();
    TestError();
    TestShutdown();
    TestTooLarge();
    Benchmark();
    std::cout << "command recorder: order, inline, record points, error, shutdown PASS\n";
    return 0;
}
