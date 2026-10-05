// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later

// JIT compile throughput: translates, optimizes and emits many distinct A64 blocks with the
// production Dynarmic library and reports the time per block. Link it against two builds of
// dynarmic.lib to compare them. Build from a desktop x64 vcvars prompt (not build-env.bat):
//   cl /O2 /MD /std:c++20 /EHsc /utf-8 /DNOMINMAX /I src /I src\dynarmic\src /I <fmt include>
//      /I build-uwp\src jit-compile-bench.cpp dynarmic.lib fmt.lib mincore.lib
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <vector>
#include "common/logging.h"
#include "dynarmic/interface/A64/a64.h"

void AssertFatalImpl() { std::abort(); }
void AssertFailSoftImpl() { std::abort(); }
void AssertFailedAt(const char*) { std::abort(); }
void UnreachableAt(const char*) { std::abort(); }
namespace Common::Log {
void FmtLogMessageImpl(Class, Level, const char*, unsigned, const char*, fmt::string_view,
                       const fmt::format_args&) {}
}

constexpr std::uint64_t BASE = 0x10000;
constexpr int BLOCKS = 6000;
constexpr int INSTS = 32; // per block, plus the SVC that ends it

struct Callbacks final : Dynarmic::A64::UserCallbacks {
    Dynarmic::A64::Jit* jit{};
    std::vector<std::uint32_t> code;
    std::optional<std::uint32_t> MemoryReadCode(std::uint64_t addr) override {
        const std::uint64_t i = (addr - BASE) / 4;
        return addr >= BASE && i < code.size() ? std::optional{code[i]} : std::nullopt;
    }
    std::uint64_t MemoryRead(std::uint64_t, std::size_t) override { return 0; }
    Dynarmic::A64::Vector MemoryRead128(std::uint64_t) override { return {}; }
    void MemoryWrite(std::uint64_t, std::uint64_t, std::size_t) override {}
    void MemoryWrite128(std::uint64_t, Dynarmic::A64::Vector) override {}
    void CallSVC(std::uint32_t) override { jit->HaltExecution(Dynarmic::HaltReason::UserDefined1); }
    void ExceptionRaised(std::uint64_t, Dynarmic::A64::Exception) override { std::abort(); }
    void AddTicks(std::uint64_t) override {}
    std::uint64_t GetTicksRemaining() override { return 1u << 30; }
    std::uint64_t GetCNTPCT() override { return 0; }
};

// A mix close to compiled game code: integer ALU, compares and selects, loads and stores,
// scalar FP and NEON. Registers 0..28 only (31 would be SP or ZR depending on the opcode).
static std::uint32_t Instruction(std::uint32_t& rng) {
    rng = rng * 1664525u + 1013904223u;
    const std::uint32_t r = rng >> 8;
    const std::uint32_t d = r % 29, n = (r >> 5) % 29, m = (r >> 10) % 29, a = (r >> 15) % 29;
    const std::uint32_t imm = (r >> 3) & 0xfff;
    switch ((rng >> 27) % 14) {
    case 0: return 0x91000000 | imm << 10 | n << 5 | d;           // ADD Xd, Xn, #imm
    case 1: return 0xCB000000 | m << 16 | n << 5 | d;             // SUB Xd, Xn, Xm
    case 2: return 0xCA000000 | m << 16 | n << 5 | d;             // EOR
    case 3: return 0x8A000000 | m << 16 | n << 5 | d;             // AND
    case 4: return 0x9B000000 | m << 16 | a << 10 | n << 5 | d;   // MADD
    case 5: return 0x9AC02000 | m << 16 | n << 5 | d;             // LSLV
    case 6: return 0xEB00001F | m << 16 | n << 5;                 // CMP Xn, Xm
    case 7: return 0x9A800000 | m << 16 | (r & 0xd) << 12 | n << 5 | d; // CSEL
    case 8: return 0xF9400000 | (imm & 0x1ff) << 10 | n << 5 | d; // LDR Xt, [Xn, #imm]
    case 9: return 0xF9000000 | (imm & 0x1ff) << 10 | n << 5 | d; // STR Xt, [Xn, #imm]
    case 10: return 0x1E602800 | m << 16 | n << 5 | d;            // FADD Dd, Dn, Dm
    case 11: return 0x1E600800 | m << 16 | n << 5 | d;            // FMUL Dd, Dn, Dm
    case 12: return 0x4EA08400 | m << 16 | n << 5 | d;            // ADD Vd.4S
    default: return 0x4E20DC00 | m << 16 | n << 5 | d;            // FMUL Vd.4S
    }
}

int main() {
    Callbacks cb;
    std::uint32_t rng = 12345;
    for (int b = 0; b < BLOCKS; ++b) {
        for (int i = 0; i < INSTS; ++i) {
            cb.code.push_back(Instruction(rng));
        }
        cb.code.push_back(0xD4000001); // SVC #0
    }
    Dynarmic::A64::UserConfig config{};
    config.callbacks = &cb;
    config.code_cache_size = 256 * 1024 * 1024;
    std::vector<double> runs;
    for (int run = 0; run < 7; ++run) {
        Dynarmic::A64::Jit jit{config};
        cb.jit = &jit;
        const auto start = std::chrono::steady_clock::now();
        for (int b = 0; b < BLOCKS; ++b) {
            jit.SetPC(BASE + std::uint64_t(b) * (INSTS + 1) * 4);
            jit.Run();
        }
        const std::chrono::duration<double, std::micro> took =
            std::chrono::steady_clock::now() - start;
        runs.push_back(took.count() / BLOCKS);
    }
    std::sort(runs.begin(), runs.end());
    std::printf("%d blocks x %d instructions: median %.2f us/block (min %.2f, max %.2f)\n",
                BLOCKS, INSTS + 1, runs[runs.size() / 2], runs.front(), runs.back());
}
