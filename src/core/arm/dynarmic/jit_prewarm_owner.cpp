// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <exception>
#include <fstream>
#include <system_error>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/settings.h"
#include "core/arm/arm_interface.h"
#include "core/arm/dynarmic/jit_prewarm_owner.h"
#include "core/arm/jit_prewarm.h"
#include "core/core.h"
#include "core/hardware_properties.h"
#include "core/hle/kernel/k_process.h"

namespace Core {

void ConfigureApplicationPrewarm(System& system, bool warm,
                                const std::function<void(std::size_t, std::size_t)>& progress,
                                std::size_t code_budget) {
    auto* process = system.ApplicationProcess();
    if (!process || Settings::IsNceEnabled()) {
        return;
    }
    const auto& layout = process->Is64Bit() ? JitPrewarm::A64Layout : JitPrewarm::A32Layout;
    try {
        JitPrewarm::application_catalog = std::make_shared<JitPrewarm::Catalog>(layout);
    } catch (const std::exception& e) {
        LOG_WARNING(Core, "JIT profile {}: cross-core index unavailable: {}", layout.name, e.what());
    }
    std::array<JitPrewarm::Owner*, Hardware::NUM_CPU_CORES> owners{};
    for (std::size_t core = 0; core < owners.size(); ++core) {
        if (auto* arm = process->GetArmInterface(core)) owners[core] = arm->LoadPrewarmProfile();
    }
    if (JitPrewarm::application_catalog) {
        try {
            JitPrewarm::application_catalog->Finalize();
        } catch (const std::exception& e) {
            JitPrewarm::application_catalog->pcs.clear();
            JitPrewarm::application_catalog->priority.clear();
            LOG_WARNING(Core, "JIT profile {}: cross-core index unavailable: {}", layout.name, e.what());
        }
        // Owners keep their own reference; nothing else may register into it now.
        JitPrewarm::application_catalog.reset();
    }
    if (!warm) return;
    std::array<std::size_t, Hardware::NUM_CPU_CORES> sizes{};
    for (std::size_t core = 0; core < owners.size(); ++core) {
        if (owners[core]) sizes[core] = owners[core]->PrepareCandidates();
    }
    const auto start = std::chrono::steady_clock::now();
    try {
        JitPrewarm::RunOwners(sizes, [&](std::size_t core, const JitPrewarm::Progress& callback) {
            owners[core]->Warm(callback, code_budget);
        }, progress);
    } catch (const std::exception& e) {
        // RunOwners joins all workers before unwinding. Prepared blocks remain valid.
        LOG_WARNING(Core, "JIT prewarm {}: parallel stage incomplete, using normal JIT: {}", layout.name, e.what());
    }
    LOG_INFO(Core, "JIT prewarm {} parallel: {} owners, {:.1f} ms wall time", layout.name,
             std::count_if(sizes.begin(), sizes.end(), [](auto size) { return size != 0; }),
             std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
}

} // namespace Core

namespace Core::JitPrewarm {

namespace {

/// The process's static code (user R-X pages in the code region), sorted and disjoint.
/// Profiles only learn and warm blocks inside it: other code is relocated or generated.
std::vector<std::pair<u64, u64>> CollectExecutableRanges(Kernel::KProcess& process) {
    std::vector<std::pair<u64, u64>> ranges;
    auto& table = process.GetPageTable().GetBasePageTable();
    const auto end = GetInteger(table.GetCodeRegionStart()) + table.GetCodeRegionSize();
    const auto rx = (Kernel::KMemoryPermission::UserRead | Kernel::KMemoryPermission::UserExecute) &
                    Kernel::KMemoryPermission::UserMask;
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
        if ((info.m_permission & Kernel::KMemoryPermission::UserMask) == rx) {
            ranges.emplace_back(info.m_address, std::min<u64>(next, end));
        }
        addr = next;
    }
    return ranges;
}

} // namespace

std::unique_ptr<Owner> Owner::Create(System& system, Kernel::KProcess& process, std::size_t core,
                                     const Layout& layout, std::unique_ptr<Target> target) {
    if (!target || system.DebuggerEnabled()) {
        return nullptr;
    }
    try {
        Profile p{layout};
        p.title = process.GetProgramId();
        p.build = system.GetApplicationProcessBuildID();
        p.core = static_cast<u32>(core);
        p.base = GetInteger(process.GetEntryPoint());
        p.path = Common::FS::GetEdenPath(Common::FS::EdenPath::CacheDir) / "jit-profile" /
                 fmt::format("{:016x}", p.title) / fmt::format("{}{}.bin", layout.file_stem, p.core);
        p.executable_ranges = CollectExecutableRanges(process);
        p.observed.reserve(MaxRecords - GameplayCapacity);
        p.gameplay_observed.reserve(GameplayCapacity);
        std::error_code ec;
        const auto bytes = std::filesystem::file_size(p.path, ec);
        if (!ec) {
            std::ifstream file{p.path, std::ios::binary};
            if (!Read(file, bytes, layout, p.title, p.build, p.core, p.loaded)) {
                LOG_WARNING(Core, "JIT profile {} core {}: incompatible or corrupt, learning again",
                            layout.name, p.core);
            }
        }
        p.status.assign(p.loaded.size(), WarmStatus::RecordOnly);
        if (application_catalog && application_catalog->layout == &layout) {
            p.catalog = application_catalog;
            auto& keys = application_catalog->descriptors[p.core];
            keys.reserve(p.loaded.size());
            for (const auto& r : p.loaded) {
                keys.push_back(r.descriptor);
                if (r.gameplay_samples != 0) application_catalog->priority.push_back(r);
            }
        }
        LOG_INFO(Core, "JIT profile {} core {}: loaded {}, RX ranges {}", layout.name, p.core,
                 p.loaded.size(), p.executable_ranges.size());
        std::unique_ptr<Owner> owner{new Owner{std::move(p), std::move(target)}};
        owner->AttachObserver();
        return owner;
    } catch (const std::exception& e) {
        LOG_WARNING(Core, "JIT prewarm {} core {}: unavailable, using normal JIT: {}", layout.name, core,
                    e.what());
        return nullptr;
    }
}

Owner::Owner(Profile profile_, std::unique_ptr<Target> target_)
    : profile{std::move(profile_)}, target{std::move(target_)} {}

Owner::~Owner() {
    target->SetBlockObserver({});
}

void Owner::AttachObserver() {
    target->SetBlockObserver([this](const Dynarmic::BlockProfile& block) { profile.Observe(block); });
}

std::size_t Owner::PrepareCandidates() {
    try {
        return profile.PrepareShared();
    } catch (const std::exception& e) {
        profile.shared.clear();
        profile.shared_status.clear();
        LOG_WARNING(Core, "JIT profile {} core {}: shared candidates unavailable: {}", profile.layout->name,
                    profile.core, e.what());
        return 0;
    }
}

void Owner::Warm(const Progress& progress, std::size_t code_budget) {
    auto& p = profile;
    target->SetBlockObserver({}); // Warming must not record its own compilations.
    try {
        const auto plan = p.WarmPlan();
        std::size_t accepted{}, rejected{}, budget_skipped{}, priority_accepted{}, shared_accepted{};
        const auto start = std::chrono::steady_clock::now();
        const auto space = target->CodeCacheSpaceRemaining();
        const auto used = [&] { return space - target->CodeCacheSpaceRemaining(); };
        target->BeginWarm();
        for (std::size_t n = 0; n < plan.size(); ++n) {
            if (used() >= code_budget) {
                budget_skipped = plan.size() - n;
                break;
            }
            const auto& candidate = plan[n];
            const auto& record = p.GetRecord(candidate);
            auto& status = p.GetStatus(candidate);
            status = WarmStatus::Rejected;
            if (const auto descriptor = p.Rebase(record)) {
                auto block = static_cast<Dynarmic::BlockProfile>(record);
                block.descriptor = *descriptor;
                if (target->PrecompileBlock(block)) {
                    status = WarmStatus::Accepted;
                    ++accepted;
                    shared_accepted += candidate.shared;
                    priority_accepted += !candidate.shared && record.gameplay_samples != 0;
                }
            }
            rejected += status == WarmStatus::Rejected;
            if (progress && (n % 256 == 0 || n + 1 == plan.size())) {
                progress(n + 1, plan.size());
            }
        }
        if (progress && !plan.empty()) {
            progress(plan.size(), plan.size());
        }
        LOG_INFO(Core, "JIT prewarm {} core {}: RX ranges {}, loaded {}, accepted {}, rejected {}, budget skipped {}, code {:.2f} MiB, {:.1f} ms",
                 p.layout->name, p.core, p.executable_ranges.size(), p.loaded.size(), accepted, rejected,
                 budget_skipped, used() / (1024.0 * 1024.0),
                 std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
        LOG_INFO(Core, "JIT prewarm {} core {}: priority blocks {}, accepted {}; shared candidates {}, accepted {}",
                 p.layout->name, p.core,
                 std::count_if(p.loaded.begin(), p.loaded.end(), [](const auto& r) { return r.gameplay_samples != 0; }),
                 priority_accepted, p.shared.size(), shared_accepted);
    } catch (const std::exception& e) {
        LOG_WARNING(Core, "JIT prewarm {} core {}: incomplete, using normal JIT: {}", p.layout->name, p.core,
                    e.what());
    }
    AttachObserver();
}

void Owner::Save() {
    auto& p = profile;
    if (p.observed.empty() && p.gameplay_observed.empty()) {
        return;
    }
    try {
        Merge(p.loaded, p.observed);
        Merge(p.loaded, p.gameplay_observed);
        const auto observed = p.observed.size(), gameplay = p.gameplay_observed.size();
        // Merged: a second Save must not count these samples again.
        p.observed.clear();
        p.gameplay_observed.clear();
        std::error_code ec;
        std::filesystem::create_directories(p.path.parent_path(), ec);
        if (ec) {
            LOG_WARNING(Core, "JIT profile {} core {}: directory unavailable: {}", p.layout->name, p.core,
                        ec.message());
            return;
        }
        auto temp = p.path;
        temp += ".tmp";
        std::ofstream file{temp, std::ios::binary | std::ios::trunc};
        const bool written = Write(file, *p.layout, p.title, p.build, p.core, p.loaded);
        file.flush();
        const bool flushed = bool(file);
        file.close();
        if (!written || !flushed || file.fail()) {
            LOG_WARNING(Core, "JIT profile {} core {}: write failed, previous profile retained", p.layout->name,
                        p.core);
            return;
        }
        // Replace only after a complete close. No delete-before-rename gap.
        std::filesystem::rename(temp, p.path, ec);
        if (ec) {
            LOG_WARNING(Core, "JIT profile {} core {}: replacement failed: {}", p.layout->name, p.core,
                        ec.message());
            return;
        }
        LOG_INFO(Core, "JIT profile {} core {}: saved {} descriptors, {} observed, {} during T, {} dropped at limit",
                 p.layout->name, p.core, p.loaded.size(), observed, gameplay, p.dropped);
    } catch (const std::exception& e) {
        LOG_WARNING(Core, "JIT profile {} core {}: save skipped: {}", p.layout->name, p.core, e.what());
    }
}

} // namespace Core::JitPrewarm
