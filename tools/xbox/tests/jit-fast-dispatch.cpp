// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Standalone Windows x64 regression: software invalidation hash must match the generated
// fast dispatcher, including the CRC seed's truncation and the non-SSE4.2 fallback.
#include <array>
#include <cstdio>
#include <random>

#include <xbyak/xbyak.h>
#include <xbyak/xbyak_util.h>
#include "dynarmic/common/crypto/crc32.h"

struct LookupOffset : Xbyak::CodeGenerator {
    explicit LookupOffset(bool crc) {
        // Windows x64 ABI: rcx = descriptor, rdx = table address. Same operations as
        // A64EmitX64::GenTerminalHandlers, returning the byte offset instead of a pointer.
        if (crc) crc32(rcx, rdx);
        and_(ecx, 0xFFFFF0);
        mov(rax, rcx);
        ret();
        ready();
    }
};

int main() {
    const Xbyak::util::Cpu cpu;
    const bool sse42 = cpu.has(Xbyak::util::Cpu::tSSE42);
    std::mt19937_64 random{0xEDE12345};
    std::array<u64, 8> addresses{0, 1, 0xFFFFFFFF, 0x100000000, 0x7FFF12345000,
                               0xFFFF000012345000, 0xFFFFFFFFFFFFFFFF,
                               reinterpret_cast<u64>(&random)};
    u64 cases = 0;
    for (const bool crc : {false, true}) {
        if (crc && !sse42) continue;
        LookupOffset lookup{crc};
        const auto generated = lookup.getCode<u64 (*)(u64, u64)>();
        for (const u64 address : addresses) {
            for (size_t i = 0; i < 16384; ++i) {
                const u64 descriptor = i == 0 ? 0 : i == 1 ? ~u64{} : random();
                const u64 hash = crc
                    ? Dynarmic::Common::Crypto::CRC32::ComputeCRC32Castagnoli(
                          static_cast<u32>(descriptor), address, sizeof(u64))
                    : descriptor;
                if ((hash & 0xFFFFF0) != generated(descriptor, address)) {
                    std::fprintf(stderr, "fast-dispatch hash mismatch: crc=%d case=%zu\n", crc, i);
                    return 1;
                }
                ++cases;
            }
        }
    }
    std::printf("PASS: %llu fast-dispatch offsets; SSE4.2 %s\n",
                static_cast<unsigned long long>(cases), sse42 ? "tested" : "unavailable");
    return 0;
}
