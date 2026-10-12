// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <span>
#include <vector>
#include "common/pulse_archive.h"
#include "core/hle/kernel/svc_types.h"
#include "core/arm/exclusive_monitor.h"

namespace Core::Pulse {

// Explicit little-endian wire representation. Never serialize ThreadContext padding,
// a host ABI structure, the JIT, or pointers. Version belongs to the archive section.
inline constexpr size_t ContextSize = 796;
using EncodedContext = std::array<u8, ContextSize>;

inline EncodedContext EncodeContext(const Kernel::Svc::ThreadContext& context) {
    EncodedContext output{};
    size_t offset{};
    const auto put = [&](u64 value, size_t width) {
        for (size_t i = 0; i < width; ++i) output[offset++] = static_cast<u8>(value >> (i * 8));
    };
    for (auto value : context.r) put(value, 8);
    put(context.fp, 8);
    put(context.lr, 8);
    put(context.sp, 8);
    put(context.pc, 8);
    put(context.pstate, 4);
    for (const auto& vector : context.v) {
        put(vector[0], 8);
        put(vector[1], 8);
    }
    put(context.fpcr, 4);
    put(context.fpsr, 4);
    put(context.tpidr, 8);
    return output;
}

inline Kernel::Svc::ThreadContext DecodeContext(std::span<const u8> input) {
    if (input.size() != ContextSize) {
        throw Common::Pulse::Failure(Common::Pulse::Error::Invalid, "Pulse: ARM context size");
    }
    Kernel::Svc::ThreadContext context{};
    size_t offset{};
    const auto get = [&](size_t width) {
        u64 value{};
        for (size_t i = 0; i < width; ++i) value |= u64{input[offset++]} << (i * 8);
        return value;
    };
    for (auto& value : context.r) value = get(8);
    context.fp = get(8);
    context.lr = get(8);
    context.sp = get(8);
    context.pc = get(8);
    context.pstate = static_cast<u32>(get(4));
    for (auto& vector : context.v) {
        vector[0] = get(8);
        vector[1] = get(8);
    }
    context.fpcr = static_cast<u32>(get(4));
    context.fpsr = static_cast<u32>(get(4));
    context.tpidr = get(8);
    return context;
}

inline constexpr size_t ExclusiveStateSize = 136;
using EncodedExclusiveState = std::array<u8, ExclusiveStateSize>;

inline void ValidateExclusiveState(const ExclusiveMonitorCheckpoint& state) {
    if (!state.core_count || state.core_count > state.reservations.size()) {
        throw Common::Pulse::Failure(Common::Pulse::Error::Invalid, "Pulse: exclusive core count");
    }
    for (size_t i = 0; i < state.reservations.size(); ++i) {
        const auto& r = state.reservations[i];
        if (((i >= state.core_count || !r.active) && (r.active || r.address || r.value != u128{})) ||
            (r.active && r.address == 0xdeaddeaddeaddeadULL)) {
            throw Common::Pulse::Failure(Common::Pulse::Error::Invalid, "Pulse: invalid exclusive reservation");
        }
    }
}

inline EncodedExclusiveState EncodeExclusiveState(const ExclusiveMonitorCheckpoint& state) {
    ValidateExclusiveState(state);
    EncodedExclusiveState bytes{};
    size_t offset{};
    const auto put = [&](u64 value, size_t width) {
        for (size_t i = 0; i < width; ++i) bytes[offset++] = static_cast<u8>(value >> (i * 8));
    };
    put(state.core_count, 4);
    put(0, 4);
    for (const auto& r : state.reservations) {
        put(r.address, 8); put(r.value[0], 8); put(r.value[1], 8);
        put(r.active ? 1 : 0, 4); put(0, 4);
    }
    return bytes;
}

inline ExclusiveMonitorCheckpoint DecodeExclusiveState(std::span<const u8> bytes) {
    if (bytes.size() != ExclusiveStateSize) {
        throw Common::Pulse::Failure(Common::Pulse::Error::Invalid, "Pulse: exclusive section size");
    }
    size_t offset{};
    const auto get = [&](size_t width) {
        u64 value{};
        for (size_t i = 0; i < width; ++i) value |= u64{bytes[offset++]} << (i * 8);
        return value;
    };
    ExclusiveMonitorCheckpoint state{};
    state.core_count = static_cast<u32>(get(4));
    if (get(4)) throw Common::Pulse::Failure(Common::Pulse::Error::Invalid, "Pulse: exclusive reserved bits");
    for (auto& r : state.reservations) {
        r.address = get(8); r.value[0] = get(8); r.value[1] = get(8);
        const auto active = get(4);
        if (active > 1 || get(4)) {
            throw Common::Pulse::Failure(Common::Pulse::Error::Invalid, "Pulse: exclusive reserved bits");
        }
        r.active = active != 0;
    }
    ValidateExclusiveState(state);
    return state;
}

// Physical guest allocations, NOT the kernel slab (which contains host C++ objects).
// The stopped kernel must supply a validated allocation inventory. Aliased virtual
// mappings reference these physical extents and must be reconstructed separately.
struct PhysicalExtent {
    u64 offset{};
    u64 size{};
    bool operator==(const PhysicalExtent&) const = default;
};

class PhysicalTransfer final {
public:
    // Copies only extent metadata. No copy of guest RAM or resident capture buffers.
    PhysicalTransfer(std::span<const PhysicalExtent> allocations, u64 backing_size)
    {
        if (allocations.size() > MaxExtents) Fail("Pulse: physical extent count");
        extents.assign(allocations.begin(), allocations.end());
        ends.reserve(extents.size());
        std::sort(extents.begin(), extents.end(), [](auto a, auto b) {
            return a.offset < b.offset;
        });
        u64 previous_end{};
        for (auto extent : extents) {
            if (!extent.size || (extent.offset & 4095) || (extent.size & 4095) ||
                extent.offset < previous_end || extent.offset > backing_size ||
                extent.size > backing_size - extent.offset ||
                extent.size > std::numeric_limits<u64>::max() - total) {
                Fail("Pulse: invalid or overlapping physical extent");
            }
            previous_end = extent.offset + extent.size;
            total += extent.size;
            ends.push_back(total);
        }
    }

    u64 Size() const noexcept { return total; }
    std::span<const PhysicalExtent> Extents() const noexcept { return extents; }

    void Read(std::span<const u8> backing, u64 offset, std::span<u8> output) const {
        Transfer(backing, offset, output.size(), [&](u64 physical, size_t position, size_t size) {
            std::memcpy(output.data() + position, backing.data() + physical, size);
        });
    }

    // Only into a fresh stopped system, after full archive validation. On failure,
    // destroy that system; this is not a transactional mutation of a running guest.
    void Write(std::span<u8> backing, u64 offset, std::span<const u8> input) const {
        Transfer(backing, offset, input.size(), [&](u64 physical, size_t position, size_t size) {
            std::memcpy(backing.data() + physical, input.data() + position, size);
        });
    }

private:
    [[noreturn]] static void Fail(const char* reason) {
        throw Common::Pulse::Failure(Common::Pulse::Error::Invalid, reason);
    }

    template <class Byte, class Copy>
    void Transfer(std::span<Byte> backing, u64 offset, size_t size, Copy copy) const {
        if (size > Common::Pulse::BlockSize || offset > total || size > total - offset) {
            Fail("Pulse: physical transfer bounds");
        }
        // Validate the whole backing before any destination mutation.
        if (!extents.empty() && (extents.back().offset > backing.size() ||
            extents.back().size > backing.size() - extents.back().offset)) {
            Fail("Pulse: physical backing size changed");
        }
        size_t index = std::upper_bound(ends.begin(), ends.end(), offset) - ends.begin();
        size_t position{};
        while (position < size) {
            const u64 start = index ? ends[index - 1] : 0;
            const u64 within = offset - start;
            const auto count = static_cast<size_t>(std::min<u64>(size - position,
                                                               extents[index].size - within));
            copy(extents[index].offset + within, position, count);
            offset += count;
            position += count;
            ++index;
        }
    }

    static constexpr size_t MaxExtents = 65536;
    std::vector<PhysicalExtent> extents;
    std::vector<u64> ends;
    u64 total{};
};
} // namespace Core::Pulse
