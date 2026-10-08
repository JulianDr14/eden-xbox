// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <functional>
#include <memory>

#include "common/common_types.h"
#include "core/arm/dynarmic/jit_prewarm_profile.h"
#include "core/arm/jit_prewarm_parallel.h"

namespace Core {
class System;
}

namespace Kernel {
class KProcess;
}

namespace Core::JitPrewarm {

/// What the prewarm needs from one core's JIT. Owner thread only, guest stopped.
class Target {
public:
    using Observer = std::function<void(const Dynarmic::BlockProfile&)>;

    virtual ~Target() = default;
    /// Called with every block the JIT compiles from guest code; empty detaches.
    virtual void SetBlockObserver(Observer observer) = 0;
    /// Compiles the block only if the guest code is exactly the recorded one.
    virtual bool PrecompileBlock(const Dynarmic::BlockProfile& block) = 0;
    virtual std::size_t CodeCacheSpaceRemaining() const = 0;
    /// Before a warm pass: drop state that caches guest code between compiles.
    virtual void BeginWarm() {}
};

/// Any dynarmic Jit with the profile API (A64::Jit, A32::Jit), with the frontend callbacks
/// whose code page cache must not survive into a warm pass.
template <typename Jit, typename Callbacks>
class DynarmicTarget final : public Target {
public:
    DynarmicTarget(Jit& jit_, Callbacks& callbacks_) : jit{jit_}, callbacks{callbacks_} {}

    void SetBlockObserver(Observer observer) override {
        jit.SetBlockProfileCallback(std::move(observer));
    }
    bool PrecompileBlock(const Dynarmic::BlockProfile& block) override {
        return jit.PrecompileBlock(block);
    }
    std::size_t CodeCacheSpaceRemaining() const override {
        return jit.GetCodeCacheSpaceRemaining();
    }
    void BeginWarm() override {
        callbacks.last_code_addr = u64(-1);
    }

private:
    Jit& jit;
    Callbacks& callbacks;
};

/// One core's JIT profile. While the guest runs it learns the blocks the JIT compiles; at
/// shutdown it saves them; before the next boot it compiles them again (the warm pass), so
/// the game does not stall on first execution. ISA specifics live in Layout and Target.
class Owner {
public:
    /// Loads this core's profile, or starts a new one, and attaches the observer. Registers it
    /// in application_catalog when one is being built. nullptr when the prewarm cannot run
    /// (debugger attached) or failed; the JIT then works as usual.
    static std::unique_ptr<Owner> Create(System& system, Kernel::KProcess& process, std::size_t core,
                                         const Layout& layout, std::unique_ptr<Target> target);

    /// Detaches the observer: the JIT may outlive this object by a few members.
    ~Owner();

    Owner(const Owner&) = delete;
    Owner& operator=(const Owner&) = delete;

    /// Candidate count for the warm pass, after application_catalog is final.
    std::size_t PrepareCandidates();
    /// The warm pass: compiles candidates, best first, until `code_budget` bytes of host code.
    /// Runs on its own thread, one per owner; `progress` reports (done, total).
    void Warm(const Progress& progress, std::size_t code_budget);
    /// Merges what this session learned and replaces the file atomically. The guest must be
    /// stopped; reads no guest memory, so it is safe while the process is torn down.
    void Save();

private:
    Owner(Profile profile, std::unique_ptr<Target> target);
    void AttachObserver();

    Profile profile;
    std::unique_ptr<Target> target;
};

} // namespace Core::JitPrewarm
