// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later

// A32 JIT prewarm gate: learn, precompile and relocate ARM and Thumb blocks with the production
// UWP Dynarmic library, and the A32 profile layout. Build like jit-prewarm.cpp.

#include <cassert>
#include <cstdlib>
#include <iostream>
#include <map>
#include <sstream>
#include "common/logging.h"
#include "core/arm/dynarmic/jit_prewarm_profile.h"
#include "dynarmic/interface/A32/a32.h"

// Standalone harness links the production UWP Dynarmic library. Fail assertions
// hard; it does not need the frontend logging service or a kernel instance.
void AssertFatalImpl() { std::abort(); }
void AssertFailSoftImpl() { std::abort(); }
void AssertFailedAt(const char*) { std::abort(); }
void UnreachableAt(const char*) { std::abort(); }
namespace Common::Log {
void FmtLogMessageImpl(Class, Level, const char*, unsigned, const char*,
                       fmt::string_view, const fmt::format_args&) {}
}

namespace {

constexpr std::uint32_t ThumbBit = 0x20; // CPSR.T
constexpr std::uint64_t DescriptorThumb = 1ULL << 32;
constexpr std::uint64_t DescriptorSingleStep = 4ULL << 32;

struct Callbacks final : Dynarmic::A32::UserCallbacks {
    Dynarmic::A32::Jit* jit{};
    std::map<std::uint32_t, std::uint32_t> code;
    unsigned reads{}, svcs{}, writes{};

    std::optional<std::uint32_t> MemoryReadCode(std::uint32_t addr) override {
        ++reads;
        assert((addr & 3) == 0); // TranslateCallbacks contract, also through the fingerprint.
        if (const auto it = code.find(addr); it != code.end()) return it->second;
        return std::nullopt;
    }
    std::uint64_t MemoryRead(std::uint32_t, std::size_t) override { std::abort(); }
    void MemoryWrite(std::uint32_t, std::uint64_t, std::size_t) override { ++writes; }
    void CallSVC(std::uint32_t) override {
        ++svcs;
        jit->HaltExecution(Dynarmic::HaltReason::UserDefined1);
    }
    void ExceptionRaised(std::uint32_t, Dynarmic::A32::Exception) override { std::abort(); }
    void AddTicks(std::uint64_t) override {}
    std::uint64_t GetTicksRemaining() override { return 1000000; }

    // ARM: MOV R0,#42; SVC #0.
    void LoadArm(std::uint32_t base) {
        code[base] = 0xe3a0002a;
        code[base + 4] = 0xef000000;
    }
    // Thumb from base+2 (mid-word): MOVS R0,#42; MOVW R1,#0x1234; SVC #0. The 32-bit MOVW
    // sits in one word, so Translate reads that word twice.
    void LoadThumb(std::uint32_t base) {
        code[base] = 0x202abf00;     // NOP (unused) | MOVS R0,#42
        code[base + 4] = 0x2134f241; // MOVW R1,#0x1234
        code[base + 8] = 0xbf00df00; // SVC #0 | NOP
    }
};

struct Fixture {
    Callbacks cb;
    Dynarmic::A32::UserConfig config{};
    Fixture() {
        config.callbacks = &cb;
        config.code_cache_size = 32 * 1024 * 1024;
    }
    std::unique_ptr<Dynarmic::A32::Jit> MakeJit() {
        auto jit = std::make_unique<Dynarmic::A32::Jit>(config);
        cb.jit = jit.get();
        return jit;
    }
};

void Start(Dynarmic::A32::Jit& jit, std::uint32_t pc, bool thumb) {
    jit.SetCpsr(thumb ? ThumbBit : 0);
    jit.Regs()[15] = pc;
}

Dynarmic::BlockProfile Learn(Fixture& f, std::uint32_t pc, bool thumb) {
    std::vector<Dynarmic::BlockProfile> learned;
    auto jit = f.MakeJit();
    jit->SetBlockProfileCallback([&](const auto& r) { learned.push_back(r); });
    Start(*jit, pc, thumb);
    f.cb.svcs = 0;
    jit->Run();
    assert(f.cb.svcs == 1 && jit->Regs()[0] == 42);
    assert(!learned.empty());
    return learned.front();
}

// Precompile in a fresh JIT without touching guest state; the guest then runs the warmed
// block with no translation reads.
void CheckWarm(Fixture& f, const Dynarmic::BlockProfile& block, std::uint32_t pc, bool thumb) {
    auto jit = f.MakeJit();
    Start(*jit, 0x8888, false);
    jit->Regs()[0] = 99;
    f.cb.svcs = f.cb.writes = 0;
    const auto space = jit->GetCodeCacheSpaceRemaining();
    const auto cpsr = jit->Cpsr();
    assert(jit->PrecompileBlock(block));
    assert(jit->GetCodeCacheSpaceRemaining() < space);
    assert(jit->Regs()[15] == 0x8888 && jit->Regs()[0] == 99 && jit->Cpsr() == cpsr);
    assert(f.cb.svcs == 0 && f.cb.writes == 0);
    assert(!jit->PrecompileBlock(block)); // An existing entry is never replaced by prewarm.
    Start(*jit, pc, thumb);
    f.cb.reads = 0;
    jit->Run();
    assert(f.cb.reads == 0 && jit->Regs()[0] == 42 && f.cb.svcs == 1);
}

void CheckRejected(Fixture& f, const Dynarmic::BlockProfile& block) {
    auto jit = f.MakeJit();
    const auto space = jit->GetCodeCacheSpaceRemaining();
    assert(!jit->PrecompileBlock(block));
    assert(jit->GetCodeCacheSpaceRemaining() == space); // Rejected before emitting.
}

} // namespace

int main() {
    using namespace Core::JitPrewarm;
    Fixture f;

    // ARM: learn, warm, reject mismatches, relocate.
    f.cb.LoadArm(0x1000);
    const auto arm = Learn(f, 0x1000, false);
    assert(arm.descriptor == 0x1000 && arm.code_bytes == 8);
    CheckWarm(f, arm, 0x1000, false);
    auto bad = arm;
    bad.code_hash ^= 1;
    CheckRejected(f, bad);
    bad = arm;
    bad.code_bytes = 4; // Shorter than the block: bounded translation must not emit.
    CheckRejected(f, bad);
    bad.code_bytes = 12;
    CheckRejected(f, bad);
    bad = arm;
    bad.descriptor |= 2; // ARM PC must be word-aligned.
    CheckRejected(f, bad);
    bad = arm;
    bad.descriptor |= DescriptorSingleStep;
    CheckRejected(f, bad);
    bad = arm;
    bad.descriptor |= 1ULL << 63; // Bits A32 descriptors never use.
    CheckRejected(f, bad);
    f.cb.code[0x1000] ^= 0x20;
    CheckRejected(f, arm); // Same address, different code.
    f.cb.code[0x1000] ^= 0x20;
    f.cb.LoadArm(0x5000);
    auto relocated = arm;
    relocated.descriptor = 0x5000;
    CheckWarm(f, relocated, 0x5000, false);

    // Thumb, starting mid-word: span counted from the word, repeated word reads hashed.
    f.cb.LoadThumb(0x2000);
    const auto thumb = Learn(f, 0x2002, true);
    assert(thumb.descriptor == (0x2002 | DescriptorThumb) && thumb.code_bytes == 12);
    {
        Dynarmic::BlockProfile expected{thumb.descriptor};
        for (const auto word : {0x202abf00u, 0x2134f241u, 0x2134f241u, 0xbf00df00u}) expected.MixWord(word);
        assert(thumb.code_hash == expected.code_hash);
    }
    CheckWarm(f, thumb, 0x2002, true);
    {
        auto jit = f.MakeJit();
        Start(*jit, 0x2002, true);
        jit->Run();
        assert(jit->Regs()[1] == 0x1234);
    }
    bad = thumb;
    bad.code_bytes = 8;
    CheckRejected(f, bad);
    bad = thumb;
    bad.descriptor |= 1; // Thumb PC must be halfword-aligned.
    CheckRejected(f, bad);
    bad = thumb;
    bad.descriptor &= ~DescriptorThumb; // Same address decoded as ARM is a different block.
    CheckRejected(f, bad);
    f.cb.code[0x2004] ^= 1u << 16; // Second half of the MOVW.
    CheckRejected(f, thumb);
    f.cb.code[0x2004] ^= 1u << 16;
    f.cb.LoadThumb(0x7000);
    relocated = thumb;
    relocated.descriptor = 0x7002 | DescriptorThumb;
    CheckWarm(f, relocated, 0x7002, true);

    // Single-stepping is never learned.
    {
        unsigned learned{};
        auto jit = f.MakeJit();
        jit->SetBlockProfileCallback([&](const auto&) { ++learned; });
        Start(*jit, 0x1000, false);
        jit->Step();
        assert(learned == 0);
    }

    // Layout and profile.
    assert(ValidRecord(A32Layout, Record{thumb}));
    assert(ValidRecord(A32Layout, Record{arm}));
    assert(!ValidRecord(A32Layout, Record{0x1002, 1, 4}));          // ARM halfword PC.
    assert(!ValidRecord(A32Layout, Record{0x1001 | DescriptorThumb, 1, 4}));
    assert(!ValidRecord(A32Layout, Record{0x1000 | DescriptorSingleStep, 1, 4}));
    assert(!ValidRecord(A64Layout, Record{thumb})); // A64 has no halfword PCs.

    Profile p{A32Layout};
    p.base = 0x2000;
    p.executable_ranges = {{0x2000, 0x2010}};
    p.observed.reserve(16);
    p.Observe(thumb);
    p.Observe(Record{0x2006 | DescriptorThumb, 1, 16}); // Its word span leaves the segment.
    p.Observe(Record{0x2004 | DescriptorSingleStep, 1, 4}); // Invalid: never saved.
    assert(p.observed.size() == 1);
    const auto relative = p.observed[0];
    assert(relative.descriptor == (2 | DescriptorThumb) && relative.code_bytes == 12);
    p.base = 0x7000;
    p.executable_ranges = {{0x7000, 0x7010}};
    assert(p.Rebase(relative) == (0x7002 | DescriptorThumb));
    p.executable_ranges = {{0x7000, 0x7008}};
    assert(!p.Rebase(relative)); // Code span no longer inside the RX image.
    p.base = 0xffff'f000;
    assert(!p.Rebase(Record{0x2000, 1, 4})); // PC would overflow 32 bits.

    // Codec: an A32 file round-trips; another layout's file is rejected.
    BuildId build{};
    build[0] = 9;
    std::vector<Record> records{relative, Record{0x100, 5, 8, 3}};
    std::sort(records.begin(), records.end(), [](const auto& a, const auto& b) { return a.descriptor < b.descriptor; });
    std::ostringstream output{std::ios::binary};
    assert(Write(output, A32Layout, 321, build, 1, records));
    const auto bytes = output.str();
    std::vector<Record> loaded;
    const auto check = [&](const std::string& input, const Layout& layout) {
        std::istringstream file{input, std::ios::binary};
        return Read(file, input.size(), layout, 321, build, 1, loaded);
    };
    assert(check(bytes, A32Layout) && loaded.size() == 2 && loaded[1].code_hash == relative.code_hash);
    loaded.clear();
    assert(!check(bytes, A64Layout) && loaded.empty());
    std::ostringstream a64{std::ios::binary};
    assert(Write(a64, A64Layout, 321, build, 1, std::vector<Record>{Record{0x100, 5, 8}}));
    assert(!check(a64.str(), A32Layout));
    for (size_t n = 0; n < bytes.size(); ++n) {
        auto corrupt = bytes;
        corrupt[n] ^= 1;
        assert(!check(corrupt, A32Layout));
    }

    std::cout << "PASS: A32 ARM/Thumb learn, precompile without guest effects, mid-word Thumb span/hash, "
                 "mismatch/alignment/state rejection, relocation, single-step exclusion, layout, rebase and codec\n";
}
