// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/pulse_archive.h"

#include <algorithm>
#include <istream>
#include <ostream>
#include <utility>
#include <vector>

#include <lz4.h>

namespace Common::Pulse {
namespace {
constexpr std::array<std::uint8_t, 8> Magic{'P', 'U', 'L', 'S', 'E', 0, 1, 0};

void CheckStop(std::stop_token stop) {
    if (stop.stop_requested()) throw Failure{Error::Cancelled, "Pulse operation cancelled"};
}
void Put(std::ostream& out, std::span<const std::uint8_t> bytes) {
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!out) throw Failure{Error::Io, "Pulse write failed"};
}
void Get(std::istream& in, std::span<std::uint8_t> bytes) {
    in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!in) throw Failure{Error::Invalid, "Pulse archive truncated or unreadable"};
}
void Put64(std::ostream& out, std::uint64_t value) {
    std::array<std::uint8_t, 8> bytes{};
    for (unsigned i = 0; i < 8; ++i) bytes[i] = static_cast<std::uint8_t>(value >> (8 * i));
    Put(out, bytes);
}
std::uint64_t Get64(std::istream& in) {
    std::array<std::uint8_t, 8> bytes{};
    Get(in, bytes);
    std::uint64_t result{};
    for (unsigned i = 0; i < 8; ++i) result |= std::uint64_t{bytes[i]} << (8 * i);
    return result;
}
// CRC32 detects accidental corruption; archive identities use external strong hashes.
constexpr auto MakeCrcTable() {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t i = 0; i < table.size(); ++i) {
        auto value = i;
        for (unsigned bit = 0; bit < 8; ++bit)
            value = (value >> 1) ^ ((value & 1) ? 0xedb88320U : 0);
        table[i] = value;
    }
    return table;
}
constexpr auto CrcTable = MakeCrcTable();
std::uint32_t Crc(std::span<const std::uint8_t> bytes) {
    std::uint32_t result = ~0U;
    for (auto byte : bytes) result = CrcTable[(result ^ byte) & 255] ^ (result >> 8);
    return ~result;
}
// Encode manually: no compiler padding, endianness, pointers or native size_t.
std::array<std::uint8_t, 120> EncodeIdentity(const Identity& identity) {
    std::array<std::uint8_t, 120> bytes{};
    for (unsigned i = 0; i < 8; ++i) bytes[i] = static_cast<std::uint8_t>(identity.program_id >> (8 * i));
    auto pos = std::copy(identity.user.begin(), identity.user.end(), bytes.begin() + 8);
    pos = std::copy(identity.content.begin(), identity.content.end(), pos);
    pos = std::copy(identity.revision.begin(), identity.revision.end(), pos);
    std::copy(identity.savedata.begin(), identity.savedata.end(), pos);
    return bytes;
}
void PutSection(std::ostream& out, Section section) {
    Put64(out, std::uint64_t{section.id} | (std::uint64_t{section.version} << 32));
    Put64(out, section.size);
    std::array<std::uint8_t, 16> bytes{};
    const auto tag = std::uint64_t{section.id} | (std::uint64_t{section.version} << 32);
    for (unsigned i = 0; i < 8; ++i) {
        bytes[i] = static_cast<std::uint8_t>(tag >> (8 * i));
        bytes[i + 8] = static_cast<std::uint8_t>(section.size >> (8 * i));
    }
    Put64(out, Crc(bytes));
}
Section GetSection(std::istream& in) {
    const auto tag = Get64(in);
    const auto size = Get64(in);
    std::array<std::uint8_t, 16> bytes{};
    for (unsigned i = 0; i < 8; ++i) {
        bytes[i] = static_cast<std::uint8_t>(tag >> (8 * i));
        bytes[i + 8] = static_cast<std::uint8_t>(size >> (8 * i));
    }
    if (Get64(in) != Crc(bytes)) throw Failure{Error::Invalid, "Pulse section checksum mismatch"};
    return {static_cast<std::uint32_t>(tag), static_cast<std::uint32_t>(tag >> 32), size};
}
} // namespace

Writer::Writer(std::ostream& stream_, const Identity& identity, std::stop_token stop_, Progress progress_)
    : stream{stream_}, stop{stop_}, progress{std::move(progress_)} {
    CheckStop(stop);
    Put(stream, Magic);
    const auto bytes = EncodeIdentity(identity);
    Put(stream, bytes);
    Put64(stream, Crc(bytes));
}

void Writer::Add(Section section, const ReadBlock& read) {
    if (finished || failed || section.id <= last_id || !section.version || !read || sections == 4096)
        throw Failure{Error::Invalid, "Pulse invalid section or writer state"};
    if (section.size > UINT64_MAX - total) throw Failure{Error::Limit, "Pulse size overflow"};
    // A callback/stream/allocation failure permanently poisons the writer.
    failed = true;
    CheckStop(stop);
    PutSection(stream, section);
    std::vector<std::uint8_t> raw(BlockSize);
    std::vector<char> compressed(static_cast<std::size_t>(LZ4_compressBound(BlockSize)));
    for (std::uint64_t offset = 0; offset < section.size;) {
        CheckStop(stop);
        const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(BlockSize, section.size - offset));
        auto block = std::span{raw}.first(count);
        read(offset, block);
        CheckStop(stop);
        const bool zero = std::all_of(block.begin(), block.end(), [](auto byte) { return byte == 0; });
        const int packed = zero ? 0 : LZ4_compress_default(reinterpret_cast<const char*>(raw.data()),
            compressed.data(), static_cast<int>(count), static_cast<int>(compressed.size()));
        const bool use_lz4 = packed > 0 && static_cast<std::size_t>(packed) < count;
        const std::uint64_t stored = zero ? 0 : use_lz4 ? static_cast<std::uint64_t>(packed) : count;
        Put64(stream, count);
        Put64(stream, stored);
        Put64(stream, zero ? 0 : use_lz4 ? 2 : 1);
        Put64(stream, Crc(block));
        if (use_lz4) Put(stream, {reinterpret_cast<const std::uint8_t*>(compressed.data()), static_cast<std::size_t>(packed)});
        else if (!zero) Put(stream, block);
        offset += count;
        total += count;
        if (progress) progress(total);
    }
    last_id = section.id;
    ++sections;
    failed = false;
}

void Writer::Finish() {
    if (finished || failed) throw Failure{Error::Invalid, "Pulse writer cannot finish"};
    failed = true;
    CheckStop(stop);
    PutSection(stream, {});
    Put64(stream, total);
    stream.flush();
    if (!stream) throw Failure{Error::Io, "Pulse flush failed"};
    finished = true;
}

Identity ReadHeader(std::istream& stream) {
    std::array<std::uint8_t, 8> magic{};
    Get(stream, magic);
    if (magic != Magic) throw Failure{Error::Incompatible, "Pulse archive format unsupported"};
    std::array<std::uint8_t, 120> identity{};
    Get(stream, identity);
    if (Get64(stream) != Crc(identity)) throw Failure{Error::Invalid, "Pulse identity checksum mismatch"};
    Identity result{};
    for (unsigned i = 0; i < 8; ++i) result.program_id |= std::uint64_t{identity[i]} << (8 * i);
    std::copy_n(identity.begin() + 8, result.user.size(), result.user.begin());
    std::copy_n(identity.begin() + 24, result.content.size(), result.content.begin());
    std::copy_n(identity.begin() + 56, result.revision.size(), result.revision.begin());
    std::copy_n(identity.begin() + 88, result.savedata.size(), result.savedata.begin());
    return result;
}

void Read(std::istream& stream, const Identity& expected, std::uint64_t max_decoded,
          const WriteBlock& write, std::stop_token stop, Progress progress) {
    CheckStop(stop);
    if (ReadHeader(stream) != expected) throw Failure{Error::Incompatible, "Pulse identity mismatch"};
    std::vector<std::uint8_t> raw(BlockSize), packed(BlockSize);
    std::uint32_t last_id{};
    std::uint32_t sections{};
    std::uint64_t total{};
    for (;;) {
        CheckStop(stop);
        const auto section = GetSection(stream);
        if (!section.id) {
            if (section.version || section.size || Get64(stream) != total || stream.peek() != std::char_traits<char>::eof() || stream.bad())
                throw Failure{Error::Invalid, "Pulse invalid footer or trailing data"};
            return;
        }
        if (section.id <= last_id || !section.version || ++sections > 4096)
            throw Failure{Error::Invalid, "Pulse invalid section order/count/version"};
        if (section.size > max_decoded - total) throw Failure{Error::Limit, "Pulse decoded size exceeds limit"};
        last_id = section.id;
        // Notify even for empty sections; the subsystem must validate its own version.
        if (write) write(section, 0, {});
        for (std::uint64_t offset = 0; offset < section.size;) {
            CheckStop(stop);
            const auto count = Get64(stream);
            const auto stored = Get64(stream);
            const auto codec = Get64(stream);
            const auto checksum = Get64(stream);
            if (count != std::min<std::uint64_t>(BlockSize, section.size - offset) || stored > BlockSize ||
                codec > 2 || (codec == 0 && stored != 0) || (codec == 1 && stored != count) ||
                (codec == 2 && (stored == 0 || stored >= count)))
                throw Failure{Error::Invalid, "Pulse invalid block lengths/codec"};
            auto block = std::span{raw}.first(static_cast<std::size_t>(count));
            if (codec == 0) std::fill(block.begin(), block.end(), std::uint8_t{0});
            else if (codec == 1) Get(stream, block);
            else {
                Get(stream, std::span{packed}.first(static_cast<std::size_t>(stored)));
                if (LZ4_decompress_safe(reinterpret_cast<const char*>(packed.data()), reinterpret_cast<char*>(raw.data()),
                    static_cast<int>(stored), static_cast<int>(count)) != static_cast<int>(count))
                    throw Failure{Error::Invalid, "Pulse invalid LZ4 payload"};
            }
            if (checksum != Crc(block)) throw Failure{Error::Invalid, "Pulse block checksum mismatch"};
            CheckStop(stop);
            if (write) write(section, offset, block);
            offset += count;
            total += count;
            if (progress) progress(total);
        }
    }
}
} // namespace Common::Pulse
