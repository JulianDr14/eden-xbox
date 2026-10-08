// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <atomic>
#include <iterator>
#include <utility>

#include <fmt/format.h>

#include "common/memory_ledger.h"

namespace Common {

namespace {

constexpr size_t NUM_ACCOUNTS = static_cast<size_t>(MemoryAccount::Count);

struct AccountInfo {
    std::string_view name;
    std::string_view objects; ///< What the account counts, plural.
    bool gpu_local;           ///< In the process commit only on a unified architecture.
    bool counted_only;        ///< Bytes unknown: only the objects are reported.
};

constexpr std::array<AccountInfo, NUM_ACCOUNTS> ACCOUNTS{{
    {"textures", "heaps", true, false},
    {"buffers", "buffers", true, false},
    {"staging ring", "rings", false, false},
    {"staging upload", "buffers", false, false},
    {"staging readback", "buffers", false, false},
    {"shader bytecode", "programs", false, false},
    {"pipelines", "pipelines", false, false},
    {"resident PSOs", "", false, true},
}};

std::array<std::atomic<u64>, NUM_ACCOUNTS> g_bytes{};
std::array<std::atomic<u64>, NUM_ACCOUNTS> g_objects{};
std::atomic<bool> g_gpu_unified{};

size_t Index(MemoryAccount account) noexcept {
    return static_cast<size_t>(account);
}

} // Anonymous namespace

MemoryCharge::MemoryCharge(MemoryAccount account_, u64 bytes_, u64 objects_) noexcept
    : account{account_}, bytes{bytes_}, objects{objects_} {
    g_bytes[Index(account)].fetch_add(bytes, std::memory_order_relaxed);
    g_objects[Index(account)].fetch_add(objects, std::memory_order_relaxed);
}

MemoryCharge::~MemoryCharge() {
    Reset();
}

MemoryCharge::MemoryCharge(MemoryCharge&& other) noexcept
    : account{std::exchange(other.account, MemoryAccount::Count)},
      bytes{std::exchange(other.bytes, 0)}, objects{std::exchange(other.objects, 0)} {}

MemoryCharge& MemoryCharge::operator=(MemoryCharge&& other) noexcept {
    if (this != &other) {
        Reset();
        account = std::exchange(other.account, MemoryAccount::Count);
        bytes = std::exchange(other.bytes, 0);
        objects = std::exchange(other.objects, 0);
    }
    return *this;
}

void MemoryCharge::Reset() noexcept {
    if (account == MemoryAccount::Count) {
        return;
    }
    g_bytes[Index(account)].fetch_sub(bytes, std::memory_order_relaxed);
    g_objects[Index(account)].fetch_sub(objects, std::memory_order_relaxed);
    account = MemoryAccount::Count;
    bytes = objects = 0;
}

MemoryAccountTotals ReadMemoryAccount(MemoryAccount account) noexcept {
    return {g_bytes[Index(account)].load(std::memory_order_relaxed),
            g_objects[Index(account)].load(std::memory_order_relaxed)};
}

void SetGpuMemoryUnified(bool unified) noexcept {
    g_gpu_unified.store(unified, std::memory_order_relaxed);
}

u64 MemoryLedgerCommittedBytes() noexcept {
    const bool unified = g_gpu_unified.load(std::memory_order_relaxed);
    u64 total = 0;
    for (size_t i = 0; i < NUM_ACCOUNTS; ++i) {
        if (unified || !ACCOUNTS[i].gpu_local) {
            total += g_bytes[i].load(std::memory_order_relaxed);
        }
    }
    return total;
}

std::string DescribeMemoryLedger() {
    const bool unified = g_gpu_unified.load(std::memory_order_relaxed);
    std::string text;
    for (size_t i = 0; i < NUM_ACCOUNTS; ++i) {
        const AccountInfo& info = ACCOUNTS[i];
        if (info.counted_only) {
            fmt::format_to(std::back_inserter(text), "{}{} {}", text.empty() ? "" : ", ",
                           g_objects[i].load(std::memory_order_relaxed), info.name);
            continue;
        }
        fmt::format_to(std::back_inserter(text), "{}{} {} MiB in {} {}{}", text.empty() ? "" : ", ",
                       info.name, g_bytes[i].load(std::memory_order_relaxed) >> 20,
                       g_objects[i].load(std::memory_order_relaxed), info.objects,
                       info.gpu_local && !unified ? " (video memory)" : "");
    }
    return text;
}

} // namespace Common
