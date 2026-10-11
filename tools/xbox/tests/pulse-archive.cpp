// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
// Link src/common/pulse_archive.cpp and the configured LZ4 library.
#include "common/pulse_archive.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <random>
#include <sstream>
#include <vector>

using namespace Common::Pulse;
void Check(bool ok) { if (!ok) std::abort(); }
template <typename F> void Reject(F run, Error expected) {
    try { run(); } catch (const Failure& failure) { Check(failure.code == expected); return; }
    std::abort();
}

int main() {
    Identity identity{0x123456};
    identity.user[0] = 3;
    identity.content[0] = 8;
    identity.revision[0] = 7;
    identity.savedata[0] = 5;
    std::vector<std::uint8_t> data(3 * BlockSize + 13);
    std::mt19937 random{42};
    // Zero, compressible, incompressible and partial blocks.
    std::fill(data.begin() + BlockSize, data.begin() + 2 * BlockSize, std::uint8_t{0x55});
    for (auto i = 2 * BlockSize; i < data.size(); ++i) data[i] = static_cast<std::uint8_t>(random());
    std::ostringstream output{std::ios::binary};
    Writer writer{output, identity};
    writer.Add({1, 1, 0}, [](auto, auto) {});
    writer.Add({2, 1, data.size()}, [&](auto offset, auto block) {
        std::copy_n(data.data() + offset, block.size(), block.data());
    });
    writer.Finish();
    const auto archive = output.str();
    {
        std::istringstream header_only{archive.substr(0, 136), std::ios::binary};
        Check(ReadHeader(header_only) == identity);
    }
    for (unsigned cycle = 0; cycle < 100; ++cycle) {
        std::istringstream input{archive, std::ios::binary};
        std::vector<std::uint8_t> restored(data.size());
        unsigned declarations{};
        Read(input, identity, data.size(), [&](Section section, auto offset, auto block) {
            if (block.empty()) { ++declarations; return; }
            Check(section.id == 2);
            std::copy(block.begin(), block.end(), restored.begin() + offset);
        });
        Check(restored == data && declarations == 2);
    }
    auto decode = [&](const std::string& bytes, const Identity& id, std::uint64_t limit) {
        std::istringstream in{bytes, std::ios::binary};
        Read(in, id, limit);
    };
    auto other = identity;
    other.user[0] ^= 1;
    Reject([&] { decode(archive, other, data.size()); }, Error::Incompatible);
    Reject([&] { decode(archive, identity, data.size() - 1); }, Error::Limit);
    Reject([&] { decode(archive + "x", identity, data.size()); }, Error::Invalid);
    const auto duplicate = archive.substr(0, 160) + archive.substr(136, 24) + archive.substr(160);
    Reject([&] { decode(duplicate, identity, data.size()); }, Error::Invalid);
    auto unsupported = archive;
    unsupported[6] = 2;
    Reject([&] { decode(unsupported, identity, data.size()); }, Error::Incompatible);
    for (auto length : {0U, 7U, 127U, 135U, 150U, 170U})
        Reject([&] { decode(archive.substr(0, length), identity, data.size()); }, Error::Invalid);
    Reject([&] { decode(archive.substr(0, archive.size() - 1), identity, data.size()); }, Error::Invalid);
    for (auto index : {8U, 120U, 136U, 144U, 160U, 184U, 192U, 200U, 208U}) {
        auto damaged = archive;
        damaged[index] ^= 0x40;
        Reject([&] { decode(damaged, identity, data.size()); }, Error::Invalid);
    }
    for (const auto index : {std::size_t{248}, archive.size() - 33}) {
        auto damaged = archive;
        damaged[index] ^= 0x40;
        Reject([&] { decode(damaged, identity, data.size()); }, Error::Invalid);
    }
    // Cancellation before IO, during a section and poisoned writer protection.
    std::stop_source stop;
    stop.request_stop();
    Reject([&] { std::istringstream in{archive}; Read(in, identity, data.size(), {}, stop.get_token()); }, Error::Cancelled);
    std::ostringstream cancelled;
    std::stop_source mid;
    Writer aborted{cancelled, identity, mid.get_token()};
    Reject([&] { aborted.Add({1, 1, data.size()}, [&](auto, auto) { mid.request_stop(); }); }, Error::Cancelled);
    Reject([&] { aborted.Finish(); }, Error::Invalid);
    std::ostringstream broken;
    Writer bad{broken, identity};
    broken.setstate(std::ios::badbit);
    Reject([&] { bad.Add({1, 1, 1}, [](auto, auto) {}); }, Error::Io);
    Reject([&] { bad.Finish(); }, Error::Invalid);
    std::ostringstream many;
    Writer limited{many, identity};
    for (std::uint32_t id = 1; id <= 4096; ++id) limited.Add({id, 1, 0}, [](auto, auto) {});
    Reject([&] { limited.Add({4097, 1, 0}, [](auto, auto) {}); }, Error::Invalid);
    limited.Finish();
    decode(many.str(), identity, 0);
    std::ostringstream wrong;
    Writer invalid{wrong, identity};
    Reject([&] { invalid.Add({0, 1, 0}, [](auto, auto) {}); }, Error::Invalid);
    Reject([&] { invalid.Add({1, 0, 0}, [](auto, auto) {}); }, Error::Invalid);
    Check(Fits(0, 1, DefaultQuota, FreeReserve + 1));
    Check(!Fits(UINT64_MAX, 1, DefaultQuota, UINT64_MAX));
    Check(!Fits(DefaultQuota, 1, DefaultQuota, UINT64_MAX));
    Check(!Fits(0, 1, DefaultQuota, FreeReserve));
    Check(!Fits(0, UINT64_MAX, UINT64_MAX, UINT64_MAX));
    std::cout << "Pulse archive: 100 codec round trips, malformed/truncated/identity/limit/cancellation/IO/quota cases passed\n";
}
