// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <mutex>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <share.h>
#include <windows.h>
#endif

#include "common/assert.h"
#include "common/bug_tracker.h"
#include "common/logging.h"

namespace Common::BugTracker {

namespace {

using Clock = std::chrono::steady_clock;

// ---------------------------------------------------------------------------------------------
// First-occurrence queue: bounded lock-free MPSC (Vyukov). Producers are any emulator thread; the
// only consumer is the writer thread (or a crash handler, once the writer has been excluded).

constexpr std::size_t QUEUE_SIZE = 256;
constexpr std::size_t MESSAGE_SIZE = 480;

struct Record {
    Site* site;
    std::uint64_t key;
    std::uint64_t ms;
    std::uint32_t frame;
    std::uint16_t length;
    bool keyed;
    char message[MESSAGE_SIZE];
};

struct Cell {
    std::atomic<std::size_t> sequence{0};
    Record record{};
};

std::array<Cell, QUEUE_SIZE> queue_cells;
std::atomic<std::size_t> enqueue_position{0};
std::size_t dequeue_position = 0;
std::atomic<bool> consumer_busy{false};
std::atomic<std::uint64_t> dropped{0};
Clock::time_point origin = Clock::now();

bool EnqueueRecord(Site& site, std::uint64_t key, bool keyed, std::string_view message) noexcept {
    std::size_t position = enqueue_position.load(std::memory_order_relaxed);
    Cell* cell;
    for (;;) {
        cell = &queue_cells[position % QUEUE_SIZE];
        const std::size_t sequence = cell->sequence.load(std::memory_order_acquire);
        const auto difference =
            static_cast<std::ptrdiff_t>(sequence) - static_cast<std::ptrdiff_t>(position);
        if (difference == 0) {
            if (enqueue_position.compare_exchange_weak(position, position + 1,
                                                       std::memory_order_relaxed)) {
                break;
            }
        } else if (difference < 0) {
            dropped.fetch_add(1, std::memory_order_relaxed);
            return false;
        } else {
            position = enqueue_position.load(std::memory_order_relaxed);
        }
    }
    Record& record = cell->record;
    record.site = &site;
    record.key = key;
    record.keyed = keyed;
    record.frame = CurrentFrame();
    record.ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - origin).count());
    record.length = static_cast<std::uint16_t>((std::min)(message.size(), MESSAGE_SIZE));
    std::memcpy(record.message, message.data(), record.length);
    cell->sequence.store(position + 1, std::memory_order_release);
    return true;
}

/// Consumer only.
bool DequeueRecord(Record& out) noexcept {
    Cell& cell = queue_cells[dequeue_position % QUEUE_SIZE];
    if (cell.sequence.load(std::memory_order_acquire) != dequeue_position + 1) {
        return false;
    }
    out = cell.record;
    cell.sequence.store(dequeue_position + QUEUE_SIZE, std::memory_order_release);
    ++dequeue_position;
    return true;
}

void ResetQueueCells() noexcept {
    for (std::size_t i = 0; i < QUEUE_SIZE; ++i) {
        queue_cells[i].sequence.store(i, std::memory_order_relaxed);
    }
    enqueue_position.store(0, std::memory_order_relaxed);
    dequeue_position = 0;
    dropped.store(0, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------------------------
// Registered sites: an append-only intrusive list, walked by the writer for the summary.

std::atomic<Site*> site_list{nullptr};

void Register(Site& site) noexcept {
    if (site.registered.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    site.first_frame.store(CurrentFrame(), std::memory_order_relaxed);
    Site* head = site_list.load(std::memory_order_relaxed);
    do {
        site.next.store(head, std::memory_order_relaxed);
    } while (!site_list.compare_exchange_weak(head, &site, std::memory_order_release,
                                              std::memory_order_relaxed));
}

// ---------------------------------------------------------------------------------------------
// Dynamic sites for the logger tap and the assert hook, keyed by (file literal, line) or by the
// assert's `what` literal. Fixed open-addressing table: a known site is a hash and a load.

constexpr std::size_t DYNAMIC_SITES = 1024;
constexpr std::size_t MAX_PROBES = 32;
std::array<std::atomic<std::uint64_t>, DYNAMIC_SITES> dynamic_keys{};
std::array<Site, DYNAMIC_SITES> dynamic_sites{};
Site dynamic_overflow{Category::Other, "<more sites than the bug tracker tracks>", 0};

Site& LookupDynamic(std::uint64_t key) noexcept {
    std::size_t index =
        static_cast<std::size_t>((key * 0x9E3779B97F4A7C15ULL) >> 54) % DYNAMIC_SITES;
    for (std::size_t probe = 0; probe < MAX_PROBES; ++probe) {
        auto& slot = dynamic_keys[index];
        std::uint64_t current = slot.load(std::memory_order_acquire);
        if (current == key) {
            return dynamic_sites[index];
        }
        if (current == 0 &&
            (slot.compare_exchange_strong(current, key, std::memory_order_acq_rel) ||
             current == key)) {
            return dynamic_sites[index];
        }
        index = (index + 1) % DYNAMIC_SITES;
    }
    return dynamic_overflow;
}

std::uint64_t LogKey(const char* file, unsigned int line) noexcept {
    // User-mode pointers fit in 47 bits; the line goes above them. Never 0 (file is non-null).
    return reinterpret_cast<std::uintptr_t>(file) ^ (static_cast<std::uint64_t>(line) << 47);
}

std::uint64_t AssertKey(const char* what) noexcept {
    return reinterpret_cast<std::uintptr_t>(what) | (1ULL << 63);
}

bool Contains(std::string_view text, std::string_view needle) noexcept {
    if (needle.size() > text.size()) {
        return false;
    }
    for (std::size_t i = 0; i + needle.size() <= text.size(); ++i) {
        std::size_t j = 0;
        for (; j < needle.size(); ++j) {
            char c = text[i + j];
            if (c >= 'A' && c <= 'Z') {
                c = static_cast<char>(c - 'A' + 'a');
            }
            if (c != needle[j]) {
                break;
            }
        }
        if (j == needle.size()) {
            return true;
        }
    }
    return false;
}

bool EndsWith(std::string_view text, std::string_view suffix) noexcept {
    return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
}

// Logger tap. Phase one counts the hit; only a new site asks for its message.
bool TapSeen(Log::Class, Log::Level, const char* file, unsigned int line) noexcept {
    if (Detail::tap_muted) {
        return false; // reported by an explicit BUG_TRACK at this site
    }
    return LookupDynamic(LogKey(file, line)).Hit();
}

void TapRecord(const Log::TapEntry& entry) noexcept {
    Site& site = LookupDynamic(LogKey(entry.filename, entry.line));
    site.file = entry.filename;
    site.line = entry.line;
    site.category = Testing::ClassifyLog(entry.log_class, entry.filename, entry.message);
    if (site.category == Category::Ignored) {
        return;
    }
    char buffer[MESSAGE_SIZE];
    const auto result = fmt::format_to_n(buffer, sizeof(buffer), "{} {}: {}", entry.level_name,
                                         entry.class_name, entry.message);
    Submit(site, 0, false, std::string_view(buffer, (std::min)(result.size, sizeof(buffer))));
}

void AssertHook(const char* what) noexcept {
    Site& site = LookupDynamic(AssertKey(what));
    if (!site.Hit()) {
        return;
    }
    const std::string_view text{what};
    site.file = nullptr; // the writer takes the location from the message
    site.line = 0;
    site.category = Testing::ClassifyAssert(text);
    const bool unreachable = text.find(": assert ") == std::string_view::npos;
    if (unreachable) {
        char buffer[MESSAGE_SIZE];
        const auto result = fmt::format_to_n(buffer, sizeof(buffer), "{}: unreachable", text);
        Submit(site, 0, false, std::string_view(buffer, (std::min)(result.size, sizeof(buffer))));
    } else {
        Submit(site, 0, false, text);
    }
    if (unreachable) {
        DrainForCrash(); // UnreachableAt aborts right after this hook
    }
}

// ---------------------------------------------------------------------------------------------
// Writer: owns the files; runs at the lowest priority and never touches emulator state.

struct DetailInfo {
    std::uint64_t key;
    std::uint64_t stored;
    std::string message;
};

struct EntryInfo {
    Site* site;
    Category category;
    std::string file;
    std::uint32_t line;
    std::uint64_t first_ms;
    std::uint32_t first_frame;
    std::string message;
    std::vector<DetailInfo> details;
};

class Writer {
public:
    void Start(const std::filesystem::path& dir, std::string build) {
        directory = dir;
        build_info = std::move(build);
        std::error_code ec;
        std::filesystem::create_directories(directory, ec);
        log_path = directory / "eden_graphics_bugs.log";
        json_path = directory / "eden_graphics_bugs.json";
        Rotate(log_path);
        Rotate(json_path);
#ifdef _WIN32
        log_file = _wfsopen(log_path.c_str(), L"wb", _SH_DENYNO);
        crash_path = log_path.wstring();
#else
        log_file = std::fopen(log_path.string().c_str(), "wb");
        crash_path = log_path.string();
#endif
        if (log_file) {
            const std::time_t now = std::time(nullptr);
            std::tm local{};
#ifdef _WIN32
            localtime_s(&local, &now);
#else
            localtime_r(&now, &local);
#endif
            char date[64]{};
            std::strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S", &local);
            fmt::print(log_file,
                       "Eden bug tracker - {}\nStarted {}. Each line is the first occurrence of a "
                       "bug; the summary with counts is in eden_graphics_bugs.json and at the end "
                       "of this file.\n\n",
                       build_info, date);
            std::fflush(log_file);
        }
        thread = std::thread([this] { Run(); });
    }

    void SetTitle(std::uint64_t id, std::string name) {
        {
            std::scoped_lock lock{mutex};
            title_id = id;
            title_name = std::move(name);
            title_changed = true;
            flush_requested = true;
        }
        condition.notify_one();
    }

    void RequestFlush() {
        {
            std::scoped_lock lock{mutex};
            flush_requested = true;
        }
        condition.notify_one();
    }

    void Stop() {
        if (!thread.joinable()) {
            return;
        }
        {
            std::scoped_lock lock{mutex};
            stop_requested = true;
        }
        condition.notify_one();
        thread.join();
    }

    const auto& CrashPath() const noexcept {
        return crash_path;
    }

private:
    static void Rotate(const std::filesystem::path& path) {
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) {
            return;
        }
        auto old_path = path;
        old_path.replace_extension(".old" + path.extension().string());
        std::filesystem::remove(old_path, ec);
        std::filesystem::rename(path, old_path, ec);
    }

    void Run() {
#ifdef _WIN32
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
#endif
        auto last_json = Clock::now();
        for (;;) {
            bool stopping;
            bool flush;
            bool new_title;
            std::uint64_t id;
            std::string name;
            {
                std::unique_lock lock{mutex};
                condition.wait_for(lock, std::chrono::seconds(1),
                                   [this] { return stop_requested || flush_requested; });
                stopping = stop_requested;
                flush = flush_requested;
                flush_requested = false;
                new_title = title_changed;
                title_changed = false;
                id = title_id;
                name = title_name;
            }
            if (new_title) {
                WriteTitle(id, name);
            }
            Drain();
            const auto now = Clock::now();
            if (stopping || flush || now - last_json >= std::chrono::seconds(10)) {
                last_json = now;
                const std::uint64_t total = TotalHits();
                if (stopping || flush || total != json_total) {
                    json_total = total;
                    WriteJson(id, name);
                }
            }
            if (stopping) {
                WriteSummaryTable();
                if (log_file) {
                    std::fclose(log_file);
                    log_file = nullptr;
                }
                return;
            }
        }
    }

    void WriteTitle(std::uint64_t id, const std::string& name) {
        if (!log_file) {
            return;
        }
        fmt::print(log_file, "=== Game {:016X} {} (frame {}) ===\n", id, name, CurrentFrame());
        std::fflush(log_file);
    }

    void Drain() {
        if (consumer_busy.exchange(true, std::memory_order_acquire)) {
            return; // a crash handler is draining
        }
        Record record;
        bool wrote = false;
        while (DequeueRecord(record)) {
            Add(record);
            wrote = true;
        }
        consumer_busy.store(false, std::memory_order_release);
        if (wrote && log_file) {
            std::fflush(log_file);
        }
    }

    void Add(const Record& record) {
        const std::string_view message{record.message, record.length};
        Site* const site = record.site;
        auto [it, inserted] = index_by_site.try_emplace(site, entries.size());
        if (inserted) {
            EntryInfo info{
                .site = site,
                .category = site->category,
                .file = {},
                .line = site->line,
                .first_ms = record.ms,
                .first_frame = record.frame,
                .message = record.keyed ? std::string{} : std::string{message},
                .details = {},
            };
            if (site->file) {
                info.file = Testing::TrimSourcePath(site->file);
            } else {
                auto [file, line] = Testing::ParseAssertLocation(message);
                info.file = std::move(file);
                info.line = line;
            }
            entries.push_back(std::move(info));
        }
        EntryInfo& entry = entries[it->second];
        if (record.keyed) {
            const std::uint64_t stored = record.key + 1 != 0 ? record.key + 1 : 1;
            entry.details.push_back({record.key, stored, std::string{message}});
            if (entry.message.empty()) {
                entry.message = std::string{message};
            }
        }
        if (!log_file) {
            return;
        }
        if (record.keyed) {
            fmt::print(log_file, "[+{}.{:03}s f={}] [{}] {}:{} key={:#x} {}\n", record.ms / 1000,
                       record.ms % 1000, record.frame, Name(entry.category), entry.file,
                       entry.line, record.key, message);
        } else {
            fmt::print(log_file, "[+{}.{:03}s f={}] [{}] {}:{} {}\n", record.ms / 1000,
                       record.ms % 1000, record.frame, Name(entry.category), entry.file,
                       entry.line, message);
        }
    }

    static std::uint64_t TotalHits() noexcept {
        std::uint64_t total = 0;
        for (Site* site = site_list.load(std::memory_order_acquire); site;
             site = site->next.load(std::memory_order_relaxed)) {
            total += site->hits.load(std::memory_order_relaxed);
        }
        return total;
    }

    static std::uint64_t DetailHits(const Site& site, std::uint64_t stored) noexcept {
        for (const auto& slot : site.slots) {
            if (slot.key.load(std::memory_order_relaxed) == stored) {
                return slot.hits.load(std::memory_order_relaxed);
            }
        }
        return 0;
    }

    void WriteJson(std::uint64_t id, const std::string& name) {
        std::string out;
        out.reserve(4096 + entries.size() * 256);
        out += "{\n  \"build\": ";
        Testing::AppendJsonString(out, build_info);
        fmt::format_to(std::back_inserter(out), ",\n  \"title_id\": \"{:016X}\",\n  \"title\": ",
                       id);
        Testing::AppendJsonString(out, name);
        fmt::format_to(std::back_inserter(out),
                       ",\n  \"frames\": {},\n  \"dropped_reports\": {},\n  \"entries\": [",
                       CurrentFrame(), dropped.load(std::memory_order_relaxed));
        bool first = true;
        for (const EntryInfo& entry : entries) {
            const Site& site = *entry.site;
            out += first ? "\n    {" : ",\n    {";
            first = false;
            out += "\"category\": ";
            Testing::AppendJsonString(out, Name(entry.category));
            out += ", \"file\": ";
            Testing::AppendJsonString(out, entry.file);
            fmt::format_to(std::back_inserter(out),
                           ", \"line\": {}, \"hits\": {}, \"first_frame\": {}, \"last_frame\": {}, "
                           "\"first_ms\": {}, \"message\": ",
                           entry.line, site.hits.load(std::memory_order_relaxed),
                           entry.first_frame, site.last_frame.load(std::memory_order_relaxed),
                           entry.first_ms);
            Testing::AppendJsonString(out, entry.message);
            if (!entry.details.empty()) {
                out += ", \"details\": [";
                for (std::size_t i = 0; i < entry.details.size(); ++i) {
                    const DetailInfo& detail = entry.details[i];
                    fmt::format_to(std::back_inserter(out),
                                   "{}{{\"key\": \"{:#x}\", \"hits\": {}, \"message\": ",
                                   i ? ", " : "", detail.key, DetailHits(site, detail.stored));
                    Testing::AppendJsonString(out, detail.message);
                    out += "}";
                }
                fmt::format_to(std::back_inserter(out), "], \"other_detail_hits\": {}",
                               site.overflow_hits.load(std::memory_order_relaxed));
            }
            out += "}";
        }
        out += "\n  ]\n}\n";

        auto temporary = json_path;
        temporary += ".tmp";
#ifdef _WIN32
        std::FILE* file = _wfsopen(temporary.c_str(), L"wb", _SH_DENYNO);
#else
        std::FILE* file = std::fopen(temporary.string().c_str(), "wb");
#endif
        if (!file) {
            return;
        }
        const bool written = std::fwrite(out.data(), 1, out.size(), file) == out.size();
        std::fclose(file);
        std::error_code ec;
        if (written) {
            std::filesystem::rename(temporary, json_path, ec); // replaces the previous summary
        }
    }

    void WriteSummaryTable() {
        if (!log_file) {
            return;
        }
        fmt::print(log_file, "\n=== Summary at frame {} ({} distinct, {} reports dropped) ===\n",
                   CurrentFrame(), entries.size(), dropped.load(std::memory_order_relaxed));
        for (const EntryInfo& entry : entries) {
            fmt::print(log_file, "{:>10}  {:<17} {}:{}  {}\n",
                       entry.site->hits.load(std::memory_order_relaxed), Name(entry.category),
                       entry.file, entry.line, entry.message.substr(0, 120));
        }
    }

    std::filesystem::path directory;
    std::filesystem::path log_path;
    std::filesystem::path json_path;
#ifdef _WIN32
    std::wstring crash_path;
#else
    std::string crash_path;
#endif
    std::string build_info;
    std::FILE* log_file = nullptr;
    std::thread thread;
    std::mutex mutex;
    std::condition_variable condition;
    bool stop_requested = false;
    bool flush_requested = false;
    bool title_changed = false;
    std::uint64_t title_id = 0;
    std::string title_name;
    std::uint64_t json_total = ~0ULL;
    std::vector<EntryInfo> entries;
    std::unordered_map<Site*, std::size_t> index_by_site;
};

std::mutex install_mutex;
std::unique_ptr<Writer> writer;

/// Joins the writer before static destruction (a joinable std::thread would terminate).
struct ShutdownAtExit {
    ~ShutdownAtExit() {
        Shutdown();
    }
} shutdown_at_exit;

} // Anonymous namespace

std::string_view Name(Category category) noexcept {
    switch (category) {
    case Category::Unimplemented:
        return "Unimplemented";
    case Category::AssertFailed:
        return "AssertFailed";
    case Category::Stubbed:
        return "Stubbed";
    case Category::UnsupportedState:
        return "UnsupportedState";
    case Category::UnsupportedFormat:
        return "UnsupportedFormat";
    case Category::ShaderCompile:
        return "ShaderCompile";
    case Category::PipelineRejected:
        return "PipelineRejected";
    case Category::DrawSkipped:
        return "DrawSkipped";
    case Category::CopySkipped:
        return "CopySkipped";
    case Category::ApiCallFailed:
        return "ApiCallFailed";
    case Category::DeviceRemoved:
        return "DeviceRemoved";
    case Category::NonFinite:
        return "NonFinite";
    case Category::GpuHle:
        return "GpuHle";
    case Category::Renderer:
        return "Renderer";
    case Category::Other:
        return "Other";
    case Category::Ignored:
    case Category::Count:
        break;
    }
    return "Ignored";
}

void Submit(Site& site, std::uint64_t key, bool keyed, std::string_view message) noexcept {
    Register(site);
    if (site.category != Category::Ignored) {
        EnqueueRecord(site, key, keyed, message);
    }
}

namespace Testing {

Category ClassifyLog(Log::Class log_class, std::string_view file,
                     std::string_view message) noexcept {
    using Log::Class;
    // AssertFailedAt/UnreachableAt reach the tracker through the assert hook, with the real site.
    if (EndsWith(file, "assert.cpp")) {
        return Category::Ignored;
    }
    if (log_class == Class::Debug) {
        return Contains(message, ": unimplemented ") ? Category::Unimplemented
                                                      : Category::AssertFailed;
    }
    if (Contains(message, "stub")) {
        return Category::Stubbed;
    }
    if (Contains(message, "device removed") || Contains(message, "device_removed") ||
        Contains(message, "device hung")) {
        return Category::DeviceRemoved;
    }
    if (Contains(message, "not implemented") || Contains(message, "unimplemented")) {
        return Category::Unimplemented;
    }
    switch (log_class) {
    case Class::Shader:
    case Class::Shader_SPIRV:
    case Class::Shader_GLASM:
    case Class::Shader_GLSL:
        return Category::ShaderCompile;
    case Class::Render:
    case Class::Render_Software:
    case Class::Render_OpenGL:
    case Class::Render_Vulkan:
        if (Contains(message, "unsupported") || Contains(message, "not supported")) {
            return Category::UnsupportedState;
        }
        return Category::Renderer;
    case Class::HW_GPU:
    case Class::Service_NVDRV:
    case Class::Debug_GPU:
        return Category::GpuHle;
    default:
        return Category::Other;
    }
}

Category ClassifyAssert(std::string_view what) noexcept {
    return Contains(what, "unimplemented!") ? Category::Unimplemented : Category::AssertFailed;
}

std::string TrimSourcePath(std::string_view file) {
    std::string path{file};
    for (char& c : path) {
        if (c == '\\') {
            c = '/';
        }
    }
    const std::size_t src = path.rfind("/src/");
    if (src != std::string::npos) {
        path.erase(0, src + 5);
    }
    return path;
}

std::pair<std::string, std::uint32_t> ParseAssertLocation(std::string_view what) {
    std::string_view location = what;
    if (const std::size_t end = what.find(": "); end != std::string_view::npos) {
        location = what.substr(0, end);
    }
    const std::size_t colon = location.rfind(':');
    if (colon == std::string_view::npos || colon + 1 >= location.size()) {
        return {TrimSourcePath(location), 0};
    }
    std::uint32_t line = 0;
    for (const char c : location.substr(colon + 1)) {
        if (c < '0' || c > '9') {
            return {TrimSourcePath(location), 0};
        }
        line = line * 10 + static_cast<std::uint32_t>(c - '0');
    }
    return {TrimSourcePath(location.substr(0, colon)), line};
}

void AppendJsonString(std::string& out, std::string_view text) {
    out += '"';
    for (const char c : text) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                fmt::format_to(std::back_inserter(out), "\\u{:04x}", static_cast<unsigned>(c));
            } else {
                out += c;
            }
        }
    }
    out += '"';
}

bool Enqueue(Site& site, std::uint64_t key, bool keyed, std::string_view message) noexcept {
    return EnqueueRecord(site, key, keyed, message);
}

bool Dequeue(Category& category, std::uint64_t& key, std::string& message) {
    Record record;
    if (!DequeueRecord(record)) {
        return false;
    }
    category = record.site->category;
    key = record.key;
    message.assign(record.message, record.length);
    return true;
}

void ResetQueue() noexcept {
    ResetQueueCells();
}

std::uint64_t Dropped() noexcept {
    return dropped.load(std::memory_order_relaxed);
}

} // namespace Testing

void Install(const std::filesystem::path& log_dir, std::string build_info) {
    std::scoped_lock lock{install_mutex};
    if (writer) {
        return;
    }
    ResetQueueCells();
    origin = Clock::now();
    writer = std::make_unique<Writer>();
    writer->Start(log_dir, std::move(build_info));
    Log::SetTap(&TapSeen, &TapRecord);
    SetAssertHook(&AssertHook);
    Detail::enabled.store(true, std::memory_order_release);
}

void SetTitle(std::uint64_t title_id, std::string name) {
    std::scoped_lock lock{install_mutex};
    if (writer) {
        writer->SetTitle(title_id, std::move(name));
    }
}

void RequestFlush() noexcept {
    try {
        std::scoped_lock lock{install_mutex};
        if (writer) {
            writer->RequestFlush();
        }
    } catch (...) {
    }
}

void Shutdown() {
    std::unique_ptr<Writer> stopping;
    {
        std::scoped_lock lock{install_mutex};
        if (!writer) {
            return;
        }
        Detail::enabled.store(false, std::memory_order_release);
        Log::SetTap(nullptr, nullptr);
        SetAssertHook(nullptr);
        stopping = std::move(writer);
    }
    stopping->Stop();
}

void DrainForCrash() noexcept {
    Writer* const current = writer.get();
    if (!current || consumer_busy.exchange(true, std::memory_order_acquire)) {
        return;
    }
#ifdef _WIN32
    std::FILE* file = _wfsopen(current->CrashPath().c_str(), L"ab", _SH_DENYNO);
#else
    std::FILE* file = std::fopen(current->CrashPath().c_str(), "ab");
#endif
    if (file) {
        Record record;
        while (DequeueRecord(record)) {
            const char* file_name = record.site->file ? record.site->file : "";
            std::fprintf(file, "[+%llu.%03llus f=%u] [%.*s] %s:%u %.*s\n",
                         static_cast<unsigned long long>(record.ms / 1000),
                         static_cast<unsigned long long>(record.ms % 1000), record.frame,
                         static_cast<int>(Name(record.site->category).size()),
                         Name(record.site->category).data(), file_name, record.site->line,
                         static_cast<int>(record.length), record.message);
        }
        std::fputs("=== Process is crashing; the JSON summary may be up to 10 s old ===\n", file);
        std::fclose(file);
    }
    consumer_busy.store(false, std::memory_order_release);
}

} // namespace Common::BugTracker
