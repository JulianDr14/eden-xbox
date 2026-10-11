// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <iosfwd>
#include <span>
#include <stdexcept>
#include <stop_token>

namespace Common::Pulse {

inline constexpr std::uint32_t BlockSize = 1U << 20;
inline constexpr std::uint64_t DefaultQuota = 20ULL << 30;
inline constexpr std::uint64_t FreeReserve = 1ULL << 30;
inline constexpr std::uint64_t TransferBudget = 32ULL << 20;

// Caller computes cryptographic fingerprints over canonical effective content/settings,
// emulator revision and savedata. No paths or host pointers are archive identities.
struct Identity {
    std::uint64_t program_id{};
    std::array<std::uint8_t, 16> user{};
    std::array<std::uint8_t, 32> content{};
    std::array<std::uint8_t, 32> revision{};
    std::array<std::uint8_t, 32> savedata{};
    bool operator==(const Identity&) const = default;
};

enum class Error { Io, Invalid, Incompatible, Limit, Cancelled };
class Failure final : public std::runtime_error {
public:
    Failure(Error code_, const char* message) : std::runtime_error{message}, code{code_} {}
    const Error code;
};

struct Section {
    std::uint32_t id{}; // Nonzero, strictly increasing; stable subsystem ID.
    std::uint32_t version{};
    std::uint64_t size{};
};

// Called on the operation worker, never from guest hot paths. Throwing aborts the
// operation. Stream errors leave an unusable temporary; publication is separate.
using Progress = std::function<void(std::uint64_t transferred)>;
using ReadBlock = std::function<void(std::uint64_t offset, std::span<std::uint8_t>)>;
using WriteBlock = std::function<void(Section, std::uint64_t offset,
                                    std::span<const std::uint8_t>)>;

// Reads only the fixed 136-byte header; does not certify the section payloads.
// Used for the library catalogue without decompressing guest memory.
[[nodiscard]] Identity ReadHeader(std::istream& stream);

class Writer final {
public:
    Writer(std::ostream& stream, const Identity& identity, std::stop_token stop = {},
           Progress progress = {});
    void Add(Section section, const ReadBlock& read);
    void Finish();
private:
    std::ostream& stream;
    std::stop_token stop;
    Progress progress;
    std::uint32_t last_id{};
    std::uint32_t sections{};
    std::uint64_t total{};
    bool finished{};
    bool failed{};
};

// Scan first with no consumer to validate every checksum/limit before rebuilding.
// Use a fresh, stopped system for the second pass; never load into a live guest.
// max_decoded is a trusted caller limit, not a value taken from the file.
void Read(std::istream& stream, const Identity& expected, std::uint64_t max_decoded,
          const WriteBlock& write = {}, std::stop_token stop = {}, Progress progress = {});

// Includes old generations and temporary files in used. Overflow-safe admission;
// caller serializes admission/writes and rechecks actual available disk space.
[[nodiscard]] constexpr bool Fits(std::uint64_t used, std::uint64_t incoming,
                                  std::uint64_t quota, std::uint64_t available) {
    return used <= quota && incoming <= quota - used && available >= FreeReserve &&
           incoming <= available - FreeReserve;
}

} // namespace Common::Pulse
