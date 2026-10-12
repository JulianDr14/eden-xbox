// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <thread>
#include <vector>

#include "common/fs/path_util.h"
#include "common/pulse_store.h"
#include "common/scm_rev.h"
#include "core/core.h"
#include "core/cpu_manager.h"
#include "core/file_sys/registered_cache.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/svc/svc_debug_string.h"
#include "core/hle/service/am/applet_manager.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/pulse_checkpoint.h"
#include "dynarmic/interface/code_memory.h"
#include "eden_uwp/headless_emu_window.h"
#include "eden_uwp/pulse_gate.h"
#include "eden_uwp/uwp_rom_storage.h"
#include "hid_core/hid_core.h"
#include "video_core/gpu.h"

#pragma comment(lib, "bcrypt.lib")

namespace EdenXbox {
namespace {

using Clock = std::chrono::steady_clock;
constexpr u64 JobLimit = 5120ULL << 20; // decoded bytes one restore may produce
constexpr auto CaptureBudget = std::chrono::seconds{10};
constexpr auto BeatTimeout = std::chrono::seconds{20};

double Ms(Clock::duration duration) {
    return std::chrono::duration<double, std::milli>(duration).count();
}

std::array<u8, 32> Sha256(std::span<const u8> data) {
    std::array<u8, 32> digest{};
    if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, const_cast<PUCHAR>(data.data()),
                   static_cast<ULONG>(data.size()), digest.data(),
                   static_cast<ULONG>(digest.size())) != 0) {
        throw std::runtime_error("Pulse gate: SHA-256 failed");
    }
    return digest;
}

// What the homebrew reports through svcOutputDebugString.
struct Heartbeats {
    std::mutex mutex;
    std::condition_variable changed;
    u64 beat{};
    u64 first_after_restore{};
    bool watch_restore{};
    bool failed{};
    std::string last;
    std::string failure;

    void Observe(std::string_view chunk) {
        std::scoped_lock lock{mutex};
        if (chunk.find("PULSE_FAIL") != std::string_view::npos) {
            failed = true;
            failure.assign(chunk);
        }
        constexpr std::string_view Prefix = "PULSE_HB ";
        if (const auto at = chunk.find(Prefix); at != std::string_view::npos) {
            const auto digits = chunk.substr(at + Prefix.size());
            u64 value{};
            if (std::from_chars(digits.data(), digits.data() + digits.size(), value).ec == std::errc{}) {
                beat = value;
                last.assign(chunk);
                if (watch_restore) {
                    first_after_restore = value;
                    watch_restore = false;
                }
            }
            if (chunk.find(" FAILED") != std::string_view::npos) failed = true;
        }
        changed.notify_all();
    }

    // Waits for a heartbeat at or past target; false on timeout or a reported failure.
    bool WaitFor(u64 target) {
        std::unique_lock lock{mutex};
        return changed.wait_for(lock, BeatTimeout, [&] { return failed || beat >= target; }) && !failed;
    }
};

// One System per session; never two alive at once (they share the window and the host memory).
struct Session {
    std::unique_ptr<FileSys::ManualContentProvider> provider =
        std::make_unique<FileSys::ManualContentProvider>();
    std::unique_ptr<Core::System> system = std::make_unique<Core::System>();
    bool loaded{};

    void Boot(HeadlessEmuWindow& window, const std::string& nro_path,
              const Kernel::ProcessLayoutPin* pin) {
        auto& s = *system;
        s.Initialize();
        s.ApplySettings();
        s.HIDCore().ReloadInputDevices();
        s.SetContentProvider(std::make_unique<FileSys::ContentProviderUnion>());
        s.RegisterContentProvider(FileSys::ContentProviderUnionSlot::FrontendManual, provider.get());
        s.SetFilesystem(MakeUwpFilesystem());
        s.GetFileSystemController().CreateFactories(*s.GetFilesystem());
        s.GetUserChannel().clear();
        if (pin) s.Kernel().PinNextProcessLayout(*pin);
        Service::AM::FrontendAppletParameters parameters{
            .applet_id = Service::AM::AppletId::Application,
        };
        loaded = true; // a failed Load still needs ShutdownMainProcess
        if (s.Load(window, nro_path, parameters) != Core::SystemResultStatus::Success) {
            throw std::runtime_error("Pulse gate: system.Load failed");
        }
        s.GPU().Start();
        s.GetCpuManager().OnGpuReady();
    }

    ~Session() {
        if (!loaded) return;
        void(system->Pause());
        system->ShutdownMainProcess();
        system->HIDCore().UnloadInputDevices();
    }
};

struct CycleReport {
    u32 retries{};
    double capture_ms{};
    double publish_ms{};
    double restore_ms{};
    u64 jit_after_destroy{}; // JIT code still committed once the old System is gone
    u64 archive_bytes{};
    Core::Pulse::CheckpointStats stats;
};

} // namespace

int RunPulseGate(const std::string& nro_path, const PulseGateOptions& options) {
    const auto& diag = options.diag;
    Heartbeats heartbeats;
    Kernel::Svc::SetDebugStringObserver([&](std::string_view chunk) { heartbeats.Observe(chunk); });
    struct DetachObserver {
        ~DetachObserver() { Kernel::Svc::SetDebugStringObserver(nullptr); }
    } detach_observer;

    HeadlessEmuWindow window{options.core_window, options.width, options.height};
    std::mt19937 rng{options.seed};
    std::optional<Session> session;
    std::vector<CycleReport> reports;
    try {
        std::ifstream file{std::filesystem::path{reinterpret_cast<const char8_t*>(nro_path.c_str())},
                           std::ios::binary};
        const std::vector<u8> nro{std::istreambuf_iterator<char>{file}, {}};
        if (nro.empty()) throw std::runtime_error("Pulse gate: cannot read the homebrew");
        const std::string revision = std::string{Common::g_scm_rev} + Common::g_build_fullname;

        Common::Pulse::Store store{Common::FS::GetEdenPath(Common::FS::EdenPath::EdenDir) / "pulse" /
                                   "gate"};
        session.emplace();
        session->Boot(window, nro_path, nullptr);
        Common::Pulse::Identity identity{};
        identity.program_id = session->system->GetApplicationProcessProgramID();
        identity.content = Sha256(nro);
        identity.revision = Sha256({reinterpret_cast<const u8*>(revision.data()), revision.size()});
        const auto key = Common::Pulse::StorageKey(identity);
        void(session->system->Run());
        if (!heartbeats.WaitFor(4)) throw std::runtime_error("Pulse gate: no heartbeat after boot");
        diag("PULSE gate: homebrew running, " + std::to_string(options.cycles) + " cycles, seed " +
             std::to_string(options.seed));

        for (u32 cycle = 1; cycle <= options.cycles; ++cycle) {
            // Land the capture anywhere: mid-computation, mid-IPC (retried) or deep in a long wait.
            std::this_thread::sleep_for(std::chrono::milliseconds{50 + rng() % 1500});
            CycleReport report;
            auto& system = *session->system;
            std::optional<Core::Pulse::ProcessCheckpoint> checkpoint;
            const auto capture_start = Clock::now();
            checkpoint.emplace(Core::Pulse::ProcessCheckpoint::CaptureAtBarrier(
                system, CaptureBudget, report.retries));
            report.capture_ms = Ms(Clock::now() - capture_start);
            u64 captured_beat;
            {
                std::scoped_lock lock{heartbeats.mutex};
                captured_beat = heartbeats.beat;
            }

            const auto publish_start = Clock::now();
            store.Publish(key, identity, checkpoint->MaxEncodedSize(), checkpoint->DecodedSize(),
                          [&](Common::Pulse::Writer& writer) { checkpoint->Write(writer); });
            report.publish_ms = Ms(Clock::now() - publish_start);
            report.stats = checkpoint->Stats();
            for (const auto& entry : store.List()) {
                if (entry.key == key) report.archive_bytes = entry.bytes;
            }
            checkpoint.reset();

            // Destroy the process and the whole System, then rebuild from the archive alone.
            session.reset();
            report.jit_after_destroy = Dynarmic::CommittedCodeBytes();
            const auto restore_start = Clock::now();
            session.emplace();
            Core::Pulse::ProcessRestore restore{
                *session->system, [&](const Kernel::ProcessLayoutPin& pin) {
                    session->Boot(window, nro_path, std::addressof(pin));
                }};
            store.RestoreAndConsume(key, identity, JobLimit,
                                    [&](Common::Pulse::Section section, u64 offset,
                                        std::span<const u8> data) {
                                        restore.Consume(section, offset, data);
                                    });
            restore.Finish();
            report.restore_ms = Ms(Clock::now() - restore_start);
            {
                std::scoped_lock lock{heartbeats.mutex};
                heartbeats.watch_restore = true;
                heartbeats.first_after_restore = 0;
            }
            void(session->system->Run());
            if (!heartbeats.WaitFor(captured_beat + 3)) {
                throw std::runtime_error("Pulse gate: no progress after restore (beat " +
                                         std::to_string(captured_beat) + ")");
            }
            u64 first;
            std::string last;
            {
                std::scoped_lock lock{heartbeats.mutex};
                first = heartbeats.first_after_restore;
                last = heartbeats.last;
            }
            if (first != captured_beat + 1) {
                throw std::runtime_error("Pulse gate: restored at beat " + std::to_string(first) +
                                         ", captured after beat " + std::to_string(captured_beat));
            }
            const auto& s = report.stats;
            diag("PULSE cycle " + std::to_string(cycle) + ": beat " + std::to_string(captured_beat) +
                 ", retries " + std::to_string(report.retries) +
                 ", capture " +
                 std::to_string(report.capture_ms) + " ms, publish " +
                 std::to_string(report.publish_ms) + " ms, restore " +
                 std::to_string(report.restore_ms) + " ms, archive " +
                 std::to_string(report.archive_bytes) + " B, memory " +
                 std::to_string(s.memory_bytes) + " B, metadata " + std::to_string(s.metadata_bytes) +
                 " B, threads " + std::to_string(s.threads) + " (" +
                 std::to_string(s.waiting_threads) + " waiting), objects " +
                 std::to_string(s.objects) + ", handles " + std::to_string(s.handles) +
                 ", timing events " + std::to_string(s.timing_events) + ", JIT after destroy " +
                 std::to_string(report.jit_after_destroy >> 20) + " MiB | " + last);
            reports.push_back(report);
        }
    } catch (const std::exception& error) {
        diag(std::string{"PULSE gate FAIL after "} + std::to_string(reports.size()) + " cycles: " +
             error.what());
        session.reset();
        return 17;
    }

    const auto max_of = [&](auto member) {
        double result{};
        for (const auto& report : reports) result = std::max(result, static_cast<double>(report.*member));
        return result;
    };
    u32 in_wait{};
    for (const auto& report : reports) in_wait += report.stats.waiting_threads > 0;
    diag("PULSE gate PASS: " + std::to_string(reports.size()) + " cycles, " + std::to_string(in_wait) +
         " captured with threads in long waits; max capture " +
         std::to_string(max_of(&CycleReport::capture_ms)) + " ms, max publish " +
         std::to_string(max_of(&CycleReport::publish_ms)) + " ms, max restore " +
         std::to_string(max_of(&CycleReport::restore_ms)) + " ms, max archive " +
         std::to_string(static_cast<u64>(max_of(&CycleReport::archive_bytes))) + " B, max retries " +
         std::to_string(static_cast<u32>(max_of(&CycleReport::retries))));
    session.reset();
    return 0;
}

} // namespace EdenXbox
