// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/pulse_archive.h"

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <istream>
#include <mutex>
#include <ostream>
#include <thread>
#include <utility>
#include <vector>

#include <lz4.h>

#if defined(_M_X64) || defined(__x86_64__)
#define PULSE_CRC_CLMUL 1
#include <immintrin.h>
#ifdef _MSC_VER
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#if defined(__GNUC__) || defined(__clang__)
#define PULSE_CRC_TARGET __attribute__((target("pclmul,sse4.1")))
#else
#define PULSE_CRC_TARGET
#endif
#endif

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
// Slicing-by-8: eight bytes per step, the same CRC as the byte-wise table. It covers what the
// carry-less multiply below does not (short tails, CPUs without PCLMULQDQ).
constexpr auto MakeCrcTables() {
    std::array<std::array<std::uint32_t, 256>, 8> tables{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        auto value = i;
        for (unsigned bit = 0; bit < 8; ++bit)
            value = (value >> 1) ^ ((value & 1) ? 0xedb88320U : 0);
        tables[0][i] = value;
    }
    for (std::uint32_t i = 0; i < 256; ++i)
        for (unsigned slice = 1; slice < 8; ++slice)
            tables[slice][i] = (tables[slice - 1][i] >> 8) ^ tables[0][tables[slice - 1][i] & 255];
    return tables;
}
constexpr auto CrcTables = MakeCrcTables();
std::uint32_t CrcSlices(std::uint32_t state, const std::uint8_t* data, std::size_t size) {
    const auto& t = CrcTables;
    for (; size >= 8; data += 8, size -= 8) {
        std::uint32_t low, high;
        std::memcpy(&low, data, 4);
        std::memcpy(&high, data + 4, 4);
        low ^= state;
        state = t[7][low & 255] ^ t[6][(low >> 8) & 255] ^ t[5][(low >> 16) & 255] ^
                t[4][low >> 24] ^ t[3][high & 255] ^ t[2][(high >> 8) & 255] ^
                t[1][(high >> 16) & 255] ^ t[0][high >> 24];
    }
    for (; size; ++data, --size) state = t[0][(state ^ *data) & 255] ^ (state >> 8);
    return state;
}

#ifdef PULSE_CRC_CLMUL
bool HasClmul() {
    static const bool supported = [] {
        unsigned ecx{};
#ifdef _MSC_VER
        int regs[4]{};
        __cpuid(regs, 1);
        ecx = static_cast<unsigned>(regs[2]);
#else
        unsigned eax{}, ebx{}, edx{};
        if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx)) return false;
#endif
        constexpr unsigned Pclmulqdq = 1U << 1, Sse41 = 1U << 19;
        return (ecx & (Pclmulqdq | Sse41)) == (Pclmulqdq | Sse41);
    }();
    return supported;
}

// Folds 64 bytes per step with carry-less multiplies: Intel's PCLMULQDQ CRC-32 for the reflected
// 0xEDB88320 polynomial, as zlib-ng and Chromium's zlib use it (several times the table's speed).
// `state` and the result are the running, inverted CRC; `size` is at least 64, a multiple of 16.
PULSE_CRC_TARGET std::uint32_t CrcFold(const std::uint8_t* data, std::size_t size,
                                       std::uint32_t state) {
    alignas(16) static constexpr std::uint64_t k1k2[] = {0x0154442bd4, 0x01c6e41596};
    alignas(16) static constexpr std::uint64_t k3k4[] = {0x01751997d0, 0x00ccaa009e};
    alignas(16) static constexpr std::uint64_t k5k0[] = {0x0163cd6124, 0x0000000000};
    alignas(16) static constexpr std::uint64_t poly[] = {0x01db710641, 0x01f7011641};
    const auto load = [](const std::uint8_t* at) {
        return _mm_loadu_si128(reinterpret_cast<const __m128i*>(at));
    };
    __m128i x1 = load(data), x2 = load(data + 16), x3 = load(data + 32), x4 = load(data + 48);
    x1 = _mm_xor_si128(x1, _mm_cvtsi32_si128(static_cast<int>(state)));
    __m128i x0 = _mm_load_si128(reinterpret_cast<const __m128i*>(k1k2));
    data += 64;
    size -= 64;
    for (; size >= 64; data += 64, size -= 64) {
        const __m128i x5 = _mm_clmulepi64_si128(x1, x0, 0x00);
        const __m128i x6 = _mm_clmulepi64_si128(x2, x0, 0x00);
        const __m128i x7 = _mm_clmulepi64_si128(x3, x0, 0x00);
        const __m128i x8 = _mm_clmulepi64_si128(x4, x0, 0x00);
        x1 = _mm_xor_si128(_mm_xor_si128(_mm_clmulepi64_si128(x1, x0, 0x11), x5), load(data));
        x2 = _mm_xor_si128(_mm_xor_si128(_mm_clmulepi64_si128(x2, x0, 0x11), x6), load(data + 16));
        x3 = _mm_xor_si128(_mm_xor_si128(_mm_clmulepi64_si128(x3, x0, 0x11), x7), load(data + 32));
        x4 = _mm_xor_si128(_mm_xor_si128(_mm_clmulepi64_si128(x4, x0, 0x11), x8), load(data + 48));
    }
    x0 = _mm_load_si128(reinterpret_cast<const __m128i*>(k3k4));
    const auto fold = [&](__m128i value, __m128i next) {
        const __m128i low = _mm_clmulepi64_si128(value, x0, 0x00);
        return _mm_xor_si128(_mm_xor_si128(_mm_clmulepi64_si128(value, x0, 0x11), next), low);
    };
    x1 = fold(x1, x2);
    x1 = fold(x1, x3);
    x1 = fold(x1, x4);
    for (; size >= 16; data += 16, size -= 16) x1 = fold(x1, load(data));
    // 128 bits down to 64, then a Barrett reduction to 32.
    x2 = _mm_clmulepi64_si128(x1, x0, 0x10);
    x3 = _mm_setr_epi32(~0, 0, ~0, 0);
    x1 = _mm_xor_si128(_mm_srli_si128(x1, 8), x2);
    x0 = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(k5k0));
    x2 = _mm_srli_si128(x1, 4);
    x1 = _mm_xor_si128(_mm_clmulepi64_si128(_mm_and_si128(x1, x3), x0, 0x00), x2);
    x0 = _mm_load_si128(reinterpret_cast<const __m128i*>(poly));
    x2 = _mm_and_si128(_mm_clmulepi64_si128(_mm_and_si128(x1, x3), x0, 0x10), x3);
    x2 = _mm_clmulepi64_si128(x2, x0, 0x00);
    return static_cast<std::uint32_t>(_mm_extract_epi32(_mm_xor_si128(x1, x2), 1));
}
#endif

std::uint32_t Crc(std::span<const std::uint8_t> bytes) {
    std::uint32_t state = ~0U;
    const std::uint8_t* data = bytes.data();
    std::size_t size = bytes.size();
#ifdef PULSE_CRC_CLMUL
    if (size >= 64 && HasClmul()) {
        const std::size_t bulk = size & ~std::size_t{15};
        state = CrcFold(data, bulk, state);
        data += bulk;
        size -= bulk;
    }
#endif
    return ~CrcSlices(state, data, size);
}
// Untouched guest memory makes most of a session zero blocks, all of one size.
std::uint32_t ZeroCrc(std::size_t count) {
    static const std::uint32_t full = Crc(std::vector<std::uint8_t>(BlockSize));
    return count == BlockSize ? full : Crc(std::vector<std::uint8_t>(count));
}
bool IsZero(std::span<const std::uint8_t> bytes) {
    return bytes.empty() || (bytes[0] == 0 && std::memcmp(bytes.data(), bytes.data() + 1, bytes.size() - 1) == 0);
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
// Blocks are independent, so they are checksummed and (de)compressed on several threads while
// the stream is read or written in order on the caller's. Each lane holds one block in flight, raw
// and packed: about 2 MiB, so a save stays within its memory budget.
constexpr unsigned MaxLanes = 4;

// Runs job(0..count-1) on the caller and its helper threads, and waits for all of them. The first
// exception a job throws is rethrown on the caller once the round is over.
class Lanes {
public:
    explicit Lanes(unsigned wanted) {
        const unsigned cores = std::max(1U, std::thread::hardware_concurrency());
        const unsigned lanes = std::clamp(wanted, 1U, std::min(cores, MaxLanes));
        for (unsigned i = 1; i < lanes; ++i) threads.emplace_back([this] { Loop(); });
    }
    ~Lanes() {
        {
            std::scoped_lock lock{mutex};
            quit = true;
        }
        wake.notify_all();
        for (auto& thread : threads) thread.join();
    }
    Lanes(const Lanes&) = delete;
    Lanes& operator=(const Lanes&) = delete;

    [[nodiscard]] unsigned Size() const { return static_cast<unsigned>(threads.size()) + 1; }

    void Run(unsigned count, const std::function<void(unsigned)>& job) {
        {
            std::scoped_lock lock{mutex};
            current = &job;
            jobs = count;
            next = 0;
            pending = count;
            error = nullptr;
            ++round;
        }
        wake.notify_all();
        Work();
        std::unique_lock lock{mutex};
        done.wait(lock, [this] { return pending == 0; });
        current = nullptr;
        if (error) std::rethrow_exception(std::exchange(error, nullptr));
    }

private:
    void Loop() {
        std::uint64_t seen{};
        for (;;) {
            {
                std::unique_lock lock{mutex};
                wake.wait(lock, [&] { return quit || round != seen; });
                if (quit) return;
                seen = round;
            }
            Work();
        }
    }
    void Work() {
        for (;;) {
            unsigned index;
            const std::function<void(unsigned)>* job;
            {
                std::scoped_lock lock{mutex};
                if (!current || next >= jobs) return;
                index = next++;
                job = current;
            }
            std::exception_ptr failure;
            try {
                (*job)(index);
            } catch (...) {
                failure = std::current_exception();
            }
            std::scoped_lock lock{mutex};
            if (failure && !error) error = failure;
            if (--pending == 0) done.notify_all();
        }
    }

    std::vector<std::thread> threads;
    std::mutex mutex;
    std::condition_variable wake, done;
    const std::function<void(unsigned)>* current{};
    unsigned jobs{}, next{}, pending{};
    std::uint64_t round{};
    std::exception_ptr error;
    bool quit{};
};

std::uint64_t BlockCount(std::uint64_t size) {
    return (size + BlockSize - 1) / BlockSize;
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
    struct Encoded {
        std::vector<std::uint8_t> raw;
        std::vector<char> packed;
        std::size_t count{};
        std::uint64_t stored{}, codec{};
        std::uint32_t crc{};
    };
    const auto blocks = BlockCount(section.size);
    Lanes lanes{static_cast<unsigned>(std::min<std::uint64_t>(blocks, MaxLanes))};
    const auto capacity = static_cast<std::size_t>(std::min<std::uint64_t>(BlockSize, section.size));
    std::vector<Encoded> slots(static_cast<std::size_t>(std::min<std::uint64_t>(blocks, lanes.Size())));
    for (auto& slot : slots) {
        slot.raw.resize(capacity);
        slot.packed.resize(static_cast<std::size_t>(LZ4_compressBound(static_cast<int>(capacity))));
    }
    for (std::uint64_t offset = 0; offset < section.size;) {
        CheckStop(stop);
        const auto batch = static_cast<unsigned>(
            std::min<std::uint64_t>(slots.size(), BlockCount(section.size - offset)));
        // read() may run on several threads at once, each on its own block.
        lanes.Run(batch, [&](unsigned index) {
            auto& slot = slots[index];
            const std::uint64_t at = offset + std::uint64_t{index} * BlockSize;
            slot.count = static_cast<std::size_t>(std::min<std::uint64_t>(BlockSize, section.size - at));
            const auto block = std::span{slot.raw}.first(slot.count);
            read(at, block);
            if (IsZero(block)) {
                slot.stored = slot.codec = 0;
                slot.crc = ZeroCrc(slot.count);
                return;
            }
            slot.crc = Crc(block);
            const int packed = LZ4_compress_default(reinterpret_cast<const char*>(block.data()),
                slot.packed.data(), static_cast<int>(slot.count), static_cast<int>(slot.packed.size()));
            const bool use_lz4 = packed > 0 && static_cast<std::size_t>(packed) < slot.count;
            slot.stored = use_lz4 ? static_cast<std::uint64_t>(packed) : slot.count;
            slot.codec = use_lz4 ? 2 : 1;
        });
        CheckStop(stop);
        for (unsigned index = 0; index < batch; ++index) {
            const auto& slot = slots[index];
            Put64(stream, slot.count);
            Put64(stream, slot.stored);
            Put64(stream, slot.codec);
            Put64(stream, slot.crc);
            if (slot.codec == 2) Put(stream, {reinterpret_cast<const std::uint8_t*>(slot.packed.data()), static_cast<std::size_t>(slot.stored)});
            else if (slot.codec == 1) Put(stream, std::span{slot.raw}.first(slot.count));
            offset += slot.count;
            total += slot.count;
            if (progress) progress(total);
        }
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
    struct Stored {
        std::vector<std::uint8_t> raw, packed;
        std::uint64_t count{}, stored{}, codec{}, checksum{};
    };
    Lanes lanes{MaxLanes};
    std::vector<Stored> slots(lanes.Size());
    for (auto& slot : slots) {
        slot.raw.resize(BlockSize);
        slot.packed.resize(BlockSize);
    }
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
            // The stream is read in order here; the blocks it holds are decoded on the lanes.
            unsigned batch{};
            for (std::uint64_t at = offset; batch < slots.size() && at < section.size; ++batch) {
                auto& slot = slots[batch];
                slot.count = Get64(stream);
                slot.stored = Get64(stream);
                slot.codec = Get64(stream);
                slot.checksum = Get64(stream);
                const auto count = slot.count, stored = slot.stored, codec = slot.codec;
                if (count != std::min<std::uint64_t>(BlockSize, section.size - at) || stored > BlockSize ||
                    codec > 2 || (codec == 0 && stored != 0) || (codec == 1 && stored != count) ||
                    (codec == 2 && (stored == 0 || stored >= count)))
                    throw Failure{Error::Invalid, "Pulse invalid block lengths/codec"};
                if (codec == 1) Get(stream, std::span{slot.raw}.first(static_cast<std::size_t>(count)));
                else if (codec == 2) Get(stream, std::span{slot.packed}.first(static_cast<std::size_t>(stored)));
                at += count;
            }
            lanes.Run(batch, [&](unsigned index) {
                auto& slot = slots[index];
                const auto block = std::span{slot.raw}.first(static_cast<std::size_t>(slot.count));
                if (slot.codec == 0) {
                    std::fill(block.begin(), block.end(), std::uint8_t{0});
                    if (slot.checksum != ZeroCrc(block.size())) throw Failure{Error::Invalid, "Pulse block checksum mismatch"};
                    return;
                }
                if (slot.codec == 2 && LZ4_decompress_safe(reinterpret_cast<const char*>(slot.packed.data()),
                        reinterpret_cast<char*>(slot.raw.data()), static_cast<int>(slot.stored),
                        static_cast<int>(slot.count)) != static_cast<int>(slot.count))
                    throw Failure{Error::Invalid, "Pulse invalid LZ4 payload"};
                if (slot.checksum != Crc(block)) throw Failure{Error::Invalid, "Pulse block checksum mismatch"};
            });
            CheckStop(stop);
            for (unsigned index = 0; index < batch; ++index) {
                const auto block = std::span{slots[index].raw}.first(static_cast<std::size_t>(slots[index].count));
                if (write) write(section, offset, block);
                offset += block.size();
                total += block.size();
                if (progress) progress(total);
            }
        }
    }
}
} // namespace Common::Pulse
