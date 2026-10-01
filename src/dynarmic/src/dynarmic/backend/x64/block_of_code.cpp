// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2016 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include "dynarmic/backend/x64/block_of_code.h"

#ifdef _WIN32
#    define WIN32_LEAN_AND_MEAN
#    include <windows.h>
// On the Xbox/UWP AppContainer the plain Virtual* APIs are unavailable for JIT memory; the
// sandbox-legal *FromApp variants must be used instead, and emitting executable pages additionally
// requires the `codeGeneration` restricted capability in the package manifest. The function shapes
// are identical, so we just select the symbol. RWX is never grantable under the AppContainer, so
// DYNARMIC_UWP_APPCONTAINER always implies DYNARMIC_ENABLE_NO_EXECUTE_SUPPORT (W^X) — see CMakeLists.
#    if defined(DYNARMIC_UWP_APPCONTAINER)
#        define DYNARMIC_VIRTUAL_ALLOC   VirtualAllocFromApp
#        define DYNARMIC_VIRTUAL_PROTECT VirtualProtectFromApp
// A failed protect or commit here does not fail at the call: it surfaces later as a fault inside
// JIT code, where the process dies without reaching any unhandled-exception filter. Raise at the
// cause instead, with a code the UWP frontend's first-chance logger decodes:
//   Information[0] = GetLastError(), [1] = address, [2] = size, [3] = requested protection
#        define DYNARMIC_UWP_PROTECT_FAILED 0xE0DA0001u
#        define DYNARMIC_UWP_COMMIT_FAILED  0xE0DA0002u
static void RaiseJitMemoryFailure(DWORD code, const void* base, size_t size, DWORD protection) {
    const ULONG_PTR info[4] = {GetLastError(), reinterpret_cast<ULONG_PTR>(base), size, protection};
    RaiseException(code, EXCEPTION_NONCONTINUABLE, 4, info);
}
#    else
#        define DYNARMIC_VIRTUAL_ALLOC   VirtualAlloc
#        define DYNARMIC_VIRTUAL_PROTECT VirtualProtect
#    endif
#else
#    include <sys/mman.h>
#endif

#ifdef __APPLE__
#    include <errno.h>
#    include <fmt/format.h>
#    include <sys/sysctl.h>
#endif

#include <algorithm>
#include <atomic>
#include <array>
#include <cstdint>
#include <cstring>

#include "common/assert.h"
#include "dynarmic/mcl/bit.hpp"
#include "dynarmic/backend/x64/xbyak.h"

#include "dynarmic/backend/x64/a32_jitstate.h"
#include "dynarmic/backend/x64/abi.h"
#include "dynarmic/backend/x64/hostloc.h"
#include "dynarmic/backend/x64/perf_map.h"
#include "dynarmic/backend/x64/stack_layout.h"
#include "dynarmic/backend/x64/writable_code_ranges.h"
#include "dynarmic/interface/jit_profile.h"

namespace Dynarmic::Backend::X64 {

const Xbyak::Reg64 BlockOfCode::ABI_JIT_PTR = HostLocToReg64(Dynarmic::Backend::X64::ABI_JIT_PTR);
#ifdef _WIN32
const Xbyak::Reg64 BlockOfCode::ABI_RETURN = HostLocToReg64(Dynarmic::Backend::X64::ABI_RETURN);
const Xbyak::Reg64 BlockOfCode::ABI_PARAM1 = HostLocToReg64(Dynarmic::Backend::X64::ABI_PARAM1);
const Xbyak::Reg64 BlockOfCode::ABI_PARAM2 = HostLocToReg64(Dynarmic::Backend::X64::ABI_PARAM2);
const Xbyak::Reg64 BlockOfCode::ABI_PARAM3 = HostLocToReg64(Dynarmic::Backend::X64::ABI_PARAM3);
const Xbyak::Reg64 BlockOfCode::ABI_PARAM4 = HostLocToReg64(Dynarmic::Backend::X64::ABI_PARAM4);
const std::array<Xbyak::Reg64, ABI_PARAM_COUNT> BlockOfCode::ABI_PARAMS = {BlockOfCode::ABI_PARAM1, BlockOfCode::ABI_PARAM2, BlockOfCode::ABI_PARAM3, BlockOfCode::ABI_PARAM4};
#else
const Xbyak::Reg64 BlockOfCode::ABI_RETURN = HostLocToReg64(Dynarmic::Backend::X64::ABI_RETURN);
const Xbyak::Reg64 BlockOfCode::ABI_RETURN2 = HostLocToReg64(Dynarmic::Backend::X64::ABI_RETURN2);
const Xbyak::Reg64 BlockOfCode::ABI_PARAM1 = HostLocToReg64(Dynarmic::Backend::X64::ABI_PARAM1);
const Xbyak::Reg64 BlockOfCode::ABI_PARAM2 = HostLocToReg64(Dynarmic::Backend::X64::ABI_PARAM2);
const Xbyak::Reg64 BlockOfCode::ABI_PARAM3 = HostLocToReg64(Dynarmic::Backend::X64::ABI_PARAM3);
const Xbyak::Reg64 BlockOfCode::ABI_PARAM4 = HostLocToReg64(Dynarmic::Backend::X64::ABI_PARAM4);
const Xbyak::Reg64 BlockOfCode::ABI_PARAM5 = HostLocToReg64(Dynarmic::Backend::X64::ABI_PARAM5);
const Xbyak::Reg64 BlockOfCode::ABI_PARAM6 = HostLocToReg64(Dynarmic::Backend::X64::ABI_PARAM6);
const std::array<Xbyak::Reg64, ABI_PARAM_COUNT> BlockOfCode::ABI_PARAMS = {BlockOfCode::ABI_PARAM1, BlockOfCode::ABI_PARAM2, BlockOfCode::ABI_PARAM3, BlockOfCode::ABI_PARAM4, BlockOfCode::ABI_PARAM5, BlockOfCode::ABI_PARAM6};
#endif

namespace {

constexpr size_t CONSTANT_POOL_SIZE = 2 * 1024 * 1024;
constexpr size_t PRELUDE_COMMIT_SIZE = 16 * 1024 * 1024;

#ifdef _WIN32
// Reserve-only allocator for the code region: EnsureMemoryCommitted() commits pages on demand, so
// the process is only charged for code actually emitted. Eden upstream switched to Xbyak's default
// allocator; the Xbox UWP AppContainer needs this one, both for the *FromApp APIs and to stay within
// the Game-mode commit limit.
class CustomXbyakAllocator : public Xbyak::Allocator {
public:
    uint8_t* alloc(size_t size) override {
        void* p = DYNARMIC_VIRTUAL_ALLOC(nullptr, size, MEM_RESERVE, PAGE_READWRITE);
        if (p == nullptr) {
            using Xbyak::Error;
            XBYAK_THROW(Xbyak::ERR_CANT_ALLOC);
        }
        return static_cast<uint8_t*>(p);
    }

    void free(uint8_t* p) override {
        VirtualFree(static_cast<void*>(p), 0, MEM_RELEASE);
    }

    bool useProtect() const override { return false; }
};

// This is threadsafe as Xbyak::Allocator does not contain any state; it is a pure interface.
CustomXbyakAllocator s_allocator;
#endif

#if defined(_WIN32) && defined(DYNARMIC_ENABLE_NO_EXECUTE_SUPPORT)
constexpr size_t WX_PAGE_SIZE = 4096;

const u8* WxPageDown(const u8* ptr) {
    return reinterpret_cast<const u8*>(reinterpret_cast<uintptr_t>(ptr) & ~(WX_PAGE_SIZE - 1));
}

const u8* WxPageUp(const u8* ptr) {
    return WxPageDown(ptr + WX_PAGE_SIZE - 1);
}
#endif

#ifdef DYNARMIC_ENABLE_NO_EXECUTE_SUPPORT
void ProtectMemory(const void* base, size_t size, bool is_executable,
                   [[maybe_unused]] bool preserve_cfg = false) {
    // The constructor enables writing before anything is committed (committed_size == 0); there is
    // nothing to protect yet, and a zero-sized protect is an error on the UWP path.
    if (size == 0) {
        return;
    }
#    ifdef _WIN32
    const JitProfile::Timer timer{JitProfile::Phase::Protect};
    DWORD oldProtect = 0;
    // The is_executable→PAGE_EXECUTE_READ transition is the call that requires the `codeGeneration`
    // capability under the AppContainer (VirtualProtectFromApp); the W^X invariant means we only ever
    // hold RW or RX, never RWX.
    static std::atomic_bool preserve_cfg_supported{true};
    const bool keep_targets = is_executable && preserve_cfg &&
                              preserve_cfg_supported.load(std::memory_order_relaxed);
    DWORD protection = is_executable ? PAGE_EXECUTE_READ : PAGE_READWRITE;
    if (keep_targets) {
        protection |= PAGE_TARGETS_NO_UPDATE;
    }
    BOOL protected_ok;
    if (is_executable) {
        const JitProfile::Timer rx_timer{keep_targets ? JitProfile::Phase::CfgPreserveRx
                                                    : JitProfile::Phase::CfgInitializeRx};
        protected_ok = DYNARMIC_VIRTUAL_PROTECT(const_cast<void*>(base), size, protection, &oldProtect);
    } else {
        protected_ok = DYNARMIC_VIRTUAL_PROTECT(const_cast<void*>(base), size, protection, &oldProtect);
    }
    if (!protected_ok && keep_targets) {
        const DWORD error = GetLastError();
        if (error == ERROR_INVALID_PARAMETER || error == ERROR_NOT_SUPPORTED) {
            // Some AppContainer runtimes may reject the modifier. Keep the established
            // RX path and stop retrying the unsupported flag on subsequent blocks.
            preserve_cfg_supported.store(false, std::memory_order_relaxed);
            protection = PAGE_EXECUTE_READ;
            const JitProfile::Timer fallback_timer{JitProfile::Phase::CfgFallback};
            protected_ok = DYNARMIC_VIRTUAL_PROTECT(const_cast<void*>(base), size, protection,
                                                    &oldProtect);
        }
    }
#        if defined(DYNARMIC_UWP_APPCONTAINER)
    if (!protected_ok) {
        RaiseJitMemoryFailure(DYNARMIC_UWP_PROTECT_FAILED, base, size, protection);
    }
#        else
    (void)protected_ok;
#        endif
#    else
    static const size_t pageSize = sysconf(_SC_PAGESIZE);
    const size_t iaddr = reinterpret_cast<size_t>(base);
    const size_t roundAddr = iaddr & ~(pageSize - static_cast<size_t>(1));
    const int mode = is_executable ? (PROT_READ | PROT_EXEC) : (PROT_READ | PROT_WRITE);
    mprotect(reinterpret_cast<void*>(roundAddr), size + (iaddr - roundAddr), mode);
#    endif
}
#endif

static const HostFeature features = []() {
    HostFeature f{};
#ifdef DYNARMIC_ENABLE_CPU_FEATURE_DETECTION
    using Cpu = Xbyak::util::Cpu;
    Xbyak::util::Cpu cpu_info{};
    if (cpu_info.has(Cpu::tSSSE3)) f |= HostFeature::SSSE3;
    if (cpu_info.has(Cpu::tSSE41)) f |= HostFeature::SSE41;
    if (cpu_info.has(Cpu::tSSE42)) f |= HostFeature::SSE42;
    if (cpu_info.has(Cpu::tAVX)) f |= HostFeature::AVX;
    if (cpu_info.has(Cpu::tAVX2)) f |= HostFeature::AVX2;
    if (cpu_info.has(Cpu::tAVX512F)) f |= HostFeature::AVX512F;
    if (cpu_info.has(Cpu::tAVX512CD)) f |= HostFeature::AVX512CD;
    if (cpu_info.has(Cpu::tAVX512VL)) f |= HostFeature::AVX512VL;
    if (cpu_info.has(Cpu::tAVX512BW)) f |= HostFeature::AVX512BW;
    if (cpu_info.has(Cpu::tAVX512DQ)) f |= HostFeature::AVX512DQ;
    if (cpu_info.has(Cpu::tAVX512_BITALG)) f |= HostFeature::AVX512BITALG;
    if (cpu_info.has(Cpu::tAVX512VBMI)) f |= HostFeature::AVX512VBMI;
    if (cpu_info.has(Cpu::tPCLMULQDQ)) f |= HostFeature::PCLMULQDQ;
    if (cpu_info.has(Cpu::tF16C)) f |= HostFeature::F16C;
    if (cpu_info.has(Cpu::tFMA)) f |= HostFeature::FMA;
    if (cpu_info.has(Cpu::tAESNI)) f |= HostFeature::AES;
    if (cpu_info.has(Cpu::tSHA)) f |= HostFeature::SHA;
    if (cpu_info.has(Cpu::tPOPCNT)) f |= HostFeature::POPCNT;
    if (cpu_info.has(Cpu::tBMI1)) f |= HostFeature::BMI1;
    if (cpu_info.has(Cpu::tBMI2)) f |= HostFeature::BMI2;
    if (cpu_info.has(Cpu::tLZCNT)) f |= HostFeature::LZCNT;
    if (cpu_info.has(Cpu::tGFNI)) f |= HostFeature::GFNI;
    if (cpu_info.has(Cpu::tWAITPKG)) f |= HostFeature::WAITPKG;
    if (cpu_info.has(Cpu::tBMI2)) {
        // BMI2 instructions such as pdep and pext have been very slow up until Zen 3.
        // Check for Zen 3 or newer by its family (0x19).
        // See also: https://en.wikichip.org/wiki/amd/cpuid
        if (cpu_info.has(Cpu::tAMD)) {
            std::array<u32, 4> data{};
            cpu_info.getCpuid(1, data.data());
            const u32 family_base = mcl::bit::get_bits<8, 11>(data[0]);
            const u32 family_extended = mcl::bit::get_bits<20, 27>(data[0]);
            const u32 family = family_base + family_extended;
            if (family >= 0x19)
                f |= HostFeature::FastBMI2;
        } else {
            f |= HostFeature::FastBMI2;
        }
    }
    return f;
#endif
}();
HostFeature GetHostFeatures() {
    return features;
}

#ifdef __APPLE__
bool IsUnderRosetta() {
    int result = 0;
    size_t result_size = sizeof(result);
    if (sysctlbyname("sysctl.proc_translated", &result, &result_size, nullptr, 0) == -1) {
        if (errno != ENOENT)
            fmt::print("IsUnderRosetta: Failed to detect Rosetta state, assuming not under Rosetta");
        return false;
    }
    return result != 0;
}
#endif

}  // anonymous namespace

BlockOfCode::BlockOfCode(RunCodeCallbacks cb, JitStateInfo jsi, size_t total_code_size, std::function<void(BlockOfCode&)> rcp)
    : Xbyak::CodeGenerator(total_code_size
#ifdef DYNARMIC_ENABLE_NO_EXECUTE_SUPPORT
        , Xbyak::DontSetProtectRWE
#else
        , nullptr //Allow RWE
#endif
#ifdef _WIN32
        , &s_allocator)
#else
        , nullptr)
#endif
    , constant_pool(*this, CONSTANT_POOL_SIZE)
    , jsi(jsi)
    , cb(std::move(cb))
{
    EnableWriting();
    EnsureMemoryCommitted(PRELUDE_COMMIT_SIZE);
#if defined(_WIN32) && defined(DYNARMIC_ENABLE_NO_EXECUTE_SUPPORT)
    // Code starts on a page of its own, so the constant pool pages below never need execute.
    align(WX_PAGE_SIZE);
    wx_exec_begin = getCurr<const u8*>();
#endif
    GenRunCode(rcp);
}

bool BlockOfCode::HasHostFeature(HostFeature feature) const noexcept {
    return (GetHostFeatures() & feature) == feature;
}

void BlockOfCode::PreludeComplete() {
    prelude_complete = true;
    code_begin = getCurr();
    ClearCache();
#if defined(_WIN32) && defined(DYNARMIC_ENABLE_NO_EXECUTE_SUPPORT)
    wx_exec_end = WxPageUp(static_cast<const u8*>(code_begin));
    ProtectMemory(wx_exec_begin, wx_exec_end - wx_exec_begin, true);
#else
    DisableWriting();
#endif
}

// Paged W^X on Windows. Flipping the whole committed code region on every block, as the other
// platforms do, costs two VirtualProtect calls over tens to hundreds of MiB per compiled block; on
// the UWP build it took ~90% of a guest core while a game booted. Only the pages actually written
// change instead: the page the next block starts in (shared with the end of the previous one),
// patch sites via MakeWritable, and nothing else, since everything past the emitted code is still
// RW. Each JIT instance has its own BlockOfCode, used from one thread, so a page is never RW while
// code in it can run.
void BlockOfCode::EnableWriting() {
#ifdef DYNARMIC_ENABLE_NO_EXECUTE_SUPPORT
#    ifdef _WIN32
    if (!prelude_complete || wx_writing) {
        return; // before PreludeComplete everything committed is still RW
    }
    wx_writing = true;
    wx_write_begin = std::max(WxPageDown(getCurr<const u8*>()), wx_exec_begin);
    if (wx_write_begin < wx_exec_end) {
        ProtectMemory(wx_write_begin, wx_exec_end - wx_write_begin, false);
    }
#    else
    ProtectMemory(getCode(), maxSize_, false);
#    endif
#endif
}

void BlockOfCode::DisableWriting() {
#ifdef DYNARMIC_ENABLE_NO_EXECUTE_SUPPORT
#    ifdef _WIN32
    if (!prelude_complete || !wx_writing) {
        return;
    }
    wx_writing = false;
    const u8* const write_end = WxPageUp(getCurr<const u8*>());
    // Return adjacent patch pages and the append window to RX in one syscall per
    // contiguous range. Every included page was already RW; gaps remain untouched.
    std::sort(wx_extra_pages.begin(), wx_extra_pages.end());
    RestoreWritableCodeRanges<u8>(wx_extra_pages, wx_write_begin, write_end, WX_PAGE_SIZE,
                                  [this](const u8* begin, size_t size) {
                                      const u8* const end = begin + size;
                                      // New pages must establish normal CFG call targets once.
                                      // Only previously executable pages may preserve that bitmap.
                                      const u8* const initialized_end = std::min(end, wx_exec_end);
                                      if (begin < initialized_end) {
                                          ProtectMemory(begin, initialized_end - begin, true, true);
                                      }
                                      const u8* const new_begin = std::max(begin, wx_exec_end);
                                      if (new_begin < end) {
                                          ProtectMemory(new_begin, end - new_begin, true);
                                      }
                                  });
    wx_exec_end = std::max(wx_exec_end, write_end);
    wx_extra_pages.clear();
#    else
    ProtectMemory(getCode(), maxSize_, true);
#    endif
#endif
}

void BlockOfCode::MakeWritable([[maybe_unused]] CodePtr where, [[maybe_unused]] size_t size) {
#if defined(_WIN32) && defined(DYNARMIC_ENABLE_NO_EXECUTE_SUPPORT)
    if (!prelude_complete || !wx_writing) {
        return;
    }
    const u8* const begin = static_cast<const u8*>(where);
    for (const u8* page = WxPageDown(begin); page < begin + size; page += WX_PAGE_SIZE) {
        // Pages from the write window on are RW already.
        if (page < wx_exec_begin || page >= wx_write_begin ||
            std::find(wx_extra_pages.begin(), wx_extra_pages.end(), page) != wx_extra_pages.end()) {
            continue;
        }
        ProtectMemory(page, WX_PAGE_SIZE, false);
        wx_extra_pages.push_back(page);
    }
#endif
}

void BlockOfCode::ClearCache() {
    ASSERT(prelude_complete);
    SetCodePtr(code_begin);
#if defined(_WIN32) && defined(DYNARMIC_ENABLE_NO_EXECUTE_SUPPORT)
    // Code is emitted from code_begin again: hand the old blocks' pages back to the RW tail.
    const u8* const tail = WxPageUp(static_cast<const u8*>(code_begin));
    if (wx_exec_end != nullptr && tail < wx_exec_end) {
        ProtectMemory(tail, wx_exec_end - tail, false);
        wx_exec_end = tail;
    }
#endif
}

size_t BlockOfCode::SpaceRemaining() const {
    ASSERT(prelude_complete);
    const u8* current_ptr = getCurr<const u8*>();
    if (current_ptr >= &top_[maxSize_])
        return 0;
    return &top_[maxSize_] - current_ptr;
}

void BlockOfCode::EnsureMemoryCommitted([[maybe_unused]] size_t codesize) {
#ifdef _WIN32
    if (committed_size < size_ + codesize) {
        [[maybe_unused]] const size_t old_committed_size = committed_size;
        committed_size = std::min<size_t>(maxSize_, committed_size + codesize);
#    ifdef DYNARMIC_ENABLE_NO_EXECUTE_SUPPORT
        // W^X: commit read-write only; ProtectMemory() flips pages to RX before execution. Only
        // the pages not committed yet: committing again the ones holding emitted code would make
        // them RW.
        const u8* const commit_begin = WxPageUp(top_ + old_committed_size);
        const u8* const commit_end = WxPageUp(top_ + committed_size);
        void* const committed = commit_begin >= commit_end
            ? top_
            : DYNARMIC_VIRTUAL_ALLOC(const_cast<u8*>(commit_begin), commit_end - commit_begin,
                                     MEM_COMMIT, PAGE_READWRITE);
#        if defined(DYNARMIC_UWP_APPCONTAINER)
        if (committed == nullptr) {
            RaiseJitMemoryFailure(DYNARMIC_UWP_COMMIT_FAILED, commit_begin, commit_end - commit_begin,
                                  PAGE_READWRITE);
        }
#        else
        (void)committed;
#        endif
#    else
        // RWX fast path — desktop only. Never compiled under DYNARMIC_UWP_APPCONTAINER, which
        // forces DYNARMIC_ENABLE_NO_EXECUTE_SUPPORT on (the AppContainer never grants RWX).
        VirtualAlloc(top_, committed_size, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
#    endif
    }
#endif
}

HaltReason BlockOfCode::RunCode(void* jit_state, CodePtr code_ptr) const {
    return run_code(jit_state, code_ptr);
}

HaltReason BlockOfCode::StepCode(void* jit_state, CodePtr code_ptr) const {
    return step_code(jit_state, code_ptr);
}

void BlockOfCode::ReturnFromRunCode(bool mxcsr_already_exited) {
    size_t index = 0;
    if (mxcsr_already_exited)
        index |= MXCSR_ALREADY_EXITED;
    jmp(return_from_run_code[index]);
}

void BlockOfCode::ForceReturnFromRunCode(bool mxcsr_already_exited) {
    size_t index = FORCE_RETURN;
    if (mxcsr_already_exited)
        index |= MXCSR_ALREADY_EXITED;
    jmp(return_from_run_code[index]);
}

void BlockOfCode::GenRunCode(std::function<void(BlockOfCode&)> rcp) {
    Xbyak::Label return_to_caller, return_to_caller_mxcsr_already_exited;

    align();
    run_code = getCurr<RunCodeFuncType>();

    // This serves two purposes:
    // 1. It saves all the registers we as a callee need to save.
    // 2. It aligns the stack so that the code the JIT emits can assume
    //    that the stack is appropriately aligned for CALLs.
    ABI_PushCalleeSaveRegistersAndAdjustStack(*this, sizeof(StackLayout));

    mov(ABI_JIT_PTR, ABI_PARAM1);
    mov(rbx, ABI_PARAM2); // save temporarily in non-volatile register

    if (cb.enable_cycle_counting) {
        cb.GetTicksRemaining.EmitCall(*this);
        mov(qword[rsp + ABI_SHADOW_SPACE + offsetof(StackLayout, cycles_to_run)], ABI_RETURN);
        mov(qword[rsp + ABI_SHADOW_SPACE + offsetof(StackLayout, cycles_remaining)], ABI_RETURN);
    }

    // r14 = page table
    // r13 = fastmem pointer
    rcp(*this);

    cmp(dword[ABI_JIT_PTR + jsi.offsetof_halt_reason], 0);
    jne(return_to_caller_mxcsr_already_exited, T_NEAR);

    SwitchMxcsrOnEntry();
    jmp(rbx);

    align();
    step_code = getCurr<RunCodeFuncType>();

    ABI_PushCalleeSaveRegistersAndAdjustStack(*this, sizeof(StackLayout));

    mov(ABI_JIT_PTR, ABI_PARAM1);

    if (cb.enable_cycle_counting) {
        mov(qword[rsp + ABI_SHADOW_SPACE + offsetof(StackLayout, cycles_to_run)], 1);
        mov(qword[rsp + ABI_SHADOW_SPACE + offsetof(StackLayout, cycles_remaining)], 1);
    }

    rcp(*this);

    cmp(dword[ABI_JIT_PTR + jsi.offsetof_halt_reason], 0);
    jne(return_to_caller_mxcsr_already_exited, T_NEAR);
    lock(); or_(dword[ABI_JIT_PTR + jsi.offsetof_halt_reason], u32(HaltReason::Step));

    SwitchMxcsrOnEntry();
    jmp(ABI_PARAM2);

    // Dispatcher loop

    align();
    return_from_run_code[0] = getCurr<const void*>();

    cmp(dword[ABI_JIT_PTR + jsi.offsetof_halt_reason], 0);
    jne(return_to_caller);
    if (cb.enable_cycle_counting) {
        cmp(qword[rsp + ABI_SHADOW_SPACE + offsetof(StackLayout, cycles_remaining)], 0);
        jng(return_to_caller);
    }
    cb.LookupBlock.EmitCall(*this);
    jmp(ABI_RETURN);

    align();
    return_from_run_code[MXCSR_ALREADY_EXITED] = getCurr<const void*>();

    cmp(dword[ABI_JIT_PTR + jsi.offsetof_halt_reason], 0);
    jne(return_to_caller_mxcsr_already_exited);
    if (cb.enable_cycle_counting) {
        cmp(qword[rsp + ABI_SHADOW_SPACE + offsetof(StackLayout, cycles_remaining)], 0);
        jng(return_to_caller_mxcsr_already_exited);
    }
    SwitchMxcsrOnEntry();
    cb.LookupBlock.EmitCall(*this);
    jmp(ABI_RETURN);

    align();
    return_from_run_code[FORCE_RETURN] = getCurr<const void*>();
    L(return_to_caller);

    SwitchMxcsrOnExit();
    // fallthrough

    return_from_run_code[MXCSR_ALREADY_EXITED | FORCE_RETURN] = getCurr<const void*>();
    L(return_to_caller_mxcsr_already_exited);

    if (cb.enable_cycle_counting) {
        cb.AddTicks.EmitCall(*this, [this](RegList param) {
            mov(param[0], qword[rsp + ABI_SHADOW_SPACE + offsetof(StackLayout, cycles_to_run)]);
            sub(param[0], qword[rsp + ABI_SHADOW_SPACE + offsetof(StackLayout, cycles_remaining)]);
        });
    }

    xor_(eax, eax);
    xchg(dword[ABI_JIT_PTR + jsi.offsetof_halt_reason], eax);

    ABI_PopCalleeSaveRegistersAndAdjustStack(*this, sizeof(StackLayout));
    ret();

    PerfMapRegister(run_code, getCurr(), "dynarmic_dispatcher");
}

void BlockOfCode::SwitchMxcsrOnEntry() {
    stmxcsr(dword[rsp + ABI_SHADOW_SPACE + offsetof(StackLayout, save_host_MXCSR)]);
    ldmxcsr(dword[ABI_JIT_PTR + jsi.offsetof_guest_MXCSR]);
}

void BlockOfCode::SwitchMxcsrOnExit() {
    stmxcsr(dword[ABI_JIT_PTR + jsi.offsetof_guest_MXCSR]);
    ldmxcsr(dword[rsp + ABI_SHADOW_SPACE + offsetof(StackLayout, save_host_MXCSR)]);
}

void BlockOfCode::EnterStandardASIMD() {
    stmxcsr(dword[ABI_JIT_PTR + jsi.offsetof_guest_MXCSR]);
    ldmxcsr(dword[ABI_JIT_PTR + jsi.offsetof_asimd_MXCSR]);
}

void BlockOfCode::LeaveStandardASIMD() {
    stmxcsr(dword[ABI_JIT_PTR + jsi.offsetof_asimd_MXCSR]);
    ldmxcsr(dword[ABI_JIT_PTR + jsi.offsetof_guest_MXCSR]);
}

void BlockOfCode::UpdateTicks() {
    if (!cb.enable_cycle_counting) {
        return;
    }

    cb.AddTicks.EmitCall(*this, [this](RegList param) {
        mov(param[0], qword[rsp + ABI_SHADOW_SPACE + offsetof(StackLayout, cycles_to_run)]);
        sub(param[0], qword[rsp + ABI_SHADOW_SPACE + offsetof(StackLayout, cycles_remaining)]);
    });

    cb.GetTicksRemaining.EmitCall(*this);
    mov(qword[rsp + ABI_SHADOW_SPACE + offsetof(StackLayout, cycles_to_run)], ABI_RETURN);
    mov(qword[rsp + ABI_SHADOW_SPACE + offsetof(StackLayout, cycles_remaining)], ABI_RETURN);
}

void BlockOfCode::LookupBlock() {
    cb.LookupBlock.EmitCall(*this);
}

void BlockOfCode::LoadRequiredFlagsForCondFromRax(IR::Cond cond) {
#ifdef __APPLE__
    static const bool is_rosetta = IsUnderRosetta();
#endif

    // sahf restores SF, ZF, CF
    // add al, 0x7F restores OF

    switch (cond) {
    case IR::Cond::EQ:  // z
    case IR::Cond::NE:  // !z
    case IR::Cond::CS:  // c
    case IR::Cond::CC:  // !c
    case IR::Cond::MI:  // n
    case IR::Cond::PL:  // !n
        sahf();
        break;
    case IR::Cond::VS:  // v
    case IR::Cond::VC:  // !v
        cmp(al, 0x81);
        break;
    case IR::Cond::HI:  // c & !z
    case IR::Cond::LS:  // !c | z
        sahf();
        cmc();
        break;
    case IR::Cond::GE:  // n == v
    case IR::Cond::LT:  // n != v
    case IR::Cond::GT:  // !z & (n == v)
    case IR::Cond::LE:  // z | (n != v)
#ifdef __APPLE__
        if (is_rosetta) {
            shl(al, 3);
            xchg(al, ah);
            push(rax);
            popf();
            break;
        }
#endif
        cmp(al, 0x81);
        sahf();
        break;
    case IR::Cond::AL:
    case IR::Cond::NV:
        break;
    default:
        UNREACHABLE();
    }
}

Xbyak::Address BlockOfCode::Const(const Xbyak::AddressFrame& frame, u64 lower, u64 upper) {
    return constant_pool.GetConstant(*this, frame, lower, upper);
}

CodePtr BlockOfCode::GetCodeBegin() const {
    return code_begin;
}

size_t BlockOfCode::GetTotalCodeSize() const {
    return maxSize_;
}

void* BlockOfCode::AllocateFromCodeSpace(size_t alloc_size) {
    if (size_ + alloc_size >= maxSize_) {
        using Xbyak::Error;
        XBYAK_THROW(Xbyak::ERR_CODE_IS_TOO_BIG);
    }

    EnsureMemoryCommitted(alloc_size);

    void* ret = getCurr<void*>();
    size_ += alloc_size;
    memset(ret, 0, alloc_size);
    return ret;
}

void BlockOfCode::SetCodePtr(CodePtr code_ptr) {
    // The "size" defines where top_, the insertion point, is.
    size_t required_size = reinterpret_cast<const u8*>(code_ptr) - getCode();
    setSize(required_size);
}

void BlockOfCode::EnsurePatchLocationSize(CodePtr begin, size_t size) {
    size_t current_size = getCurr<const u8*>() - reinterpret_cast<const u8*>(begin);
    ASSERT(current_size <= size);
    nop(size - current_size);
}

}  // namespace Dynarmic::Backend::X64
