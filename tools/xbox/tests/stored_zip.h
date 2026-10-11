// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Archives for the mod harnesses: a stored (uncompressed) zip written by hand, so a test controls
// every name in it (hostile ones included), and a volume that reads it from memory.

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "eden_uwp/mods/mod_archive.h"

namespace TestArchives {

class MemorySource final : public EdenXbox::Mods::ArchiveSource {
public:
    explicit MemorySource(std::vector<std::uint8_t> bytes_) : bytes{std::move(bytes_)} {}
    std::uint64_t Size() const override { return bytes.size(); }
    std::size_t Read(std::uint64_t offset, std::uint8_t* out, std::size_t size) override {
        if (offset >= bytes.size()) return 0;
        const auto count = std::min<std::size_t>(size, static_cast<std::size_t>(bytes.size() - offset));
        std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(offset), count, out);
        return count;
    }

private:
    std::vector<std::uint8_t> bytes;
};

inline std::uint32_t Crc32(const std::string& data) {
    std::uint32_t crc = 0xFFFFFFFF;
    for (const unsigned char c : data) {
        crc ^= c;
        for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
    }
    return ~crc;
}

inline std::vector<std::uint8_t> StoredZip(const std::vector<std::pair<std::string, std::string>>& files) {
    std::vector<std::uint8_t> out, central;
    const auto put = [](std::vector<std::uint8_t>& v, std::uint32_t value, int bytes) {
        for (int i = 0; i < bytes; ++i) v.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
    };
    for (const auto& [name, data] : files) {
        const auto offset = static_cast<std::uint32_t>(out.size());
        const auto crc = Crc32(data);
        const auto size = static_cast<std::uint32_t>(data.size());
        const auto name_size = static_cast<std::uint32_t>(name.size());
        put(out, 0x04034b50, 4); put(out, 20, 2); put(out, 0, 2); put(out, 0, 2);
        put(out, 0, 4); put(out, crc, 4); put(out, size, 4); put(out, size, 4);
        put(out, name_size, 2); put(out, 0, 2);
        out.insert(out.end(), name.begin(), name.end());
        out.insert(out.end(), data.begin(), data.end());
        put(central, 0x02014b50, 4); put(central, 20, 2); put(central, 20, 2); put(central, 0, 2);
        put(central, 0, 2); put(central, 0, 4); put(central, crc, 4); put(central, size, 4);
        put(central, size, 4); put(central, name_size, 2); put(central, 0, 2); put(central, 0, 2);
        put(central, 0, 2); put(central, 0, 2); put(central, 0, 4); put(central, offset, 4);
        central.insert(central.end(), name.begin(), name.end());
    }
    const auto central_offset = static_cast<std::uint32_t>(out.size());
    const auto central_size = static_cast<std::uint32_t>(central.size());
    const auto count = static_cast<std::uint32_t>(files.size());
    out.insert(out.end(), central.begin(), central.end());
    put(out, 0x06054b50, 4); put(out, 0, 2); put(out, 0, 2); put(out, count, 2); put(out, count, 2);
    put(out, central_size, 4); put(out, central_offset, 4); put(out, 0, 2);
    return out;
}

inline std::vector<std::unique_ptr<EdenXbox::Mods::ArchiveSource>> Volumes(std::vector<std::uint8_t> bytes) {
    std::vector<std::unique_ptr<EdenXbox::Mods::ArchiveSource>> volumes;
    volumes.push_back(std::make_unique<MemorySource>(std::move(bytes)));
    return volumes;
}

} // namespace TestArchives
