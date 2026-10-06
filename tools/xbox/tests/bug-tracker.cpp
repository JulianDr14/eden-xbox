// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Build (MSVC, from the repository root, fmt header-only):
//   cl /nologo /std:c++20 /EHsc /O2 /W4 /DNDEBUG /DFMT_HEADER_ONLY /Isrc
//      /I.cache\cpm\fmt\12.1.0\include tools\xbox\tests\bug-tracker.cpp src\common\bug_tracker.cpp

// The checks must run in any configuration: an NDEBUG build would otherwise pass vacuously.
#undef NDEBUG
#include <cassert>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "common/assert.h"
#include "common/bug_tracker.h"
#include "common/logging.h"

// The tracker installs these into the logger and assert code; capture them instead.
namespace {
Common::Log::TapSeen captured_seen = nullptr;
Common::Log::TapRecord captured_record = nullptr;
AssertHook captured_assert = nullptr;
} // Anonymous namespace

void Common::Log::SetTap(TapSeen seen, TapRecord record) noexcept {
    captured_seen = seen;
    captured_record = record;
}
void SetAssertHook(AssertHook hook) noexcept {
    captured_assert = hook;
}

namespace {
using namespace Common::BugTracker;
using Common::Log::Class;

std::vector<std::pair<Category, std::string>> DrainAll() {
    std::vector<std::pair<Category, std::string>> out;
    Category category;
    std::uint64_t key;
    std::string message;
    while (Testing::Dequeue(category, key, message)) {
        out.emplace_back(category, message);
    }
    return out;
}

void HitPlain(int value) {
    BUG_TRACK(DrawSkipped, "draw skipped: value {}", value);
}

void HitKeyed(std::uint64_t key) {
    BUG_TRACK_KEY(UnsupportedFormat, key, "format {:#x} has no host equivalent", key);
}

int evaluated = 0;
int Expensive() {
    return ++evaluated;
}
void HitLazy() {
    BUG_TRACK(Other, "lazy {}", Expensive());
}

void TestSites() {
    Testing::ResetQueue();
    Detail::enabled.store(true);
    for (int i = 0; i < 1000; ++i) {
        HitPlain(i);
    }
    auto reports = DrainAll();
    assert(reports.size() == 1);
    assert(reports[0].first == Category::DrawSkipped);
    assert(reports[0].second == "draw skipped: value 0");

    // Arguments are evaluated only on the first hit.
    for (int i = 0; i < 100; ++i) {
        HitLazy();
    }
    assert(evaluated == 1);
    DrainAll();

    // Each new key once; 16 slots, then one overflow report; every hit counted on one thread.
    for (int round = 0; round < 3; ++round) {
        for (std::uint64_t key = 0; key < 20; ++key) {
            HitKeyed(key);
        }
    }
    reports = DrainAll();
    assert(reports.size() == Site::DETAIL_SLOTS + 1);
    assert(reports[0].second == "format 0x0 has no host equivalent");

    // Disabled: nothing is recorded.
    Detail::enabled.store(false);
    HitPlain(5);
    HitKeyed(999);
    assert(DrainAll().empty());
    Detail::enabled.store(true);
}

void TestConcurrentKeys() {
    Testing::ResetQueue();
    static constinit Site site{Category::ShaderCompile, __FILE__, __LINE__};
    std::atomic<int> reported{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&] {
            for (std::uint64_t key = 0; key < 10000; ++key) {
                if (site.Hit(key)) {
                    reported.fetch_add(1);
                }
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    // Reports are exact under contention: each slot is claimed once and the overflow flag once.
    assert(reported.load() == static_cast<int>(Site::DETAIL_SLOTS) + 1);
    // Counts may lose increments under true contention, never gain them.
    assert(site.hits.load() <= 80000 && site.hits.load() > 0);
}

void TestQueue() {
    Testing::ResetQueue();
    static constinit Site site{Category::Other, __FILE__, __LINE__};
    for (int i = 0; i < 300; ++i) {
        Testing::Enqueue(site, 0, false, "x");
    }
    assert(Testing::Dropped() == 300 - 256);
    assert(DrainAll().size() == 256);
    // A message longer than a record is truncated, not overflowed.
    const std::string long_message(2000, 'a');
    Testing::Enqueue(site, 0, false, long_message);
    const auto reports = DrainAll();
    assert(reports.size() == 1 && reports[0].second.size() == 480);
}

void TestHelpers() {
    using Testing::ClassifyLog;
    assert(ClassifyLog(Class::Debug, "C:/e/src/common/assert.cpp", "x") == Category::Ignored);
    assert(ClassifyLog(Class::Debug, "C:/e/src/core/a.cpp",
                       "C:/e/src/core/a.cpp: unimplemented op {}") == Category::Unimplemented);
    assert(ClassifyLog(Class::Debug, "C:/e/src/core/a.cpp", "a.cpp: assert x") ==
           Category::AssertFailed);
    assert(ClassifyLog(Class::Service_HID, "h.cpp", "(STUBBED) called") == Category::Stubbed);
    assert(ClassifyLog(Class::HW_GPU, "puller.cpp", "Special puller engine method X not implemented") ==
           Category::Unimplemented);
    assert(ClassifyLog(Class::HW_GPU, "maxwell_3d.cpp", "bad query") == Category::GpuHle);
    assert(ClassifyLog(Class::Service_NVDRV, "nvmap.cpp", "handle") == Category::GpuHle);
    assert(ClassifyLog(Class::Shader_SPIRV, "x.cpp", "Int64 atomics") == Category::ShaderCompile);
    assert(ClassifyLog(Class::Render, "r.cpp", "logic op is not supported") ==
           Category::UnsupportedState);
    assert(ClassifyLog(Class::Render, "r.cpp", "D3D12: device removed (reason 0x887A0006)") ==
           Category::DeviceRemoved);
    assert(ClassifyLog(Class::Render, "r.cpp", "frame took long") == Category::Renderer);
    assert(ClassifyLog(Class::Service_FS, "f.cpp", "file missing") == Category::Other);

    assert(Testing::ClassifyAssert("a.cpp:3: assert false && \"Unimplemented!\"") ==
           Category::Unimplemented);
    assert(Testing::ClassifyAssert("a.cpp:3: assert x != 0") == Category::AssertFailed);

    auto [file, line] =
        Testing::ParseAssertLocation("C:\\e\\src\\video_core\\engines\\dma.cpp:117: assert x");
    assert(file == "video_core/engines/dma.cpp" && line == 117);
    std::tie(file, line) = Testing::ParseAssertLocation("C:/e/src/core/k.cpp:42");
    assert(file == "core/k.cpp" && line == 42);
    std::tie(file, line) = Testing::ParseAssertLocation("garbage");
    assert(line == 0);
    assert(Testing::TrimSourcePath("C:\\x\\eden\\src\\common\\a.h") == "common/a.h");

    std::string json;
    Testing::AppendJsonString(json, "a\"b\\c\nd\x01");
    assert(json == "\"a\\\"b\\\\c\\nd\\u0001\"");
}

std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream file{path, std::ios::binary};
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}

void HitPipelineRejected(int frame) {
    BUG_TRACK(PipelineRejected, "PSO rejected at frame {}", frame);
}

void TestEndToEnd() {
    const auto dir = std::filesystem::temp_directory_path() / "eden-bug-tracker-test";
    std::filesystem::remove_all(dir);
    Detail::enabled.store(false);
    Install(dir, "test build");
    assert(Enabled() && captured_seen && captured_record && captured_assert);
    SetTitle(0x010019401051C000ULL, "Mario Strikers: Battle League");

    for (int frame = 0; frame < 5; ++frame) {
        TickFrame();
        HitPipelineRejected(frame);
    }
    // Logger tap: a new site wants its message; the same site never again.
    static const char log_file[] = "C:/e/src/video_core/engines/puller.cpp";
    assert(captured_seen(Class::HW_GPU, Common::Log::Level::Error, log_file, 114));
    captured_record({Class::HW_GPU, Common::Log::Level::Error, log_file, 114, "f", "HW.GPU",
                     "Error", "Special puller engine method 0x51 not implemented"});
    for (int i = 0; i < 99; ++i) {
        assert(!captured_seen(Class::HW_GPU, Common::Log::Level::Error, log_file, 114));
    }
    // A log next to an explicit BUG_TRACK is muted, so it is not recorded twice.
    static const char muted_file[] = "C:/e/src/video_core/renderer_d3d12/d3d12_rasterizer.cpp";
    {
        const TapMute mute;
        assert(!captured_seen(Class::Render, Common::Log::Level::Warning, muted_file, 311));
    }
    assert(captured_seen(Class::Render, Common::Log::Level::Warning, muted_file, 312));
    // Assert hook: the literal identifies the site.
    static const char what[] = "C:/e/src/video_core/engines/maxwell_dma.cpp:117: assert false && "
                               "\"Unimplemented!\"";
    for (int i = 0; i < 3; ++i) {
        captured_assert(what);
    }
    Shutdown();
    assert(!Enabled());

    const std::string log = ReadFile(dir / "eden_graphics_bugs.log");
    const std::string json = ReadFile(dir / "eden_graphics_bugs.json");
    assert(log.find("=== Game 010019401051C000 Mario Strikers: Battle League") != std::string::npos);
    assert(log.find("f=1] [PipelineRejected]") != std::string::npos);
    assert(log.find("PSO rejected at frame 0") != std::string::npos);
    assert(log.find("[Unimplemented] video_core/engines/puller.cpp:114 Error HW.GPU: Special "
                    "puller") != std::string::npos);
    assert(log.find("[Unimplemented] video_core/engines/maxwell_dma.cpp:117") != std::string::npos);
    assert(log.find("=== Summary") != std::string::npos);
    assert(json.find("\"title_id\": \"010019401051C000\"") != std::string::npos);
    assert(json.find("\"file\": \"video_core/engines/puller.cpp\", \"line\": 114, \"hits\": 100") !=
           std::string::npos);
    assert(json.find("\"file\": \"video_core/engines/maxwell_dma.cpp\", \"line\": 117, \"hits\": 3") !=
           std::string::npos);
    std::filesystem::remove_all(dir);
}

double NsPerCall(void (*function)(int), int iterations) {
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < iterations; ++i) {
        function(i);
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    return std::chrono::duration<double, std::nano>(elapsed).count() / iterations;
}

#if defined(_MSC_VER)
__declspec(noinline)
#else
__attribute__((noinline))
#endif
void Empty(int value) {
    static volatile int sink;
    sink = value;
}

#if defined(_MSC_VER)
__declspec(noinline)
#else
__attribute__((noinline))
#endif
void Tracked(int value) {
    static volatile int sink;
    sink = value;
    BUG_TRACK(DrawSkipped, "benchmark {}", value);
}

void Benchmark() {
    Testing::ResetQueue();
    constexpr int iterations = 100'000'000;
    Detail::enabled.store(true);
    Tracked(0);
    const double empty = NsPerCall(&Empty, iterations);
    const double enabled = NsPerCall(&Tracked, iterations);
    Detail::enabled.store(false);
    const double disabled = NsPerCall(&Tracked, iterations);
    std::printf("benchmark: empty call %.2f ns, repeated hit %.2f ns (+%.2f), disabled %.2f ns "
                "(+%.2f)\n",
                empty, enabled, enabled - empty, disabled, disabled - empty);
    // Loose bounds so a busy machine does not fail the test; the printed figures are the result.
    assert(enabled - empty < 5.0);
    assert(disabled - empty < 1.0);
}
} // Anonymous namespace

int main() {
    TestSites();
    TestConcurrentKeys();
    TestQueue();
    TestHelpers();
    TestEndToEnd();
    Benchmark();
    std::cout << "PASS: sites, lazy arguments, detail keys and overflow, concurrent reports, "
                 "lock-free queue, classification, location parsing, JSON escaping, end-to-end "
                 "files and benchmark\n";
}
