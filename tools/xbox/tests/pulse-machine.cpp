// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#include <iostream>
#include <random>
#include <sstream>
#include "core/pulse_machine_state.h"

namespace {
void Require(bool value) {
    if (!value) throw std::runtime_error("Pulse machine assertion");
}
template <class F> void Reject(F operation) {
    bool rejected{};
    try { operation(); } catch (const Common::Pulse::Failure&) { rejected = true; }
    Require(rejected);
}
}

int main() {
    try {
        std::mt19937_64 random{0x50554c5345};
        for (size_t cycle = 0; cycle < 100; ++cycle) {
            Kernel::Svc::ThreadContext source{};
            for (auto& value : source.r) value = random();
            source.fp = random(); source.lr = random(); source.sp = random();
            source.pc = random(); source.pstate = static_cast<u32>(random());
            source.fpcr = static_cast<u32>(random()); source.fpsr = static_cast<u32>(random());
            source.tpidr = random(); source.padding = 0xdeadbeef;
            for (auto& vector : source.v) vector = {random(), random()};
            const auto encoded = Core::Pulse::EncodeContext(source);
            const auto restored = Core::Pulse::DecodeContext(encoded);
            Require(restored.padding == 0);
            Require(Core::Pulse::EncodeContext(restored) == encoded);
            // Verify independently chosen offsets rather than only inverse roundtrips.
            Require(encoded[0] == static_cast<u8>(source.r[0]));
            Require(encoded[7] == static_cast<u8>(source.r[0] >> 56));
            Require(encoded[264] == static_cast<u8>(source.pstate));
            Require(encoded[788] == static_cast<u8>(source.tpidr));
            Reject([&] { Core::Pulse::DecodeContext(std::span{encoded}.first(795)); });

            constexpr size_t backing_size = 2 * Common::Pulse::BlockSize;
            std::vector<u8> backing(backing_size, 0xa5);
            // Deliberately unsorted and sparse; a transfer crosses physical holes.
            const std::array<Core::Pulse::PhysicalExtent, 3> allocations{{
                {1024 * 1024, 512 * 1024}, {4096, 8192}, {32768, 512 * 1024}}};
            const Core::Pulse::PhysicalTransfer transfer{allocations, backing_size};
            std::vector<u8> expected;
            for (auto extent : transfer.Extents()) {
                for (u64 i = 0; i < extent.size; ++i) {
                    const auto value = static_cast<u8>(random());
                    backing[extent.offset + i] = value;
                    expected.push_back(value);
                }
            }
            std::vector<u8> encoded_memory(transfer.Size());
            for (u64 offset = 0; offset < transfer.Size();) {
                const auto count = static_cast<size_t>(std::min<u64>(
                    131071, transfer.Size() - offset));
                transfer.Read(backing, offset, std::span{encoded_memory}.subspan(offset, count));
                offset += count;
            }
            Require(encoded_memory == expected);
            std::stringstream archive{std::ios::in | std::ios::out | std::ios::binary};
            const Common::Pulse::Identity identity{};
            Common::Pulse::Writer writer{archive, identity};
            writer.Add({1, 1, encoded.size()}, [&](u64 offset, std::span<u8> bytes) {
                std::copy_n(encoded.begin() + offset, bytes.size(), bytes.begin());
            });
            writer.Add({2, 1, transfer.Size()}, [&](u64 offset, std::span<u8> bytes) {
                transfer.Read(backing, offset, bytes);
            });
            writer.Finish();
            backing.clear(); backing.shrink_to_fit();
            std::vector<u8> fresh(backing_size, 0xa5);
            archive.seekg(0);
            Common::Pulse::Read(archive, identity, transfer.Size() + encoded.size());
            archive.seekg(0);
            size_t restored_sections{};
            Common::Pulse::Read(archive, identity, transfer.Size() + encoded.size(),
                [&](Common::Pulse::Section section, u64 offset, std::span<const u8> bytes) {
                    Require(section.version == 1);
                    if (bytes.empty()) {
                        Require((section.id == 1 && section.size == encoded.size()) ||
                                (section.id == 2 && section.size == transfer.Size()));
                        ++restored_sections;
                        return;
                    }
                    if (section.id == 1) {
                        Require(section.size == encoded.size() && offset == 0);
                        Require(Core::Pulse::EncodeContext(Core::Pulse::DecodeContext(bytes)) == encoded);
                    } else {
                        Require(section.id == 2 && section.size == transfer.Size());
                        transfer.Write(fresh, offset, bytes);
                    }
                });
            Require(restored_sections == 2);
            size_t position{};
            size_t previous{};
            for (auto extent : transfer.Extents()) {
                Require(std::all_of(fresh.begin() + previous, fresh.begin() + extent.offset,
                                    [](u8 value) { return value == 0xa5; }));
                Require(std::equal(expected.begin() + position,
                                   expected.begin() + position + extent.size,
                                   fresh.begin() + extent.offset));
                position += extent.size; previous = extent.offset + extent.size;
            }
            Require(std::all_of(fresh.begin() + previous, fresh.end(),
                                [](u8 value) { return value == 0xa5; }));
            const auto before = fresh;
            Reject([&] { transfer.Write(fresh, transfer.Size(), std::span<const u8>{encoded_memory}.first(1)); });
            Reject([&] { transfer.Write(fresh, 0, std::span<const u8>{encoded_memory}); });
            Reject([&] { transfer.Write(std::span{fresh}.first(4096), 0,
                                        std::span<const u8>{encoded_memory}.first(1)); });
            Require(before == fresh);
        }
        const std::array<Core::Pulse::PhysicalExtent, 2> overlap{{{4096, 8192}, {8192, 4096}}};
        Reject([&] { Core::Pulse::PhysicalTransfer invalid{overlap, 65536}; });
        const std::array<Core::Pulse::PhysicalExtent, 1> overflow{{{~u64{4095}, 4096}}};
        Reject([&] { Core::Pulse::PhysicalTransfer invalid{overflow, ~u64{0}}; });
        const Core::Pulse::PhysicalTransfer empty{{}, 0};
        empty.Write({}, 0, {});
        std::cout << "Pulse machine: 100 ARM wire/sparse physical transfer cycles; bounds and holes passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
