// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/pulse_archive.h"

namespace Common::Pulse {
[[nodiscard]] std::string StorageKey(const Identity& identity);

// Diagnostic: the hash of each host page a restore wrote, so a later reader of bad guest memory
// can tell whether the session held it or something overwrote it after the resume.
void ClearRestoredPages();
void NoteRestoredPage(const void* host_page, bool untouched);
[[nodiscard]] std::string DescribeRestoredPages(const void* host, std::size_t size);
// Fault points are used by the storage regression gate. No callbacks at runtime
// unless explicitly supplied. A failed operation never deletes a published state.
enum class PublishStep { Write, Verify, Sync, Replace, Rebuild, Consume, Consumed };
using Fault = std::function<void(PublishStep)>;

enum class EntryState { Available, Consumed, Corrupt, UnknownFormat };
struct Entry {
    std::string key;
    Identity identity{};
    std::uint64_t bytes{};
    std::filesystem::file_time_type modified{};
    EntryState state{};
    // Catalogue checks ONLY the fixed header. Full validation precedes restore.
    bool header_valid{};
};

class Store final {
public:
    // root must be a dedicated, trusted LocalState subdirectory. One Store owns
    // it for the lifetime of the process; all access is serialized by this object.
    explicit Store(std::filesystem::path root_, std::uint64_t quota_ = DefaultQuota,
                   Fault fault_ = {})
        : root{std::move(root_)}, quota{quota_}, fault{std::move(fault_)} {}

    // key: 48 lowercase hex digits (program ID + user ID), never a supplied path.
    // max_encoded reserves old + temporary space and bounds EVERY byte written.
    void Publish(std::string_view key, const Identity& identity, std::uint64_t max_encoded,
                 std::uint64_t max_decoded, const std::function<void(Writer&)>& serialize,
                 std::stop_token stop = {}, Progress progress = {});
    void Verify(std::string_view key, const Identity& identity, std::uint64_t max_decoded);
    // No callback may execute guest code. Rebuild a fresh stopped system, then
    // atomically consume before the caller can Run(). Any exception means discard
    // the candidate system; do not run it. Callbacks must not reenter this Store.
    void RestoreAndConsume(std::string_view key, const Identity& identity,
                           std::uint64_t max_decoded, const WriteBlock& rebuild,
                           std::stop_token stop = {}, Progress progress = {});
    [[nodiscard]] std::vector<Entry> List();
    // Intended only for an explicit user discard. Does not touch normal saves.
    void Discard(std::string_view key);

private:
    std::filesystem::path Path(std::string_view key, const char* extension) const;
    std::uint64_t Used() const;
    const std::filesystem::path root;
    const std::uint64_t quota;
    Fault fault;
    std::mutex mutex;
};
} // namespace Common::Pulse
