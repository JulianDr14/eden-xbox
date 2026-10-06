// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Bug tracker: records graphics bugs, unsupported GPU features, failed API calls and every
// UNIMPLEMENTED/assert of the emulator in LogDir/eden_graphics_bugs.log (readable) and
// eden_graphics_bugs.json (summary), deduplicated and counted.
//
// Cost model (the tracker is always on, so it must be close to free):
// - A repeated hit of a known bug is a few relaxed loads and stores on that site's own static
//   counters: no lock-prefixed instruction, no call, no allocation, and the message arguments are
//   not even evaluated. Counts are exact on one thread and may undercount under true concurrency.
// - Only the first hit of a site (or of a new detail key) formats its message, in an out-of-line
//   cold function, and hands it to a bounded lock-free queue. It never blocks.
// - All file I/O happens on a low-priority writer thread, never on an emulator thread.

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <fmt/format.h>

namespace Common::Log {
enum class Class : std::uint8_t;
} // namespace Common::Log

#if defined(__GNUC__) || defined(__clang__)
#define EDEN_BUG_TRACKER_COLD __attribute__((noinline, cold))
#else
#define EDEN_BUG_TRACKER_COLD __declspec(noinline)
#endif

namespace Common::BugTracker {

enum class Category : std::uint8_t {
    Unimplemented,     ///< UNIMPLEMENTED* anywhere in the emulator.
    AssertFailed,      ///< ASSERT* / UNREACHABLE* anywhere in the emulator.
    Stubbed,           ///< HLE function logged as stubbed or not implemented.
    UnsupportedState,  ///< GPU state the renderer cannot express and ignores or approximates.
    UnsupportedFormat, ///< Texture/vertex format with no host equivalent.
    ShaderCompile,     ///< Shader recompilation or DXIL translation/validation failure.
    PipelineRejected,  ///< The driver refused a pipeline state object.
    DrawSkipped,       ///< A draw or dispatch was dropped.
    CopySkipped,       ///< A copy, blit, clear or transfer was skipped or approximated.
    ApiCallFailed,     ///< A graphics API call returned a failure HRESULT.
    DeviceRemoved,     ///< The GPU device was removed or hung.
    NonFinite,         ///< NaN/Inf found in GPU output (trace mode).
    GpuHle,            ///< GPU engines, nvdrv and nvmap warnings.
    Renderer,          ///< Other renderer warnings and errors.
    Other,             ///< Other warnings and errors.
    Ignored,           ///< Internal: deduplicated but never reported.
    Count,
};

[[nodiscard]] std::string_view Name(Category category) noexcept;

namespace Detail {
inline std::atomic<bool> enabled{false};
inline std::atomic<std::uint32_t> frame{0};
inline thread_local bool tap_muted = false;
} // namespace Detail

/// Keeps the logger tap from recording the logs of this scope: a site already reported with
/// BUG_TRACK would otherwise appear twice. Place it right before the LOG_* call it covers.
class TapMute {
public:
    TapMute() noexcept : previous{Detail::tap_muted} {
        Detail::tap_muted = true;
    }
    ~TapMute() noexcept {
        Detail::tap_muted = previous;
    }
    TapMute(const TapMute&) = delete;
    TapMute& operator=(const TapMute&) = delete;

private:
    bool previous;
};

/// One relaxed load; false until Install() and after Shutdown().
[[nodiscard]] inline bool Enabled() noexcept {
    return Detail::enabled.load(std::memory_order_relaxed);
}

[[nodiscard]] inline std::uint32_t CurrentFrame() noexcept {
    return Detail::frame.load(std::memory_order_relaxed);
}

/// Called once per presented frame by the GPU thread (single writer: no read-modify-write).
inline void TickFrame() noexcept {
    Detail::frame.store(CurrentFrame() + 1, std::memory_order_relaxed);
}

/// Increments a counter without a lock-prefixed instruction. Exact for a single thread; under true
/// concurrency an increment may be lost, which a bug counter can afford.
inline void Bump(std::atomic<std::uint64_t>& counter) noexcept {
    counter.store(counter.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
}

/// Per call site state. Constant-initialized (no guard variable, no constructor at run time).
struct Site {
    static constexpr std::size_t DETAIL_SLOTS = 16;

    struct Slot {
        std::atomic<std::uint64_t> key{0}; ///< Stored key (never 0 once claimed).
        std::atomic<std::uint64_t> hits{0};
    };

    constexpr Site() noexcept : Site(Category::Other, nullptr, 0) {}
    constexpr Site(Category category_, const char* file_, std::uint32_t line_) noexcept
        : category{category_}, file{file_}, line{line_} {}

    Category category;
    const char* file;
    std::uint32_t line;
    std::atomic<std::uint64_t> hits{0};
    std::atomic<std::uint64_t> overflow_hits{0}; ///< Detail keys beyond DETAIL_SLOTS.
    std::atomic<std::uint32_t> first_frame{0};
    std::atomic<std::uint32_t> last_frame{0};
    std::atomic<bool> seen{false};
    std::atomic<bool> overflowed{false};
    std::atomic<bool> registered{false};
    std::atomic<Site*> next{nullptr};
    std::array<Slot, DETAIL_SLOTS> slots{};

    /// Records a hit; true only on the first hit of this site.
    bool Hit() noexcept {
        Bump(hits);
        last_frame.store(CurrentFrame(), std::memory_order_relaxed);
        if (seen.load(std::memory_order_relaxed)) [[likely]] {
            return false;
        }
        return !seen.exchange(true, std::memory_order_relaxed);
    }

    /// Records a hit for a detail key (format, shader hash, HRESULT...); true only on the first
    /// hit of that key, and once when the table first overflows.
    bool Hit(std::uint64_t key) noexcept {
        Bump(hits);
        last_frame.store(CurrentFrame(), std::memory_order_relaxed);
        const std::uint64_t stored = key + 1 != 0 ? key + 1 : 1;
        std::size_t index = static_cast<std::size_t>((stored * 0x9E3779B97F4A7C15ULL) >> 60);
        for (std::size_t probe = 0; probe < DETAIL_SLOTS; ++probe) {
            Slot& slot = slots[index];
            std::uint64_t current = slot.key.load(std::memory_order_relaxed);
            if (current == stored) [[likely]] {
                Bump(slot.hits);
                return false;
            }
            if (current == 0) {
                if (slot.key.compare_exchange_strong(current, stored, std::memory_order_relaxed)) {
                    Bump(slot.hits);
                    seen.store(true, std::memory_order_relaxed);
                    return true;
                }
                if (current == stored) {
                    Bump(slot.hits);
                    return false;
                }
            }
            index = (index + 1) % DETAIL_SLOTS;
        }
        Bump(overflow_hits);
        if (overflowed.load(std::memory_order_relaxed)) [[likely]] {
            return false;
        }
        return !overflowed.exchange(true, std::memory_order_relaxed);
    }
};

/// Hands a first occurrence to the writer. Never blocks; drops (and counts) when the queue is full.
void Submit(Site& site, std::uint64_t key, bool keyed, std::string_view message) noexcept;

template <typename... Args>
EDEN_BUG_TRACKER_COLD void FirstHit(Site& site, std::uint64_t key, bool keyed,
                                  fmt::format_string<Args...> format, Args&&... args) noexcept {
    char buffer[480];
    try {
        const auto result =
            fmt::format_to_n(buffer, sizeof(buffer), format, std::forward<Args>(args)...);
        Submit(site, key, keyed,
               std::string_view(buffer, (std::min)(result.size, sizeof(buffer))));
    } catch (...) {
        Submit(site, key, keyed, "<message could not be formatted>");
    }
}

/// Pure helpers, exposed for tools/xbox/tests/bug-tracker.cpp.
namespace Testing {
/// Category of a logged Warning+ entry (the logger tap).
[[nodiscard]] Category ClassifyLog(Log::Class log_class, std::string_view file,
                                   std::string_view message) noexcept;
/// Category of a failed ASSERT/UNIMPLEMENTED/UNREACHABLE `what` string (the assert hook).
[[nodiscard]] Category ClassifyAssert(std::string_view what) noexcept;
/// "C:/x/src/video_core/a.cpp:12: assert c" -> {"video_core/a.cpp", 12}.
[[nodiscard]] std::pair<std::string, std::uint32_t> ParseAssertLocation(std::string_view what);
/// Absolute __FILE__ -> path below src/, with forward slashes.
[[nodiscard]] std::string TrimSourcePath(std::string_view file);
void AppendJsonString(std::string& out, std::string_view text);
/// Lock-free first-occurrence queue: false when full.
bool Enqueue(Site& site, std::uint64_t key, bool keyed, std::string_view message) noexcept;
/// Consumer side: message of the next queued record, or false when empty.
bool Dequeue(Category& category, std::uint64_t& key, std::string& message);
void ResetQueue() noexcept;
[[nodiscard]] std::uint64_t Dropped() noexcept;
} // namespace Testing

/// Starts the writer and installs the logger tap and assert hook. Idempotent.
void Install(const std::filesystem::path& log_dir, std::string build_info);
/// Records the running game in both files.
void SetTitle(std::uint64_t title_id, std::string name);
/// Asks the writer to write everything now (device removed, before an abort).
void RequestFlush() noexcept;
/// Writes the final summary and stops the writer.
void Shutdown();
/// Crash handlers only: appends pending first occurrences with plain C stdio, without locks.
void DrainForCrash() noexcept;

} // namespace Common::BugTracker

/// Records a bug at this call site. The format arguments are evaluated only on its first hit.
#define BUG_TRACK(category, ...)                                                                   \
    do {                                                                                           \
        if (::Common::BugTracker::Enabled()) {                                                       \
            static constinit ::Common::BugTracker::Site eden_bug_site_{                              \
                ::Common::BugTracker::Category::category, __FILE__, __LINE__};                       \
            if (eden_bug_site_.Hit()) [[unlikely]] {                                               \
                ::Common::BugTracker::FirstHit(eden_bug_site_, 0, false, __VA_ARGS__);               \
            }                                                                                      \
        }                                                                                          \
    } while (0)

/// As BUG_TRACK, but each distinct key (format, shader hash, HRESULT...) is reported once.
#define BUG_TRACK_KEY(category, key, ...)                                                          \
    do {                                                                                           \
        if (::Common::BugTracker::Enabled()) {                                                       \
            static constinit ::Common::BugTracker::Site eden_bug_site_{                              \
                ::Common::BugTracker::Category::category, __FILE__, __LINE__};                       \
            const ::std::uint64_t eden_bug_key_ = static_cast<::std::uint64_t>(key);               \
            if (eden_bug_site_.Hit(eden_bug_key_)) [[unlikely]] {                                  \
                ::Common::BugTracker::FirstHit(eden_bug_site_, eden_bug_key_, true, __VA_ARGS__);    \
            }                                                                                      \
        }                                                                                          \
    } while (0)
