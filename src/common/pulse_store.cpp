// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#include "common/pulse_store.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <streambuf>
#include <string>

#ifdef _WIN32
#include <windows.h>
#include <fileapifromapp.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include "common/cityhash.h"
#include "common/logging.h"
#include <unordered_map>

namespace Common::Pulse {
namespace {
long long MillisecondsSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                 start)
        .count();
}

class LimitedBuffer final : public std::streambuf {
public:
    LimitedBuffer(std::streambuf* target_, std::uint64_t limit_) : target{target_}, remaining{limit_} {}
private:
    std::streamsize xsputn(const char* source, std::streamsize size) override {
        if (size < 0 || static_cast<std::uint64_t>(size) > remaining) return 0;
        const auto written = target->sputn(source, size);
        remaining -= static_cast<std::uint64_t>(written);
        return written;
    }
    int_type overflow(int_type value) override {
        if (traits_type::eq_int_type(value, traits_type::eof())) return traits_type::not_eof(value);
        const char byte = traits_type::to_char_type(value);
        return xsputn(&byte, 1) == 1 ? value : traits_type::eof();
    }
    int sync() override { return target->pubsync(); }
    std::streambuf* target;
    std::uint64_t remaining;
};

bool SyncFile(const std::filesystem::path& path) {
#ifdef _WIN32
    const auto handle = CreateFile2FromAppW(path.c_str(), GENERIC_WRITE, 0, OPEN_EXISTING, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    const bool ok = FlushFileBuffers(handle) != 0;
    CloseHandle(handle);
    return ok;
#else
    const int fd = open(path.c_str(), O_WRONLY);
    if (fd < 0) return false;
    const bool ok = fsync(fd) == 0;
    close(fd);
    return ok;
#endif
}
bool Replace(const std::filesystem::path& next, const std::filesystem::path& current) {
#ifdef _WIN32
    const auto attributes = GetFileAttributesW(current.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        if (GetLastError() != ERROR_FILE_NOT_FOUND) return false;
        return MoveFileFromAppW(next.c_str(), current.c_str()) != 0;
    }
    return ReplaceFileFromAppW(current.c_str(), next.c_str(), nullptr, 0, nullptr, nullptr) != 0;
#else
    std::error_code ec;
    std::filesystem::rename(next, current, ec);
    if (ec) return false;
    const int fd = open(current.parent_path().c_str(), O_RDONLY | O_DIRECTORY);
    if (fd < 0) return false;
    const bool ok = fsync(fd) == 0;
    close(fd);
    return ok;
#endif
}
void RequireRegularOrAbsent(const std::filesystem::path& path) {
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(path, ec);
    if (ec == std::errc::no_such_file_or_directory) return;
    if (ec || (status.type() != std::filesystem::file_type::not_found &&
               status.type() != std::filesystem::file_type::regular))
        throw Failure{Error::Io, "Pulse storage entry is not a regular file"};
}
} // namespace

std::string StorageKey(const Identity& identity) {
    constexpr char Hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(48);
    const auto append = [&](std::uint8_t byte) {
        result.push_back(Hex[byte >> 4]);
        result.push_back(Hex[byte & 15]);
    };
    for (unsigned i = 0; i < 8; ++i)
        append(static_cast<std::uint8_t>(identity.program_id >> ((7 - i) * 8)));
    for (auto byte : identity.user) append(byte);
    return result;
}

std::filesystem::path Store::Path(std::string_view key, const char* extension) const {
    if (key.size() != 48 || !std::all_of(key.begin(), key.end(), [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        })) throw Failure{Error::Invalid, "Pulse invalid storage key"};
    return root / (std::string{key} + extension);
}

std::uint64_t Store::Used() const {
    std::uint64_t used{};
    for (const auto& entry : std::filesystem::directory_iterator{root}) {
        if (!std::filesystem::is_regular_file(entry.symlink_status()))
            throw Failure{Error::Io, "Pulse storage contains unsupported entries"};
        const auto size = entry.file_size();
        if (size > UINT64_MAX - used) throw Failure{Error::Limit, "Pulse storage size overflow"};
        used += size;
    }
    return used;
}

void Store::Publish(std::string_view key, const Identity& identity, std::uint64_t max_encoded,
                    std::uint64_t max_decoded, const std::function<void(Writer&)>& serialize,
                    std::stop_token stop, Progress progress) {
    std::scoped_lock lock{mutex};
    if (key != StorageKey(identity)) throw Failure{Error::Invalid, "Pulse key/identity mismatch"};
    const auto current = Path(key, ".pulse");
    const auto next = Path(key, ".next");
    if (stop.stop_requested()) throw Failure{Error::Cancelled, "Pulse operation cancelled"};
    if (!serialize) throw Failure{Error::Invalid, "Pulse missing serializer"};
    std::filesystem::create_directories(root);
    RequireRegularOrAbsent(current);
    RequireRegularOrAbsent(next);
    // An interrupted temporary can be removed; it was never a published session.
    std::filesystem::remove(next);
    if (!Fits(Used(), max_encoded, quota, std::filesystem::space(root).available))
        throw Failure{Error::Limit, "Pulse storage quota or free reserve exceeded"};
    try {
        auto phase = std::chrono::steady_clock::now();
        long long write_ms{}, verify_ms{};
        if (fault) fault(PublishStep::Write);
        {
            std::ofstream file{next, std::ios::binary | std::ios::trunc};
            if (!file) throw Failure{Error::Io, "Pulse temporary open failed"};
            LimitedBuffer buffer{file.rdbuf(), max_encoded};
            std::ostream output{&buffer};
            Writer writer{output, identity, stop, std::move(progress)};
            serialize(writer);
            writer.Finish();
            file.close();
            if (!file) throw Failure{Error::Io, "Pulse temporary close failed"};
        }
        write_ms = MillisecondsSince(phase);
        phase = std::chrono::steady_clock::now();
        if (fault) fault(PublishStep::Verify);
        {
            std::ifstream input{next, std::ios::binary};
            Read(input, identity, max_decoded, {}, stop);
        }
        verify_ms = MillisecondsSince(phase);
        phase = std::chrono::steady_clock::now();
        if (fault) fault(PublishStep::Sync);
        if (!SyncFile(next)) throw Failure{Error::Io, "Pulse durable flush failed"};
        LOG_INFO(Common, "Pulse: session written in {} ms, verified in {} ms, synced in {} ms ({} MiB)",
                 write_ms, verify_ms, MillisecondsSince(phase),
                 std::filesystem::file_size(next) >> 20);
        if (stop.stop_requested()) throw Failure{Error::Cancelled, "Pulse operation cancelled"};
        if (std::filesystem::space(root).available < FreeReserve)
            throw Failure{Error::Limit, "Pulse free reserve exhausted"};
        if (fault) fault(PublishStep::Replace);
        if (!Replace(next, current)) throw Failure{Error::Io, "Pulse publication failed"};
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(next, ignored);
        throw;
    }
}

void Store::Verify(std::string_view key, const Identity& identity, std::uint64_t max_decoded) {
    std::scoped_lock lock{mutex};
    if (key != StorageKey(identity)) throw Failure{Error::Invalid, "Pulse key/identity mismatch"};
    const auto path = Path(key, ".pulse");
    RequireRegularOrAbsent(path);
    std::ifstream input{path, std::ios::binary};
    Read(input, identity, max_decoded);
}

void Store::RestoreAndConsume(std::string_view key, const Identity& identity,
                             std::uint64_t max_decoded, const WriteBlock& rebuild,
                             std::stop_token stop, Progress progress) {
    std::scoped_lock lock{mutex};
    if (key != StorageKey(identity) || !rebuild)
        throw Failure{Error::Invalid, "Pulse invalid restore request"};
    const auto current = Path(key, ".pulse");
    const auto consumed = Path(key, ".consumed");
    RequireRegularOrAbsent(current);
    RequireRegularOrAbsent(consumed);
    {
        std::ifstream input{current, std::ios::binary};
        Read(input, identity, max_decoded, {}, stop);
        // Keep the same file open through both passes; no guest execution here.
        input.clear();
        input.seekg(0);
        if (!input) throw Failure{Error::Io, "Pulse restore rewind failed"};
        if (fault) fault(PublishStep::Rebuild);
        Read(input, identity, max_decoded, rebuild, stop, std::move(progress));
    }
    if (stop.stop_requested()) throw Failure{Error::Cancelled, "Pulse restore cancelled"};
    if (fault) fault(PublishStep::Consume);
    // A crash after this point cannot expose an old snapshot as resumable. The
    // caller may now Run the candidate; consumed data remains until explicit discard.
    if (!Replace(current, consumed)) throw Failure{Error::Io, "Pulse durable consume failed"};
    if (fault) fault(PublishStep::Consumed);
}

std::vector<Entry> Store::List() {
    std::scoped_lock lock{mutex};
    std::vector<Entry> entries;
    std::error_code ec;
    if (!std::filesystem::exists(root, ec)) {
        if (ec) throw Failure{Error::Io, "Pulse catalogue inaccessible"};
        return entries;
    }
    unsigned examined{};
    for (const auto& file : std::filesystem::directory_iterator{root}) {
        if (++examined > 10000) throw Failure{Error::Limit, "Pulse catalogue entry limit exceeded"};
        const auto extension = file.path().extension();
        if (extension != ".pulse" && extension != ".consumed") continue;
        const auto key = file.path().stem().string();
        // Reconstruct through validated key, never trust a path read from a header.
        const auto path = Path(key, extension == ".pulse" ? ".pulse" : ".consumed");
        RequireRegularOrAbsent(path);
        Entry entry{key};
        entry.bytes = std::filesystem::file_size(path);
        entry.modified = std::filesystem::last_write_time(path);
        entry.state = extension == ".pulse" ? EntryState::Available : EntryState::Consumed;
        try {
            std::ifstream input{path, std::ios::binary};
            entry.identity = ReadHeader(input);
            if (StorageKey(entry.identity) != key) throw Failure{Error::Invalid, "Pulse catalogue key mismatch"};
            entry.header_valid = true;
        } catch (const Failure& failure) {
            entry.state = failure.code == Error::Incompatible ? EntryState::UnknownFormat : EntryState::Corrupt;
        }
        entries.push_back(std::move(entry));
    }
    return entries;
}

void Store::Discard(std::string_view key) {
    std::scoped_lock lock{mutex};
    const auto path = Path(key, ".pulse");
    RequireRegularOrAbsent(path);
    std::filesystem::remove(path);
    const auto consumed = Path(key, ".consumed");
    RequireRegularOrAbsent(consumed);
    std::filesystem::remove(consumed);
}
namespace {
constexpr std::uintptr_t ProbePage = 0x1000;
constexpr std::uint64_t UntouchedHash = ~std::uint64_t{0};
std::mutex probe_mutex;
std::unordered_map<std::uintptr_t, std::uint64_t> restored_pages;

std::uint64_t HashPage(std::uintptr_t page) {
    return CityHash64(reinterpret_cast<const char*>(page), ProbePage);
}
} // namespace

void ClearRestoredPages() {
    std::scoped_lock lock{probe_mutex};
    restored_pages = {};
}

void NoteRestoredPage(const void* host_page, bool untouched) {
    const auto page = reinterpret_cast<std::uintptr_t>(host_page) & ~(ProbePage - 1);
    const std::uint64_t hash = untouched ? UntouchedHash : HashPage(page);
    std::scoped_lock lock{probe_mutex};
    restored_pages[page] = hash;
}

std::string DescribeRestoredPages(const void* host, std::size_t size) {
    if (!host || !size) return "no host memory";
    const auto first = reinterpret_cast<std::uintptr_t>(host) & ~(ProbePage - 1);
    const auto end = reinterpret_cast<std::uintptr_t>(host) + size;
    std::size_t same{}, changed{}, untouched{}, unknown{};
    std::scoped_lock lock{probe_mutex};
    if (restored_pages.empty()) return "no restore recorded";
    for (auto page = first; page < end; page += ProbePage) {
        const auto it = restored_pages.find(page);
        if (it == restored_pages.end()) {
            ++unknown;
        } else if (it->second == UntouchedHash) {
            ++untouched;
        } else if (it->second == HashPage(page)) {
            ++same;
        } else {
            ++changed;
        }
    }
    return fmt::format("pages {} as restored, {} changed since the restore, {} restored untouched, "
                       "{} not restored",
                       same, changed, untouched, unknown);
}
} // namespace Common::Pulse
