// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#include "eden_uwp/mods/mod_downloads.h"

#include <algorithm>
#include <chrono>
#include <thread>

#include "common/logging.h"
#include "eden_uwp/mods/mod_web.h"

namespace EdenXbox::Mods {
namespace {
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

/// Left free after a download, like after an install.
constexpr std::uint64_t SpaceMargin = 256ull << 20;
/// No mod archive is anywhere near this; a server that says otherwise is not sending one.
constexpr std::uint64_t MaxArchive = 8ull << 30;
/// Tries after the first when the connection drops, waiting 2, 4 and 8 s before each.
constexpr int Retries = 3;

ModError FromDownload(Web::DownloadError error) {
    switch (error) {
    case Web::DownloadError::None: return ModError::None;
    case Web::DownloadError::Network: return ModError::Network;
    case Web::DownloadError::Checksum: return ModError::Checksum;
    case Web::DownloadError::TooLarge: return ModError::TooLarge;
    case Web::DownloadError::Write: return ModError::Write;
    case Web::DownloadError::Cancelled: return ModError::Cancelled;
    }
    return ModError::Network;
}

/// The archive's extension, from the name the source gives it (".zip"); libarchive reads the
/// contents, the name only helps the log.
std::string Extension(const std::string& name) {
    const auto dot = name.rfind('.');
    if (dot == std::string::npos || name.size() - dot > 6) return ".archive";
    std::string extension = name.substr(dot);
    const bool plain = std::all_of(extension.begin() + 1, extension.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    });
    return plain ? extension : ".archive";
}

/// Sleeps `seconds`, waking early when stopped; false when stopped.
bool Wait(int seconds, std::stop_token stop) {
    const auto until = Clock::now() + std::chrono::seconds{seconds};
    while (Clock::now() < until) {
        if (stop.stop_requested()) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }
    return !stop.stop_requested();
}

} // namespace

ModDownloads::ModDownloads(fs::path folder_) : folder{std::move(folder_)} {}

ModDownloads::~ModDownloads() {
    CancelAll();
}

void ModDownloads::Enqueue(DownloadRequest request) {
    {
        const std::scoped_lock lock{mutex};
        if (std::any_of(status.begin(), status.end(), [&](const DownloadStatus& s) { return s.key == request.key; }))
            return;
        status.push_back({request.key, request.title, false, 0, request.size, 0});
    }
    queue.Post([this, request = std::move(request)](std::stop_token stop) { Run(request, stop); });
}

void ModDownloads::CancelAll() {
    queue.Clear();
    const std::scoped_lock lock{mutex};
    cancel.request_stop();
    cancel = std::stop_source{};
    std::erase_if(status, [](const DownloadStatus& s) { return !s.running; });
}

std::vector<DownloadStatus> ModDownloads::Status() const {
    const std::scoped_lock lock{mutex};
    return status;
}

bool ModDownloads::Active() const {
    const std::scoped_lock lock{mutex};
    return !status.empty();
}

std::optional<FinishedDownload> ModDownloads::TakeFinished() {
    const std::scoped_lock lock{mutex};
    if (finished.empty()) return std::nullopt;
    auto result = std::move(finished.front());
    finished.pop_front();
    return result;
}

void ModDownloads::Run(DownloadRequest request, std::stop_token stop) {
    std::stop_source local;
    std::stop_token cancelled;
    {
        const std::scoped_lock lock{mutex};
        cancelled = cancel.get_token();
        for (auto& s : status)
            if (s.key == request.key) s.running = true;
    }
    const std::stop_callback on_stop{stop, [&] { local.request_stop(); }};
    const std::stop_callback on_cancel{cancelled, [&] { local.request_stop(); }};

    const auto file = folder / (request.key + Extension(request.source.archive));
    auto part = file;
    part += ".part";
    ModError error = ModError::None;
    std::error_code ec;
    fs::create_directories(folder, ec);
    const auto space = fs::space(folder, ec);
    const std::uint64_t free = ec ? 0 : space.available;
    const std::uint64_t have = fs::exists(part, ec) ? fs::file_size(part, ec) : 0;
    if (fs::exists(file, ec)) {
        // Downloaded and checked before, never installed (the app closed): use it as it is.
    } else if (free < SpaceMargin || request.size > free - SpaceMargin + have) {
        error = ModError::Space;
    } else {
        // Speed over the last seconds, smoothed so the number does not jump.
        auto sample_time = Clock::now();
        std::uint64_t sample_bytes = have;
        double speed = 0;
        const auto progress = [&](std::uint64_t done, std::uint64_t total) {
            const auto now = Clock::now();
            const double seconds = std::chrono::duration<double>(now - sample_time).count();
            if (seconds >= 0.5) {
                const double current = static_cast<double>(done - std::min(done, sample_bytes)) / seconds;
                speed = speed == 0 ? current : speed * 0.7 + current * 0.3;
                sample_time = now;
                sample_bytes = done;
            }
            const std::scoped_lock lock{mutex};
            for (auto& s : status) {
                if (s.key != request.key) continue;
                s.done = done;
                s.total = total;
                s.speed = static_cast<std::uint64_t>(speed);
            }
        };
        const std::uint64_t limit = std::min(MaxArchive, free - SpaceMargin + have);
        std::string detail;
        auto result = Web::DownloadError::None;
        for (int attempt = 0; attempt <= Retries; ++attempt) {
            if (attempt > 0) {
                LOG_INFO(Frontend, "Mods: {} retry {} after: {}", request.key, attempt, detail);
                if (!Wait(1 << attempt, local.get_token())) {
                    result = Web::DownloadError::Cancelled;
                    break;
                }
            }
            result = Web::Download(request.url, part, request.md5, limit, local.get_token(), progress, detail);
            if (result != Web::DownloadError::Network || local.stop_requested()) break;
        }
        if (result == Web::DownloadError::Network && local.stop_requested()) result = Web::DownloadError::Cancelled;
        error = FromDownload(result);
        if (error == ModError::None) {
            fs::rename(part, file, ec);
            if (ec) error = ModError::Write;
        }
        if (error != ModError::None && error != ModError::Cancelled)
            LOG_WARNING(Frontend, "Mods: download of {} failed: {}", request.url, detail);
        else if (error == ModError::None)
            LOG_INFO(Frontend, "Mods: downloaded {} ({} bytes)", request.key, fs::file_size(file, ec));
    }
    const std::scoped_lock lock{mutex};
    std::erase_if(status, [&](const DownloadStatus& s) { return s.key == request.key; });
    // A download the player cancelled says nothing; its part stays for the next time.
    if (error != ModError::Cancelled) finished.push_back({std::move(request), file, error});
}

void ModDownloads::Trim(const fs::path& folder) {
    std::error_code ec;
    const auto now = fs::file_time_type::clock::now();
    for (fs::directory_iterator it{folder, ec}, end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const auto age = now - it->last_write_time(ec);
        const bool part = it->path().extension() == ".part";
        if (age > (part ? std::chrono::hours{24 * 7} : std::chrono::hours{24})) fs::remove(it->path(), ec);
    }
}

} // namespace EdenXbox::Mods
