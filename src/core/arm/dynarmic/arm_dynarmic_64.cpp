// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <fstream>
#include "common/fs/path_util.h"
#include "common/settings.h"
#include "core/arm/cpu_profile.h"
#include "core/arm/jit_prewarm.h"
#include "core/arm/jit_prewarm_parallel.h"
#include "core/arm/dynarmic/arm_dynarmic.h"
#include "core/arm/dynarmic/arm_dynarmic_64.h"
#include "core/arm/dynarmic/dynarmic_exclusive_monitor.h"
#include "core/arm/dynarmic/jit_prewarm_profile.h"
#include "core/core_timing.h"
#include "core/hle/kernel/k_process.h"
#include "dynarmic/interface/A64/config.h"
#include "dynarmic/interface/jit_profile.h"

namespace Core {

using namespace Common::Literals;

void ConfigureApplicationPrewarm(System& system, bool warm,
                                const std::function<void(size_t, size_t)>& progress) {
    auto* process = system.ApplicationProcess();
    if (!process || !process->Is64Bit() || Settings::IsNceEnabled()) {
        return;
    }
    try {
        JitPrewarm::application_catalog = std::make_shared<JitPrewarm::Catalog>();
    } catch (const std::exception& e) {
        LOG_WARNING(Core, "JIT profile: cross-core index unavailable: {}", e.what());
    }
    std::array<ArmDynarmic64*, Hardware::NUM_CPU_CORES> owners{};
    for (size_t core = 0; core < owners.size(); ++core) {
        owners[core] = static_cast<ArmDynarmic64*>(process->GetArmInterface(core));
        if (owners[core]) owners[core]->LoadPrewarmProfile();
    }
    if (JitPrewarm::application_catalog) {
        try {
            JitPrewarm::application_catalog->Finalize();
        } catch (const std::exception& e) {
            JitPrewarm::application_catalog->pcs.clear();
            JitPrewarm::application_catalog->priority.clear();
            LOG_WARNING(Core, "JIT profile: cross-core index unavailable: {}", e.what());
        }
        JitPrewarm::application_catalog.reset();
    }
    if (!warm) return;
    std::array<size_t, Hardware::NUM_CPU_CORES> sizes{};
    for (size_t core = 0; core < owners.size(); ++core) {
        if (owners[core]) sizes[core] = owners[core]->PreparePrewarmCandidates();
    }
    const auto start = std::chrono::steady_clock::now();
    try {
        JitPrewarm::RunOwners(sizes, [&](size_t core, const JitPrewarm::Progress& callback) {
            owners[core]->PrewarmBlocks(callback);
        }, progress);
    } catch (const std::exception& e) {
        // RunOwners joins all workers before unwinding. Prepared blocks remain valid.
        LOG_WARNING(Core, "JIT prewarm: parallel stage incomplete, using normal JIT: {}", e.what());
    }
    LOG_INFO(Core, "JIT prewarm parallel: {} owners, {:.1f} ms wall time",
             std::count_if(sizes.begin(), sizes.end(), [](auto size) { return size != 0; }),
             std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());

}

DynarmicCallbacks64::DynarmicCallbacks64(ArmDynarmic64& parent, Kernel::KProcess* process)
    : m_parent{parent}, m_memory(process->GetMemory())
    , m_process(process), m_debugger_enabled{parent.m_system.DebuggerEnabled()}
    , m_check_memory_access{m_debugger_enabled || !Settings::values.cpuopt_ignore_memory_aborts.GetValue()}
{}

u64 DynarmicCallbacks64::MemoryRead(u64 vaddr, size_t size) {
    const CpuProfile::CallbackTimer read_timer{m_parent.m_core_index, true};
    CpuProfile::Add(m_parent.m_core_index, CpuProfile::Counter::Reads);
    CheckMemoryAccess(vaddr, size, Kernel::DebugWatchpointType::Read);
    switch (size) {
    case sizeof(u64): return m_memory.Read64(vaddr);
    case sizeof(u32): return m_memory.Read32(vaddr);
    case sizeof(u16): return m_memory.Read16(vaddr);
    case sizeof(u8): return m_memory.Read8(vaddr);
    default: UNREACHABLE();
    }
}
Dynarmic::A64::Vector DynarmicCallbacks64::MemoryRead128(u64 vaddr) {
    CpuProfile::Add(m_parent.m_core_index, CpuProfile::Counter::Reads);
    CpuProfile::Add(m_parent.m_core_index, CpuProfile::Counter::Reads128);
    CheckMemoryAccess(vaddr, 16, Kernel::DebugWatchpointType::Read);
    return {m_memory.Read64(vaddr), m_memory.Read64(vaddr + 8)};
}

std::optional<u32> DynarmicCallbacks64::MemoryReadCode(u64 vaddr) {
    CpuProfile::Add(m_parent.m_core_index, CpuProfile::Counter::CodeWords);
    // Dynarmic guarantees four-byte-aligned instruction fetches: a word never crosses a
    // guest page. Keep validating each fetch, but avoid the generic range walker.
    if (!m_memory.IsValidVirtualAddress(vaddr))
        return std::nullopt;
    auto const aligned_vaddr = vaddr & ~Core::Memory::YUZU_PAGEMASK;
    if (last_code_addr != aligned_vaddr) {
        m_memory.ReadBlock(aligned_vaddr, &cached_code_page, sizeof(cached_code_page));
        last_code_addr = aligned_vaddr;
    }
    return cached_code_page.inst[(vaddr & Core::Memory::YUZU_PAGEMASK) / sizeof(u32)];
}

void DynarmicCallbacks64::MemoryWrite(Dynarmic::A64::VAddr vaddr, u64 value, std::size_t size) {
    CpuProfile::Add(m_parent.m_core_index, CpuProfile::Counter::Writes);
    if (CheckMemoryAccess(vaddr, size, Kernel::DebugWatchpointType::Write)) {
        switch (size) {
        case sizeof(u64): return m_memory.Write64(vaddr, u64(value));
        case sizeof(u32): return m_memory.Write32(vaddr, u32(value));
        case sizeof(u16): return m_memory.Write16(vaddr, u16(value));
        case sizeof(u8): return m_memory.Write8(vaddr, u8(value));
        default: UNREACHABLE();
        }
    }
}
void DynarmicCallbacks64::MemoryWrite128(u64 vaddr, Dynarmic::A64::Vector value) {
    CpuProfile::Add(m_parent.m_core_index, CpuProfile::Counter::Writes);
    if (CheckMemoryAccess(vaddr, 16, Kernel::DebugWatchpointType::Write)) {
        m_memory.Write64(vaddr, value[0]);
        m_memory.Write64(vaddr + 8, value[1]);
    }
}

bool DynarmicCallbacks64::MemoryWriteExclusive8(u64 vaddr, std::uint8_t value, std::uint8_t expected) {
    return CheckMemoryAccess(vaddr, 1, Kernel::DebugWatchpointType::Write) &&
            m_memory.WriteExclusive8(vaddr, value, expected);
}
bool DynarmicCallbacks64::MemoryWriteExclusive16(u64 vaddr, std::uint16_t value, std::uint16_t expected) {
    return CheckMemoryAccess(vaddr, 2, Kernel::DebugWatchpointType::Write) &&
            m_memory.WriteExclusive16(vaddr, value, expected);
}
bool DynarmicCallbacks64::MemoryWriteExclusive32(u64 vaddr, std::uint32_t value, std::uint32_t expected) {
    return CheckMemoryAccess(vaddr, 4, Kernel::DebugWatchpointType::Write) &&
            m_memory.WriteExclusive32(vaddr, value, expected);
}
bool DynarmicCallbacks64::MemoryWriteExclusive64(u64 vaddr, std::uint64_t value, std::uint64_t expected) {
    return CheckMemoryAccess(vaddr, 8, Kernel::DebugWatchpointType::Write) &&
            m_memory.WriteExclusive64(vaddr, value, expected);
}
bool DynarmicCallbacks64::MemoryWriteExclusive128(u64 vaddr, Dynarmic::A64::Vector value, Dynarmic::A64::Vector expected) {
    return CheckMemoryAccess(vaddr, 16, Kernel::DebugWatchpointType::Write) &&
            m_memory.WriteExclusive128(vaddr, value, expected);
}

void DynarmicCallbacks64::InstructionCacheOperationRaised(Dynarmic::A64::InstructionCacheOperation op, u64 value) {
    last_code_addr = u64(-1); //invalidate cached page
    switch (op) {
    case Dynarmic::A64::InstructionCacheOperation::InvalidateByVAToPoU: {
        static constexpr u64 ICACHE_LINE_SIZE = 64;
        const u64 cache_line_start = value & ~(ICACHE_LINE_SIZE - 1);
        m_parent.InvalidateCacheRange(cache_line_start, ICACHE_LINE_SIZE);
        break;
    }
    case Dynarmic::A64::InstructionCacheOperation::InvalidateAllToPoU:
        m_parent.ClearInstructionCache();
        break;
    case Dynarmic::A64::InstructionCacheOperation::InvalidateAllToPoUInnerSharable:
    default:
        LOG_DEBUG(Core_ARM, "Unprocesseed instruction cache operation: {}", op);
        break;
    }
    m_parent.m_jit->HaltExecution(Dynarmic::HaltReason::CacheInvalidation);
}

void DynarmicCallbacks64::ExceptionRaised(u64 pc, Dynarmic::A64::Exception exception) {
    switch (exception) {
    case Dynarmic::A64::Exception::WaitForInterrupt:
    case Dynarmic::A64::Exception::WaitForEvent:
    case Dynarmic::A64::Exception::SendEvent:
    case Dynarmic::A64::Exception::SendEventLocal:
    case Dynarmic::A64::Exception::Yield:
        LOG_TRACE(Core_ARM, "ExceptionRaised(exception = {}, pc = {:08X}, code = {:08X}, cached = {:08X})", std::size_t(exception), pc, m_memory.Read32(pc), MemoryReadCode(pc).value_or(0));
        return;
    case Dynarmic::A64::Exception::NoExecuteFault:
        LOG_CRITICAL(Core_ARM, "Cannot execute instruction at unmapped address {:#016x}", pc);
        ReturnException(pc, PrefetchAbort);
        return;
    default:
        if (m_debugger_enabled) {
            ReturnException(pc, InstructionBreakpoint);
        } else {
            m_parent.LogBacktrace(m_process);
            LOG_CRITICAL(Core_ARM, "ExceptionRaised(exception = {}, pc = {:08X}, code = {:08X})", static_cast<std::size_t>(exception), pc, m_memory.Read32(pc));
        }
    }
}

void DynarmicCallbacks64::CallSVC(u32 svc) {
    m_parent.m_svc = svc;
    m_parent.m_jit->HaltExecution(SupervisorCall);
}

void DynarmicCallbacks64::AddTicks(u64 ticks) {
    ASSERT(!m_parent.m_uses_wall_clock && "Dynarmic ticking disabled");
    // Divide the number of ticks by the amount of CPU cores. TODO(Subv): This yields only a
    // rough approximation of the amount of executed ticks in the system, it may be thrown off
    // if not all cores are doing a similar amount of work. Instead of doing this, we should
    // device a way so that timing is consistent across all cores without increasing the ticks 4
    // times.
    u64 amortized_ticks = ticks / Core::Hardware::NUM_CPU_CORES;
    // Always execute at least one tick.
    amortized_ticks = std::max<u64>(amortized_ticks, 1);
    m_parent.m_system.CoreTiming().AddTicks(amortized_ticks);
}

u64 DynarmicCallbacks64::GetTicksRemaining() {
    ASSERT(!m_parent.m_uses_wall_clock && "Dynarmic ticking disabled");
    return std::max<s64>(m_parent.m_system.CoreTiming().downcount, 0);
}

u64 DynarmicCallbacks64::GetCNTPCT() {
    CpuProfile::Add(m_parent.m_core_index, CpuProfile::Counter::ClockReads);
    return m_parent.m_system.CoreTiming().GetClockTicks();
}

bool DynarmicCallbacks64::CheckMemoryAccess(u64 addr, u64 size, Kernel::DebugWatchpointType type) {
    if (!m_check_memory_access) {
        return true;
    }

    if (!m_memory.IsValidVirtualAddressRange(addr, size)) {
        LOG_CRITICAL(Core_ARM, "Stopping execution due to unmapped memory access at {:#x}",
                        addr);
        m_parent.m_jit->HaltExecution(PrefetchAbort);
        return false;
    }

    if (!m_debugger_enabled) {
        return true;
    }

    const auto match{m_parent.MatchingWatchpoint(addr, size, type)};
    if (match) {
        m_parent.m_halted_watchpoint = match;
        m_parent.m_jit->HaltExecution(DataAbort);
        return false;
    }

    return true;
}

void DynarmicCallbacks64::ReturnException(u64 pc, Dynarmic::HaltReason hr) {
    m_parent.GetContext(m_parent.m_breakpoint_context);
    m_parent.m_breakpoint_context.pc = pc;
    m_parent.m_jit->HaltExecution(hr);
}

void ArmDynarmic64::MakeJit(Common::PageTable* page_table, std::size_t address_space_bits) {
    Dynarmic::JitProfile::SetEnabled(CpuProfile::Enabled());
    Dynarmic::A64::UserConfig config;

    // Callbacks
    config.callbacks = std::addressof(*m_cb);

    // Memory
    if (page_table) {
        // Dynarmic will not write to the page table, const_cast is safe here
        config.page_table = reinterpret_cast<void**>(
            const_cast<Common::PageTable::JitPageEntry*>(page_table->jit_entries.data()));
        config.page_table_address_space_bits = std::uint32_t(address_space_bits);
        config.page_table_pointer_mask = 0;
        config.page_table_marked_bit = std::nullopt;
        config.silently_mirror_page_table = false;
        config.absolute_offset_page_table = true;
        config.detect_misaligned_access_via_page_table = 16 | 32 | 64 | 128;
        config.only_detect_misalignment_via_page_table_on_page_boundary = true;

        config.fastmem_pointer = page_table->fastmem_arena ?
            std::optional<uintptr_t>{reinterpret_cast<uintptr_t>(page_table->fastmem_arena)} :
            std::nullopt;
        config.fastmem_address_space_bits = std::uint32_t(address_space_bits);
        config.silently_mirror_fastmem = false;

        config.fastmem_exclusive_access = config.fastmem_pointer != std::nullopt;
        config.recompile_on_exclusive_fastmem_failure = true;

        config.page_table_sign_extension = std::nullopt;
    }

    // Multi-process state
    config.processor_id = std::uint8_t(m_core_index);
    config.global_monitor = &m_exclusive_monitor.monitor;

    // System registers
    config.tpidrro_el0 = &m_cb->m_tpidrro_el0;
    config.tpidr_el0 = &m_cb->m_tpidr_el0;
    config.dczid_el0 = 4;
    config.ctr_el0 = 0x8444c004;
    config.cntfrq_el0 = Hardware::CNTFREQ;

    // Unpredictable instructions
    config.define_unpredictable_behaviour = true;

    // Timing
    config.wall_clock_cntpct = m_uses_wall_clock;
    config.enable_cycle_counting = !m_uses_wall_clock;

    // Code cache size
#if defined(ARCHITECTURE_arm64) || defined(__sun__) || defined(__NetBSD__) || defined(__DragonFly__) || defined(__OpenBSD__)
    config.code_cache_size = std::uint32_t(128_MiB);
#else
    config.code_cache_size = std::uint32_t(512_MiB);
#endif

    // Allow memory fault handling to work
    if (m_system.DebuggerEnabled()) {
        config.check_halt_on_memory_access = true;
    }

    // null_jit
    if (!page_table) {
        // Don't waste too much memory on null_jit
        config.code_cache_size = std::uint32_t(8_MiB);
    }

    switch (Settings::values.cpu_accuracy.GetValue()) {
    // Debug mode
    case Settings::CpuAccuracy::Debugging:
        if (!Settings::values.cpuopt_page_tables) {
            config.page_table = nullptr;
        }
        if (!Settings::values.cpuopt_block_linking) {
            config.optimizations &= ~Dynarmic::OptimizationFlag::BlockLinking;
        }
        if (!Settings::values.cpuopt_return_stack_buffer) {
            config.optimizations &= ~Dynarmic::OptimizationFlag::ReturnStackBuffer;
        }
        if (!Settings::values.cpuopt_fast_dispatcher) {
            config.optimizations &= ~Dynarmic::OptimizationFlag::FastDispatch;
        }
        if (!Settings::values.cpuopt_context_elimination) {
            config.optimizations &= ~Dynarmic::OptimizationFlag::GetSetElimination;
        }
        if (!Settings::values.cpuopt_const_prop) {
            config.optimizations &= ~Dynarmic::OptimizationFlag::ConstProp;
        }
        if (!Settings::values.cpuopt_misc_ir) {
            config.optimizations &= ~Dynarmic::OptimizationFlag::MiscIROpt;
        }
        if (!Settings::values.cpuopt_reduce_misalign_checks) {
            config.only_detect_misalignment_via_page_table_on_page_boundary = false;
        }
        if (!Settings::values.cpuopt_fastmem) {
            config.fastmem_pointer = std::nullopt;
            config.fastmem_exclusive_access = false;
        }
        if (!Settings::values.cpuopt_fastmem_exclusives) {
            config.fastmem_exclusive_access = false;
        }
        if (!Settings::values.cpuopt_recompile_exclusives) {
            config.recompile_on_exclusive_fastmem_failure = false;
        }
        if (!Settings::values.cpuopt_ignore_memory_aborts) {
            config.check_halt_on_memory_access = true;
        }
        break;
    // Unsafe optimizations
    case Settings::CpuAccuracy::Unsafe:
        config.unsafe_optimizations = true;
        if (!Settings::values.cpuopt_unsafe_host_mmu) {
            config.fastmem_pointer = std::nullopt;
            config.fastmem_exclusive_access = false;
        }
        if (Settings::values.cpuopt_unsafe_unfuse_fma) {
            config.optimizations |= Dynarmic::OptimizationFlag::Unsafe_UnfuseFMA;
        }
        if (Settings::values.cpuopt_unsafe_reduce_fp_error) {
            config.optimizations |= Dynarmic::OptimizationFlag::Unsafe_ReducedErrorFP;
        }
        if (Settings::values.cpuopt_unsafe_inaccurate_nan) {
            config.optimizations |= Dynarmic::OptimizationFlag::Unsafe_InaccurateNaN;
        }
        if (Settings::values.cpuopt_unsafe_fastmem_check) {
            config.fastmem_address_space_bits = 64;
        }
        if (Settings::values.cpuopt_unsafe_ignore_global_monitor) {
            config.optimizations |= Dynarmic::OptimizationFlag::Unsafe_IgnoreGlobalMonitor;
        }
        break;
    // Safe optimisations
    case Settings::CpuAccuracy::Auto:
        config.unsafe_optimizations = true;
        config.optimizations |= Dynarmic::OptimizationFlag::Unsafe_UnfuseFMA;
        config.fastmem_address_space_bits = 64;
        config.optimizations |= Dynarmic::OptimizationFlag::Unsafe_IgnoreGlobalMonitor;
        break;
    // Paranoia mode for debugging optimizations
    case Settings::CpuAccuracy::Paranoid:
        config.unsafe_optimizations = false;
        config.optimizations = Dynarmic::no_optimizations;
        break;
    case Settings::CpuAccuracy::Accurate:
    default:
        break;
    }
    if (!Settings::IsFastmemEnabled()) {
        config.fastmem_pointer = std::nullopt;
        config.fastmem_exclusive_access = false;
    }
    m_jit.emplace(config);
}

HaltReason ArmDynarmic64::RunThread(Kernel::KThread* thread) {
    const CpuProfile::RunTimer timer{m_core_index};
    m_jit->ClearExclusiveState();
    return TranslateHaltReason(m_jit->Run());
}

HaltReason ArmDynarmic64::StepThread(Kernel::KThread* thread) {
    m_jit->ClearExclusiveState();
    return TranslateHaltReason(m_jit->Step());
}

u32 ArmDynarmic64::GetSvcNumber() const {
    return m_svc;
}

void ArmDynarmic64::GetSvcArguments(std::span<uint64_t, 8> args) const {
    Dynarmic::A64::Jit const& j = *m_jit;
    for (size_t i = 0; i < 8; i++)
        args[i] = j.GetRegister(i);
}

void ArmDynarmic64::SetSvcArguments(std::span<const uint64_t, 8> args) {
    Dynarmic::A64::Jit& j = *m_jit;
    for (size_t i = 0; i < 8; i++)
        j.SetRegister(i, args[i]);
}

const Kernel::DebugWatchpoint* ArmDynarmic64::HaltedWatchpoint() const {
    return m_halted_watchpoint;
}

void ArmDynarmic64::RewindBreakpointInstruction() {
    this->SetContext(m_breakpoint_context);
}

ArmDynarmic64::ArmDynarmic64(System& system, bool uses_wall_clock, Kernel::KProcess* process, DynarmicExclusiveMonitor& exclusive_monitor, std::size_t core_index)
    : ArmInterface{uses_wall_clock}, m_system{system}, m_exclusive_monitor{exclusive_monitor}
    , m_cb(std::make_optional<DynarmicCallbacks64>(*this, process))
    , m_core_index{core_index}
{
    auto& page_table = process->GetPageTable().GetBasePageTable();
    auto& page_table_impl = page_table.GetImpl();
    MakeJit(&page_table_impl, page_table.GetAddressSpaceWidth());
}

ArmDynarmic64::~ArmDynarmic64() {
#if defined(ARCHITECTURE_x86_64)
    // Kernel finalization has already stopped the CPU owners. Guest memory may
    // be released here: save only the hashes collected during translation.
    if (!m_prewarm || (m_prewarm->observed.empty() && m_prewarm->gameplay_observed.empty())) {
        return;
    }
    try {
        auto& p = *m_prewarm;
        JitPrewarm::Merge(p.loaded, p.observed);
        JitPrewarm::Merge(p.loaded, p.gameplay_observed);
        std::error_code ec;
        std::filesystem::create_directories(p.path.parent_path(), ec);
        if (ec) {
            LOG_WARNING(Core, "JIT profile core {}: directory unavailable: {}", p.core, ec.message());
            return;
        }
        auto temp = p.path;
        temp += ".tmp";
        std::ofstream file{temp, std::ios::binary | std::ios::trunc};
        const bool written = JitPrewarm::Write(file, p.title, p.build, p.core, p.loaded);
        file.flush();
        const bool flushed = bool(file);
        file.close();
        if (!written || !flushed || file.fail()) {
            LOG_WARNING(Core, "JIT profile core {}: write failed, previous profile retained", p.core);
            return;
        }
        // Replace only after a complete close. No delete-before-rename gap.
        std::filesystem::rename(temp, p.path, ec);
        if (ec) {
            LOG_WARNING(Core, "JIT profile core {}: replacement failed: {}", p.core, ec.message());
            return;
        }
        LOG_INFO(Core, "JIT profile core {}: saved {} descriptors, {} observed, {} during T, {} dropped at limit",
                 p.core, p.loaded.size(), p.observed.size(), p.gameplay_observed.size(), p.dropped);
    } catch (const std::exception& e) {
        LOG_WARNING(Core, "JIT profile: save skipped: {}", e.what());
    }
#endif
}

void ArmDynarmic64::LoadPrewarmProfile() {
#if defined(ARCHITECTURE_x86_64)
    if (m_prewarm || m_system.DebuggerEnabled()) {
        return;
    }
    try {
        auto profile = std::make_unique<JitPrewarm::Profile>();
        auto& p = *profile;
        p.title = m_cb->m_process->GetProgramId();
        p.build = m_system.GetApplicationProcessBuildID();
        p.core = static_cast<u32>(m_core_index);
        p.base = GetInteger(m_cb->m_process->GetEntryPoint());
        p.path = Common::FS::GetEdenPath(Common::FS::EdenPath::CacheDir) / "jit-profile" /
                 fmt::format("{:016x}", p.title) / fmt::format("core-{}.bin", p.core);
        auto& table = m_cb->m_process->GetPageTable().GetBasePageTable();
        const auto end = GetInteger(table.GetCodeRegionStart()) + table.GetCodeRegionSize();
        for (u64 addr = GetInteger(table.GetCodeRegionStart()); addr < end;) {
            Kernel::KMemoryInfo info{};
            Kernel::Svc::PageInfo page{};
            if (table.QueryInfo(&info, &page, addr).IsError() || info.m_size == 0 ||
                info.m_address > addr || info.m_size > UINT64_MAX - info.m_address) {
                break;
            }
            const auto next = info.m_address + info.m_size;
            if (next <= addr) {
                break;
            }
            const auto user = info.m_permission & Kernel::KMemoryPermission::UserMask;
            const auto rx = (Kernel::KMemoryPermission::UserRead |
                             Kernel::KMemoryPermission::UserExecute) & Kernel::KMemoryPermission::UserMask;
            if (user == rx) {
                p.executable_ranges.emplace_back(info.m_address, std::min<u64>(next, end));
            }
            addr = next;
        }
        p.observed.reserve(JitPrewarm::MaxRecords - JitPrewarm::GameplayCapacity);
        p.gameplay_observed.reserve(JitPrewarm::GameplayCapacity);
        std::error_code ec;
        const auto bytes = std::filesystem::file_size(p.path, ec);
        if (!ec) {
            std::ifstream file{p.path, std::ios::binary};
            if (!JitPrewarm::Read(file, bytes, p.title, p.build, p.core, p.loaded)) {
                LOG_WARNING(Core, "JIT profile core {}: incompatible or corrupt, learning again", p.core);
            }
        }
        p.status.assign(p.loaded.size(), JitPrewarm::WarmStatus::RecordOnly);
        p.catalog = JitPrewarm::application_catalog;
        if (JitPrewarm::application_catalog) {
            auto& keys = JitPrewarm::application_catalog->descriptors[p.core];
            keys.reserve(p.loaded.size());
            for (const auto& r : p.loaded) {
                keys.push_back(r.descriptor);
                if (r.gameplay_samples != 0) JitPrewarm::application_catalog->priority.push_back(r);
            }
        }
        LOG_INFO(Core, "JIT profile core {}: loaded {}, RX ranges {}", p.core, p.loaded.size(), p.executable_ranges.size());
        m_prewarm = std::move(profile);
        m_jit->SetBlockProfileCallback([this](const Dynarmic::BlockProfile& block) {
            m_prewarm->Observe(block);
        });
    } catch (const std::exception& e) {
        LOG_WARNING(Core, "JIT prewarm core {}: unavailable, using normal JIT: {}", m_core_index, e.what());
    }
#endif
}

size_t ArmDynarmic64::PreparePrewarmCandidates() {
#if defined(ARCHITECTURE_x86_64)
    if (!m_prewarm) return 0;
    try {
        return m_prewarm->PrepareShared();
    } catch (const std::exception& e) {
        m_prewarm->shared.clear();
        m_prewarm->shared_status.clear();
        LOG_WARNING(Core, "JIT profile core {}: shared candidates unavailable: {}", m_core_index, e.what());
        return 0;
    }
#else
    return 0;
#endif
}

void ArmDynarmic64::PrewarmBlocks(const std::function<void(size_t, size_t)>& progress) {
#if defined(ARCHITECTURE_x86_64)
    if (!m_prewarm) return;
    auto& p = *m_prewarm;
    m_jit->SetBlockProfileCallback({}); // Warming must not record its own compilations.
    try {
        const auto plan = p.WarmPlan();
        size_t priority_accepted{}, shared_accepted{};
        const auto start = std::chrono::steady_clock::now();
        size_t accepted{}, rejected{}, budget_skipped{};
        const auto space = m_jit->GetCodeCacheSpaceRemaining();
        constexpr size_t CodeBudget = 115 * 1024 * 1024;
        {
            m_cb->last_code_addr = u64(-1);
            for (size_t n = 0; n < plan.size(); ++n) {
                if (space - m_jit->GetCodeCacheSpaceRemaining() >= CodeBudget) {
                    budget_skipped = plan.size() - n;
                    break;
                }
                const auto candidate = plan[n];
                auto block = p.GetRecord(candidate);
                auto& status = candidate.shared ? p.shared_status[candidate.index] : p.status[candidate.index];
                status = JitPrewarm::WarmStatus::Rejected;
                const auto offset = block.descriptor & JitPrewarm::PcMask;
                if (offset > JitPrewarm::PcMask - p.base ||
                    !p.Contains(p.base + offset, block.code_bytes)) {
                    ++rejected;
                } else {
                    block.descriptor = (block.descriptor & JitPrewarm::DescriptorMask) | (p.base + offset);
                    if (m_jit->PrecompileBlock(block)) {
                        ++accepted;
                        status = JitPrewarm::WarmStatus::Accepted;
                        shared_accepted += candidate.shared;
                        priority_accepted += !candidate.shared && block.gameplay_samples != 0;
                    } else {
                        ++rejected;
                    }
                }
                if (progress && (n % 256 == 0 || n + 1 == plan.size())) {
                    progress(n + 1, plan.size());
                }
            }
            if (progress && !plan.empty()) {
                progress(plan.size(), plan.size());
            }
        }
        LOG_INFO(Core, "JIT prewarm core {}: mode {}, RX ranges {}, loaded {}, accepted {}, rejected {}, budget skipped {}, code {:.2f} MiB, {:.1f} ms",
                 p.core, "warm", p.executable_ranges.size(), p.loaded.size(), accepted, rejected, budget_skipped,
                 (space - m_jit->GetCodeCacheSpaceRemaining()) / (1024.0 * 1024.0),
                 std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
        LOG_INFO(Core, "JIT prewarm core {}: priority blocks {}, accepted {}", p.core,
                 std::count_if(p.loaded.begin(), p.loaded.end(), [](const auto& r) { return r.gameplay_samples != 0; }),
                 priority_accepted);
        LOG_INFO(Core, "JIT prewarm core {}: shared candidates {}, accepted {}", p.core, p.shared.size(), shared_accepted);
    } catch (const std::exception& e) {
        LOG_WARNING(Core, "JIT prewarm core {}: incomplete, using normal JIT: {}", m_core_index, e.what());
    }
    m_jit->SetBlockProfileCallback([this](const Dynarmic::BlockProfile& block) {
        m_prewarm->Observe(block);
    });
#else
    (void)progress;
#endif
}

void ArmDynarmic64::SetTpidrroEl0(u64 value) {
    m_cb->m_tpidrro_el0 = value;
}

void ArmDynarmic64::GetContext(Kernel::Svc::ThreadContext& ctx) const {
    Dynarmic::A64::Jit const& j = *m_jit;
    auto gpr = j.GetRegisters();
    auto fpr = j.GetVectors();
    // TODO: this is inconvenient
    for (size_t i = 0; i < 29; i++)
        ctx.r[i] = gpr[i];
    ctx.fp = gpr[29];
    ctx.lr = gpr[30];
    ctx.sp = j.GetSP();
    ctx.pc = j.GetPC();
    ctx.pstate = j.GetPstate();
    ctx.v = fpr;
    ctx.fpcr = j.GetFpcr();
    ctx.fpsr = j.GetFpsr();
    ctx.tpidr = m_cb->m_tpidr_el0;
}

void ArmDynarmic64::SetContext(const Kernel::Svc::ThreadContext& ctx) {
    Dynarmic::A64::Jit& j = *m_jit;
    // TODO: this is inconvenient
    std::array<u64, 31> gpr;
    for (size_t i = 0; i < 29; i++)
        gpr[i] = ctx.r[i];
    gpr[29] = ctx.fp;
    gpr[30] = ctx.lr;
    j.SetRegisters(gpr);
    j.SetSP(ctx.sp);
    j.SetPC(ctx.pc);
    j.SetPstate(ctx.pstate);
    j.SetVectors(ctx.v);
    j.SetFpcr(ctx.fpcr);
    j.SetFpsr(ctx.fpsr);
    m_cb->m_tpidr_el0 = ctx.tpidr;
}

void ArmDynarmic64::SignalInterrupt(Kernel::KThread* thread) {
    m_jit->HaltExecution(BreakLoop);
}

void ArmDynarmic64::ClearInstructionCache() {
    m_cb->last_code_addr = u64(-1);
    m_jit->ClearCache();
}

void ArmDynarmic64::InvalidateCacheRange(u64 addr, std::size_t size) {
    m_jit->InvalidateCacheRange(addr, size);
}

} // namespace Core
