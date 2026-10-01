// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cassert>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <thread>
#include "core/arm/jit_prewarm_parallel.h"
#include "common/logging.h"
#include "core/arm/dynarmic/jit_prewarm_profile.h"
#include "dynarmic/interface/A64/a64.h"
#include "dynarmic/interface/jit_profile.h"

// Standalone harness links the production UWP Dynarmic library. Fail assertions
// hard; it does not need the frontend logging service or a kernel instance.
void AssertFatalImpl() { std::abort(); }
void AssertFailSoftImpl() { std::abort(); }
namespace Common::Log {
void FmtLogMessageImpl(Class, Level, const char*, unsigned, const char*,
                       fmt::string_view, const fmt::format_args&) {}
}

struct Callbacks final : Dynarmic::A64::UserCallbacks {
    Dynarmic::A64::Jit* jit{};
    std::array<std::uint32_t, 2> code{0xd2800540, 0xd4000001}; // MOV X0,42; SVC 0
    std::uint64_t base{0x1000};
    unsigned reads{}, svcs{}, writes{};
    std::optional<std::uint32_t> MemoryReadCode(std::uint64_t addr) override {
        ++reads;
        if (addr >= base && addr - base < sizeof(code) && !(addr & 3)) {
            return code[(addr - base) / 4];
        }
        return std::nullopt;
    }
    std::uint64_t MemoryRead(std::uint64_t, std::size_t) override { std::abort(); }
    Dynarmic::A64::Vector MemoryRead128(std::uint64_t) override { std::abort(); }
    void MemoryWrite(std::uint64_t, std::uint64_t, std::size_t) override { ++writes; }
    void MemoryWrite128(std::uint64_t, Dynarmic::A64::Vector) override { ++writes; }
    void CallSVC(std::uint32_t) override {
        ++svcs;
        jit->HaltExecution(Dynarmic::HaltReason::UserDefined1);
    }
    void ExceptionRaised(std::uint64_t, Dynarmic::A64::Exception) override { std::abort(); }
    void AddTicks(std::uint64_t) override {}
    std::uint64_t GetTicksRemaining() override { return 1000000; }
    std::uint64_t GetCNTPCT() override { return 0; }
};

int main() {
    using namespace Core::JitPrewarm;
    Callbacks cb;
    Dynarmic::A64::UserConfig config{};
    config.callbacks = &cb;
    config.code_cache_size = 32 * 1024 * 1024;
    Dynarmic::BlockProfile learned{};
    {
        Dynarmic::A64::Jit jit{config};
        cb.jit = &jit;
        jit.SetBlockProfileCallback([&](const auto& r) { learned = r; });
        jit.SetPC(cb.base);
        jit.Run();
        assert(jit.GetRegister(0) == 42 && cb.svcs == 1 && learned.code_bytes == 8);
    }
    cb.reads = cb.svcs = cb.writes = 0;
    {
        Dynarmic::A64::Jit jit{config};
        cb.jit = &jit;
        jit.SetPC(0x8888);
        jit.SetRegister(0, 99);
        const auto space = jit.GetCodeCacheSpaceRemaining();
        assert(jit.PrecompileBlock(learned));
        assert(jit.GetCodeCacheSpaceRemaining() < space);
        assert(jit.GetPC() == 0x8888 && jit.GetRegister(0) == 99 && cb.svcs == 0 && cb.writes == 0);
        cb.code[0] ^= 0x20;
        assert(!jit.PrecompileBlock(learned)); // Existing code cannot be replaced by prewarm.
        cb.code[0] ^= 0x20;
        jit.SetPC(cb.base);
        cb.reads = 0;
        jit.Run();
        assert(cb.reads == 0 && jit.GetRegister(0) == 42 && cb.svcs == 1);
    }
    {
        Dynarmic::A64::Jit jit{config};
        cb.jit = &jit;
        const auto space = jit.GetCodeCacheSpaceRemaining();
        auto bad = learned;
        bad.code_hash ^= 1;
        assert(!jit.PrecompileBlock(bad) && jit.GetCodeCacheSpaceRemaining() == space);
        bad = learned;
        bad.code_bytes = 4;
        assert(!jit.PrecompileBlock(bad)); // Bounded translation must not emit fault code.
        bad.code_bytes = 12;
        assert(!jit.PrecompileBlock(bad));
        auto relocated = learned;
        cb.base = 0x5000;
        relocated.descriptor = cb.base;
        assert(jit.PrecompileBlock(relocated));
    }

    BuildId build{};
    build[0] = 7;
    Record r{0x100, learned.code_hash, 8, 7};
    std::vector<Record> records{r};
    std::ostringstream output{std::ios::binary};
    assert(Write(output, 123, build, 2, records));
    const auto bytes = output.str();
    std::vector<Record> loaded;
    const auto check = [&](const std::string& input, std::uint64_t title = 123,
                           std::uint32_t core = 2) {
        std::istringstream file{input, std::ios::binary};
        return Read(file, input.size(), title, build, core, loaded);
    };
    assert(check(bytes) && loaded.size() == 1 && loaded[0].code_hash == r.code_hash);
    assert(loaded[0].gameplay_samples == 7);
    std::ostringstream legacy{std::ios::binary};
    legacy.write("EDJITP01", 8);
    WriteInteger(legacy, 123, 8);
    legacy.write(reinterpret_cast<const char*>(build.data()), build.size());
    WriteInteger(legacy, 2, 4);
    WriteInteger(legacy, 1, 4);
    WriteInteger(legacy, PayloadHash(records, true), 8);
    WriteInteger(legacy, r.descriptor, 8);
    WriteInteger(legacy, r.code_hash, 8);
    WriteInteger(legacy, r.code_bytes, 4);
    assert(check(legacy.str()) && loaded[0].gameplay_samples == 0);
    for (size_t n = 0; n < bytes.size(); ++n) {
        assert(!check(bytes.substr(0, n)));
        auto corrupt = bytes;
        corrupt[n] ^= 1;
        assert(!check(corrupt));
    }
    assert(!check(bytes + 'x') && !check(bytes, 124) && !check(bytes, 123, 3));
    build[0] ^= 1;
    assert(!check(bytes));
    build[0] ^= 1;
    Record updated = r;
    updated.code_hash ^= 2;
    updated.gameplay_samples = 2;
    std::array<Record, 2> recent{updated, Record{0x200, 17, 4}};
    Merge(records, recent);
    assert(records.size() == 2 && records[0].code_hash == updated.code_hash);
    assert(records[0].gameplay_samples == 2); // Changed hash resets old priority.
    Merge(records, std::span<const Record>{&updated, 1});
    assert(records[0].gameplay_samples == 4);
    std::vector<Record> bounded;
    bounded.reserve(MaxRecords + 1);
    for (size_t n = 0; n <= MaxRecords; ++n) bounded.emplace_back(n * 4, 1, 4, n == MaxRecords ? 1 : 0);
    Merge(bounded, {});
    assert(bounded.size() == MaxRecords && bounded.back().descriptor == MaxRecords * 4);
    assert(std::is_sorted(bounded.begin(), bounded.end(), [](const auto& a, const auto& b) { return a.descriptor < b.descriptor; }));
    Profile p;
    p.base = 0x1000;
    p.executable_ranges = {{0x1000, 0x1010}, {0x2000, 0x2010}};
    p.observed.reserve(MaxRecords);
    p.Observe(Record{0x1008 | DescriptorMask, 1, 8});
    p.Observe(Record{0x100c, 2, 8}); // Crosses the executable segment.
    p.Observe(Record{0x1800, 2, 4}); // Unmapped hole.
    assert(p.observed.size() == 1 && p.observed[0].descriptor == (8 | DescriptorMask));
    p.loaded = {{8, 1, 8}, {0x100, 1, 4, 3}};
    p.status = {WarmStatus::Budget, WarmStatus::Accepted};
    assert(p.WarmPlan().size() == 2 && p.WarmPlan()[0].index == 1);
    assert(p.Classify(p.loaded[0]) == Miss::Budget);
    p.status[0] = WarmStatus::Rejected;
    assert(p.Classify(p.loaded[0]) == Miss::Rejected);
    p.status[0] = WarmStatus::Accepted;
    assert(p.Classify(p.loaded[0]) == Miss::Recompiled);
    p.status[0] = WarmStatus::RecordOnly;
    assert(p.Classify(p.loaded[0]) == Miss::RecordOnly);
    auto changed = p.loaded[0]; changed.code_hash ^= 1;
    assert(p.Classify(changed) == Miss::CodeChanged);
    auto catalog = std::make_shared<Catalog>();
    catalog->descriptors[1] = {0x200};
    catalog->Finalize();
    p.catalog = catalog;
    assert(p.Classify(Record{0x200, 1, 4}) == Miss::OtherCore);
    assert(p.Classify(Record{0x200 | DescriptorMask, 1, 4}) == Miss::FpcrVariant);
    assert(p.Classify(Record{0x300, 1, 4}) == Miss::Unlearned);
    p.observed.resize(MaxRecords - GameplayCapacity);
    capture_active.store(true);
    p.Observe(Record{0x1008 | DescriptorMask, 1, 8});
    capture_active.store(false);
    assert(p.gameplay_observed.size() == 1 && p.gameplay_observed[0].gameplay_samples == 1);
    assert(p.observed.size() == MaxRecords - GameplayCapacity); // Boot saturation preserves T capacity.
    catalog->priority = {{8, 1, 8, 2}, {0x200, 1, 4, 2}, {0x200, 1, 4, 5},
                         {0x300, 1, 4, 3}, {0x300, 2, 4, 4}};
    catalog->Finalize();
    assert(catalog->priority.size() == 2 && catalog->priority.back().gameplay_samples == 5);
    assert(p.PrepareShared() == 3 && p.shared.size() == 1 && p.shared[0].descriptor == 0x200);
    const auto plan = p.WarmPlan();
    assert(plan[0].index == 1 && !plan[0].shared && plan[0].priority == 2);
    assert(plan[1].shared && plan[2].priority == 1); // Own priority, shared priority, promoted own.
    assert(p.Classify(p.shared[0]) == Miss::Budget);
    p.shared_status[0] = WarmStatus::Accepted;
    assert(p.Classify(p.shared[0]) == Miss::Recompiled);
    changed = p.shared[0]; changed.code_hash ^= 1;
    assert(p.Classify(changed) == Miss::CodeChanged);
    Profile inactive; inactive.catalog = catalog;
    assert(inactive.PrepareShared() == 0 && inactive.shared.empty());
    // Three distinct production JITs compile concurrently; none executes guest code.
    std::array<Callbacks, 3> parallel_cb;
    std::array<std::unique_ptr<Dynarmic::A64::Jit>, 3> parallel_jit;
    for (size_t n = 0; n < parallel_jit.size(); ++n) {
        auto owner_config = config; owner_config.callbacks = &parallel_cb[n];
        parallel_jit[n] = std::make_unique<Dynarmic::A64::Jit>(owner_config);
        parallel_cb[n].jit = parallel_jit[n].get();
        parallel_jit[n]->SetPC(0x8888); parallel_jit[n]->SetRegister(0, 99);
    }
    parallel_cb[2].code[0] ^= 0x20;
    Dynarmic::JitProfile::SetEnabled(true);
    const auto coordinator_compile_ns = Dynarmic::JitProfile::ReadLocalCompileNs();
    std::array<std::uint64_t, 3> worker_compile_ns{};
    std::atomic<unsigned> entered{}, accepted_parallel{};
    const auto coordinator = std::this_thread::get_id();
    RunOwners({1, 1, 1, 0}, [&](auto core, const Progress& update) {
        entered.fetch_add(1);
        while (entered.load() != 3) std::this_thread::yield();
        const auto before_compile = Dynarmic::JitProfile::ReadLocalCompileNs();
        if (parallel_jit[core]->PrecompileBlock(learned)) accepted_parallel.fetch_add(1);
        worker_compile_ns[core] = Dynarmic::JitProfile::ReadLocalCompileNs() - before_compile;
        update(1, 1);
    }, [&](auto done, auto total) {
        assert(std::this_thread::get_id() == coordinator && done <= total && total == 3);
    });
    assert(accepted_parallel.load() == 2);
    assert(Dynarmic::JitProfile::ReadLocalCompileNs() == coordinator_compile_ns);
    assert(worker_compile_ns[0] > 0 && worker_compile_ns[1] > 0);
    Dynarmic::JitProfile::SetEnabled(false);
    for (size_t n = 0; n < parallel_jit.size(); ++n) {
        assert(parallel_jit[n]->GetPC() == 0x8888 && parallel_jit[n]->GetRegister(0) == 99);
        assert(parallel_cb[n].svcs == 0 && parallel_cb[n].writes == 0);
        if (n < 2) {
            parallel_cb[n].reads = 0;
            parallel_jit[n]->SetPC(0x1000); parallel_jit[n]->Run();
            assert(parallel_cb[n].reads == 0 && parallel_jit[n]->GetRegister(0) == 42);
        }
    }
    std::atomic_bool other_owner_finished{};
    bool caught{};
    try {
        RunOwners({1, 1, 0, 0}, [&](auto core, const Progress&) {
            if (core == 0) throw std::runtime_error("owner failure");
            other_owner_finished.store(true);
        }, {});
    } catch (const std::runtime_error&) { caught = true; }
    assert(caught && other_owner_finished.load());
    other_owner_finished.store(false); caught = false;
    try {
        RunOwners({1, 0, 0, 0}, [&](auto, const Progress&) {
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
            other_owner_finished.store(true);
        }, [](auto, auto) { throw std::runtime_error("UI failure"); });
    } catch (const std::runtime_error&) { caught = true; }
    assert(caught && other_owner_finished.load()); // Unwind joins before callbacks die.
    const auto target = std::filesystem::temp_directory_path() / "eden-jit-prewarm-test.bin";
    auto temp = target;
    temp += ".tmp";
    { std::ofstream file{target, std::ios::binary}; file << "old"; }
    { std::ofstream file{temp, std::ios::binary}; assert(Write(file, 123, build, 2, records)); }
    std::error_code ec;
    std::filesystem::rename(temp, target, ec);
    assert(!ec);
    { std::ifstream file{target, std::ios::binary};
      assert(Read(file, std::filesystem::file_size(target), 123, build, 2, loaded)); }
    std::filesystem::remove(target, ec);
    std::cout << "PASS: production JIT precompile/state/hash/relocation; profile corruption, identity, v1 migration, priority, bounded T recording, miss classification, merge, ranges, shared candidates and parallel owner isolation/unwind\n";
}
