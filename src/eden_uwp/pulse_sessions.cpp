// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <ctime>
#include <mutex>
#include <stdexcept>
#include <vector>

#include <fmt/format.h>

#include "common/fs/path_util.h"
#include "common/pulse_store.h"
#include "common/scm_rev.h"
#include "core/core.h"
#include "core/pulse_checkpoint.h"
#include "eden_uwp/localization.h"
#include "eden_uwp/pulse_sessions.h"
#include "eden_uwp/system_settings.h"
#include "eden_uwp/uwp_system_settings.h"

#pragma comment(lib, "bcrypt.lib")

namespace EdenXbox::PulseSessions {
namespace {

// The most a restore decodes, as the test bench (the emulated DRAM and the process metadata).
constexpr std::uint64_t DecodedLimit = 5120ULL << 20;
// Long enough for the calls already in the kernel to return; a game that never gets there is
// left without a session rather than kept waiting at the menu.
constexpr auto CaptureBudget = std::chrono::seconds{3};
// The renderer never knowingly leaves less than this free (D3D12 HeadroomTracker::RESERVE); a save
// that would take the app below it is abandoned before the platform ends the app.
constexpr std::uint64_t MemoryFloor = 64ULL << 20;
// What a save adds on top: one image's readback at a time (16 MiB at 1080p), the writer's
// blocks and the heap it leaves behind (+30 MiB measured), with margin.
constexpr std::uint64_t SavePeak = 64ULL << 20;
// How often a save looks at the headroom again while it writes.
constexpr std::uint64_t MemoryCheckStep = 64ULL << 20;

std::array<std::uint8_t, 32> Sha256(std::span<const std::uint8_t> data) {
    std::array<std::uint8_t, 32> digest{};
    if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, const_cast<PUCHAR>(data.data()),
                   static_cast<ULONG>(data.size()), digest.data(),
                   static_cast<ULONG>(digest.size())) != 0) {
        throw std::runtime_error("Pulse: SHA-256 failed");
    }
    return digest;
}

std::array<std::uint8_t, 32> Sha256(std::string_view text) {
    return Sha256({reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
}

std::array<std::uint8_t, 32> Revision() {
    static const auto revision =
        Sha256(std::string{Common::g_scm_rev} + Common::g_build_fullname);
    return revision;
}

// One slot per game file: the library's homebrew have no title id, and two dumps of one game are
// two games to the player.
std::uint64_t Slot(std::string_view launch_path) {
    const auto digest = Sha256(launch_path);
    std::uint64_t slot;
    std::memcpy(&slot, digest.data(), sizeof(slot));
    return slot;
}

// Who hears the publication steps of the save in progress (Save sets it).
const std::function<void(const char*)>* save_phase{};

// One Store owns the folder for the whole process (pulse_store.h).
Common::Pulse::Store& Sessions() {
    static Common::Pulse::Store store{
        Common::FS::GetEdenPath(Common::FS::EdenPath::EdenDir) / "pulse" / "sessions",
        Common::Pulse::DefaultQuota, [](Common::Pulse::PublishStep step) {
            if (!save_phase || !*save_phase) return;
            using enum Common::Pulse::PublishStep;
            switch (step) {
            case Write: (*save_phase)("write"); break;
            case Verify: (*save_phase)("verify"); break;
            case Sync: (*save_phase)("sync"); break;
            case Replace: (*save_phase)("replace"); break;
            default: break;
            }
        }};
    return store;
}

std::mutex notice_mutex;
std::string notice;

} // namespace

bool Enabled() {
    return LoadSystemSettings().pulse_sessions;
}

Common::Pulse::Identity IdentityFor(std::string_view launch_path, std::span<const std::uint8_t, 16> user,
                                    std::uint64_t file_size, std::uint64_t application,
                                    std::uint32_t version, std::span<const std::uint8_t, 8> build_id) {
    Common::Pulse::Identity identity{};
    identity.program_id = Slot(launch_path);
    std::copy(user.begin(), user.end(), identity.user.begin());
    std::vector<std::uint8_t> content;
    const auto put = [&](const void* data, std::size_t size) {
        const auto* bytes = static_cast<const std::uint8_t*>(data);
        content.insert(content.end(), bytes, bytes + size);
    };
    put(&file_size, sizeof(file_size));
    put(&application, sizeof(application));
    put(&version, sizeof(version));
    put(build_id.data(), build_id.size());
    put(launch_path.data(), launch_path.size());
    identity.content = Sha256(content);
    identity.revision = Revision();
    return identity; // savedata stays zero: normal saves live apart and are never captured.
}

bool CanResume(const Common::Pulse::Identity& identity) {
    const auto key = Common::Pulse::StorageKey(identity);
    for (const auto& entry : Sessions().List()) {
        if (entry.key == key && entry.state == Common::Pulse::EntryState::Available &&
            entry.header_valid && entry.identity == identity) {
            return true;
        }
    }
    return false;
}

void Resume(Core::System& system, const Common::Pulse::Identity& identity,
            const std::function<void(const Kernel::ProcessLayoutPin&)>& load) {
    Core::Pulse::ProcessRestore restore{system, load};
    Sessions().RestoreAndConsume(Common::Pulse::StorageKey(identity), identity, DecodedLimit,
                                 [&](Common::Pulse::Section section, std::uint64_t offset,
                                     std::span<const std::uint8_t> data) {
                                     restore.Consume(section, offset, data);
                                 });
    restore.Finish();
}

std::string Save(Core::System& system, const Common::Pulse::Identity& identity,
                 const std::function<std::uint64_t()>& headroom,
                 const std::function<void(const char*)>& phase) {
    save_phase = &phase;
    struct Unset {
        ~Unset() { save_phase = nullptr; }
    } unset;
    try {
        const auto require = [&](std::uint64_t needed) {
            if (headroom && headroom() < needed) {
                throw std::runtime_error("Pulse: not enough free memory to keep the session");
            }
        };
        require(MemoryFloor + SavePeak);
        std::uint32_t retries{};
        const auto checkpoint =
            Core::Pulse::ProcessCheckpoint::CaptureAtBarrier(system, CaptureBudget, retries, phase);
        std::uint64_t next_check = MemoryCheckStep;
        Sessions().Publish(Common::Pulse::StorageKey(identity), identity, checkpoint.MaxEncodedSize(),
                           checkpoint.DecodedSize(),
                           [&](Common::Pulse::Writer& writer) { checkpoint.Write(writer); }, {},
                           [&](std::uint64_t written) {
                               if (written < next_check) return;
                               next_check = written + MemoryCheckStep;
                               require(MemoryFloor);
                           });
        return {};
    } catch (const std::exception& e) {
        void(system.Pause());
        std::string reason = e.what();
        if (reason.starts_with("Pulse: ")) reason.erase(0, 7);
        return reason.empty() ? std::string{"unknown"} : reason;
    }
}

std::optional<Kept> Find(std::string_view launch_path) {
    const auto slot = Slot(launch_path);
    std::optional<Kept> kept;
    try {
        for (const auto& entry : Sessions().List()) {
            if (entry.state != Common::Pulse::EntryState::Available || !entry.header_valid ||
                entry.identity.program_id != slot) {
                continue;
            }
            if (kept && kept->saved >= entry.modified) continue;
            kept = Kept{entry.modified, entry.identity.revision == Revision()};
        }
    } catch (...) {
        return std::nullopt; // An unreadable folder shows no session; nothing is deleted.
    }
    return kept;
}

bool Discard(std::string_view launch_path) {
    const auto slot = Slot(launch_path);
    try {
        for (const auto& entry : Sessions().List()) {
            if (entry.identity.program_id == slot) Sessions().Discard(entry.key);
        }
        return true;
    } catch (...) {
        return false;
    }
}

std::wstring FormatTime(std::filesystem::file_time_type time) {
    const auto system_time = std::chrono::clock_cast<std::chrono::system_clock>(time);
    const std::time_t seconds = std::chrono::system_clock::to_time_t(system_time);
    std::tm local{};
    if (localtime_s(&local, &seconds) != 0) return {};
    wchar_t text[32]{};
    std::wcsftime(text, std::size(text), L"%Y-%m-%d %H:%M", &local);
    return text;
}

void SetNotice(std::string text) {
    std::scoped_lock lock{notice_mutex};
    notice = std::move(text);
}

std::string TakeNotice() {
    std::scoped_lock lock{notice_mutex};
    return std::exchange(notice, {});
}

} // namespace EdenXbox::PulseSessions
