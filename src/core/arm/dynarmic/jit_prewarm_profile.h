// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <memory>
#include <numeric>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "core/arm/jit_prewarm_stats.h"
#include "dynarmic/interface/block_profile.h"

namespace Core::JitPrewarm {

/// What differs between guest ISAs in a profile: where the PC sits in a block descriptor,
/// which other descriptor bits select a translation, how a PC is aligned, and the file
/// identity. Records, merge, catalog and warm plan are shared by every layout.
struct Layout {
    std::string_view name;            ///< Log tag.
    std::string_view file_stem;       ///< "<stem><core>.bin" in the title's profile directory.
    std::array<char, 8> magic;
    std::optional<std::array<char, 8>> legacy_magic; ///< Readable older format, no gameplay samples.
    std::uint64_t pc_mask;
    std::uint64_t state_mask;         ///< Descriptor bits besides the PC that a block depends on.
    std::uint64_t halfword_pc_bit;    ///< State bit that allows a halfword-aligned PC (Thumb).

    constexpr std::uint64_t Pc(std::uint64_t descriptor) const { return descriptor & pc_mask; }
    constexpr std::uint64_t State(std::uint64_t descriptor) const { return descriptor & state_mask; }
    constexpr bool ValidDescriptor(std::uint64_t descriptor) const {
        const std::uint64_t alignment = (descriptor & halfword_pc_bit) != 0 ? 1 : 3;
        return (descriptor & alignment) == 0 && (descriptor & ~(pc_mask | state_mask)) == 0;
    }
};

// A64: 56-bit PC, FPCR mode bits above it. Single-stepping is excluded: never recorded.
inline constexpr Layout A64Layout{
    "a64", "core-", {'E','D','J','I','T','P','0','2'}, std::array<char, 8>{'E','D','J','I','T','P','0','1'},
    (1ULL << 56) - 1, 0x07c80000ULL << 37, 0};
// A32: 32-bit PC; above it FPSCR mode bits, T, E and the IT state (A32::LocationDescriptor).
inline constexpr Layout A32Layout{
    "a32", "a32-core-", {'E','D','J','A','3','2','0','1'}, std::nullopt,
    0xffff'ffffULL, 0x07f7'ff03ULL << 32, 1ULL << 32};

/// The first guest word a block was translated from; BlockProfile::code_bytes counts from it.
constexpr std::uint64_t CodeStart(std::uint64_t pc) {
    return pc & ~std::uint64_t{3};
}

struct Record : Dynarmic::BlockProfile {
    std::uint32_t gameplay_samples{};
    Record() = default;
    Record(std::uint64_t descriptor_, std::uint64_t hash_, std::uint32_t bytes_,
           std::uint32_t samples = 0)
        : Dynarmic::BlockProfile{descriptor_, hash_, bytes_}, gameplay_samples{samples} {}
    explicit Record(const Dynarmic::BlockProfile& block) : Dynarmic::BlockProfile{block} {}
};
using BuildId = std::array<std::uint8_t, 32>;
constexpr std::size_t MaxRecords = 262144;
constexpr std::size_t HeaderBytes = 8 + 8 + 32 + 4 + 4 + 8;
constexpr std::size_t RecordBytes = 8 + 8 + 4 + 4;
constexpr std::size_t LegacyRecordBytes = 8 + 8 + 4;
constexpr std::size_t GameplayCapacity = MaxRecords / 4;

inline bool ValidRecord(const Layout& layout, const Record& record) {
    return record.code_bytes != 0 && record.code_bytes <= Record::MaxCodeBytes &&
           (record.code_bytes & 3) == 0 && layout.ValidDescriptor(record.descriptor);
}

inline std::uint64_t PayloadHash(std::span<const Record> records, bool legacy = false) {
    std::uint64_t hash = Record::HashSeed;
    for (const auto& r : records) {
        for (auto [value, bytes] : {std::pair{r.descriptor, 8U},
                                   std::pair{r.code_hash, 8U},
                                   std::pair{std::uint64_t{r.code_bytes}, 4U}}) {
            for (unsigned n = 0; n < bytes; ++n) {
                hash = (hash ^ (value & 0xff)) * 1099511628211ULL;
                value >>= 8;
            }
        }
        if (!legacy) {
            auto value = r.gameplay_samples;
            for (unsigned n = 0; n < 4; ++n) {
                hash = (hash ^ (value & 0xff)) * 1099511628211ULL;
                value >>= 8;
            }
        }
    }
    return hash;
}

inline void WriteInteger(std::ostream& out, std::uint64_t value, unsigned bytes) {
    for (unsigned n = 0; n < bytes; ++n) {
        out.put(static_cast<char>(value & 0xff));
        value >>= 8;
    }
}

inline std::uint64_t ReadInteger(std::istream& in, unsigned bytes) {
    std::uint64_t value{};
    for (unsigned n = 0; n < bytes; ++n) {
        const auto byte = in.get();
        if (byte == std::char_traits<char>::eof()) {
            in.setstate(std::ios::failbit);
            return 0;
        }
        value |= std::uint64_t{static_cast<unsigned char>(byte)} << (n * 8);
    }
    return value;
}

// Versioned little-endian file; no native struct padding, guest bytes or host pointers.
// The magic names the layout, so a profile is never read with another ISA's rules.
// Failure leaves the destination unchanged and bounds allocation before reading payload.
inline bool Read(std::istream& in, std::uint64_t file_bytes, const Layout& layout, std::uint64_t title,
                 const BuildId& build, std::uint32_t core, std::vector<Record>& result) {
    std::array<char, 8> magic{};
    in.read(magic.data(), magic.size());
    const auto stored_title = ReadInteger(in, 8);
    BuildId stored_build{};
    in.read(reinterpret_cast<char*>(stored_build.data()), stored_build.size());
    const auto stored_core = ReadInteger(in, 4);
    const auto count = ReadInteger(in, 4);
    const auto hash = ReadInteger(in, 8);
    const bool legacy = layout.legacy_magic && magic == *layout.legacy_magic;
    const auto record_bytes = legacy ? LegacyRecordBytes : RecordBytes;
    if (!in || (!legacy && magic != layout.magic) ||
        stored_title != title || stored_build != build || stored_core != core ||
        count > MaxRecords || file_bytes != HeaderBytes + count * record_bytes) {
        return false;
    }
    std::vector<Record> records;
    records.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t n = 0; n < count; ++n) {
        Record r;
        r.descriptor = ReadInteger(in, 8);
        r.code_hash = ReadInteger(in, 8);
        r.code_bytes = static_cast<std::uint32_t>(ReadInteger(in, 4));
        if (!legacy) {
            r.gameplay_samples = static_cast<std::uint32_t>(ReadInteger(in, 4));
        }
        if (!in || !ValidRecord(layout, r) ||
            (!records.empty() && records.back().descriptor >= r.descriptor)) {
            return false;
        }
        records.push_back(r);
    }
    if (PayloadHash(records, legacy) != hash) {
        return false;
    }
    result = std::move(records);
    return true;
}

inline bool Write(std::ostream& out, const Layout& layout, std::uint64_t title, const BuildId& build,
                  std::uint32_t core, std::span<const Record> records) {
    if (records.size() > MaxRecords) {
        return false;
    }
    out.write(layout.magic.data(), layout.magic.size());
    WriteInteger(out, title, 8);
    out.write(reinterpret_cast<const char*>(build.data()), build.size());
    WriteInteger(out, core, 4);
    WriteInteger(out, records.size(), 4);
    WriteInteger(out, PayloadHash(records), 8);
    for (const auto& r : records) {
        WriteInteger(out, r.descriptor, 8);
        WriteInteger(out, r.code_hash, 8);
        WriteInteger(out, r.code_bytes, 4);
        WriteInteger(out, r.gameplay_samples, 4);
    }
    return bool(out);
}

// New observations win if code at an existing descriptor changed. Sort/dedup is
// deferred until shutdown; recording is a bounded push into preallocated storage.
inline void Merge(std::vector<Record>& old, std::span<const Record> recent) {
    old.insert(old.end(), recent.begin(), recent.end());
    std::stable_sort(old.begin(), old.end(), [](const auto& a, const auto& b) {
        return a.descriptor < b.descriptor;
    });
    std::size_t output{};
    for (std::size_t i = 0; i < old.size();) {
        auto end = i + 1;
        while (end < old.size() && old[end].descriptor == old[i].descriptor) {
            ++end;
        }
        auto latest = old[end - 1];
        std::uint64_t samples{};
        for (auto n = i; n < end; ++n) {
            // Changed code must learn its own priority.
            if (old[n].code_hash == latest.code_hash && old[n].code_bytes == latest.code_bytes) {
                samples += old[n].gameplay_samples;
            }
        }
        latest.gameplay_samples = static_cast<std::uint32_t>(std::min<std::uint64_t>(samples, UINT32_MAX));
        old[output++] = latest;
        i = end;
    }
    old.resize(output);
    if (old.size() > MaxRecords) {
        std::partial_sort(old.begin(), old.begin() + MaxRecords, old.end(), [](const auto& a, const auto& b) {
            return a.gameplay_samples != b.gameplay_samples ? a.gameplay_samples > b.gameplay_samples
                                                           : a.descriptor < b.descriptor;
        });
        old.resize(MaxRecords);
        std::sort(old.begin(), old.end(), [](const auto& a, const auto& b) { return a.descriptor < b.descriptor; });
    }
}

enum class WarmStatus : std::uint8_t { RecordOnly, Budget, Rejected, Accepted };

// Shared immutable indexes once CPU owners start. Identity-checked records only;
// cross-core/FPCR matches indicate profile coverage, not permission to reuse host code.
// One application has one ISA, so one layout describes every owner's descriptors.
struct Catalog {
    explicit Catalog(const Layout& layout_) : layout{&layout_} {}

    const Layout* layout;
    std::array<std::vector<std::uint64_t>, 4> descriptors;
    std::vector<std::uint64_t> pcs;
    std::vector<Record> priority;
    void Finalize() {
        // Conflicting fingerprints at one descriptor cannot be chosen safely
        // across profiles. Own-core records remain authoritative for that core.
        std::stable_sort(priority.begin(), priority.end(), [](const auto& a, const auto& b) {
            return a.descriptor < b.descriptor;
        });
        std::size_t output{};
        for (std::size_t i = 0; i < priority.size();) {
            auto end = i + 1;
            auto chosen = priority[i];
            bool conflict{};
            while (end < priority.size() && priority[end].descriptor == chosen.descriptor) {
                conflict |= priority[end].code_hash != chosen.code_hash || priority[end].code_bytes != chosen.code_bytes;
                chosen.gameplay_samples = std::max(chosen.gameplay_samples, priority[end].gameplay_samples);
                ++end;
            }
            if (!conflict) priority[output++] = chosen;
            i = end;
        }
        priority.resize(output);
        for (const auto& core : descriptors) {
            for (const auto descriptor : core) pcs.push_back(layout->Pc(descriptor));
        }
        std::sort(pcs.begin(), pcs.end());
        pcs.erase(std::unique(pcs.begin(), pcs.end()), pcs.end());
    }
};
inline std::shared_ptr<Catalog> application_catalog;

struct Profile {
    explicit Profile(const Layout& layout_) : layout{&layout_} {}

    const Layout* layout;
    std::filesystem::path path;
    BuildId build{};
    std::uint64_t title{};
    std::uint64_t base{};
    std::uint32_t core{};
    std::vector<std::pair<std::uint64_t, std::uint64_t>> executable_ranges;
    std::vector<Record> loaded;
    std::vector<Record> observed;
    std::vector<Record> gameplay_observed;
    std::vector<WarmStatus> status;
    std::vector<Record> shared;
    std::vector<WarmStatus> shared_status;
    std::shared_ptr<const Catalog> catalog;
    std::size_t dropped{};

    bool Contains(std::uint64_t pc, std::uint32_t bytes) const {
        const auto next = std::upper_bound(executable_ranges.begin(), executable_ranges.end(), pc,
            [](auto address, const auto& range) { return address < range.first; });
        if (next == executable_ranges.begin()) {
            return false;
        }
        const auto& range = *std::prev(next);
        return pc >= range.first && pc < range.second && bytes <= range.second - pc;
    }

    /// The recorded block's descriptor in this session's address space, or nullopt when its
    /// code does not lie inside the static RX image (a relocation or a different build).
    std::optional<std::uint64_t> Rebase(const Record& relative) const {
        const auto offset = layout->Pc(relative.descriptor);
        if (base > layout->pc_mask || offset > layout->pc_mask - base) {
            return std::nullopt;
        }
        const auto pc = base + offset;
        if (!Contains(CodeStart(pc), relative.code_bytes)) {
            return std::nullopt;
        }
        return layout->State(relative.descriptor) | pc;
    }

    std::size_t PrepareShared() {
        shared.clear();
        // Do not fill a previously unused owner solely on speculation.
        if (catalog && !loaded.empty()) {
            for (const auto& block : catalog->priority) {
                const auto own = std::lower_bound(loaded.begin(), loaded.end(), block.descriptor,
                    [](const auto& r, auto descriptor) { return r.descriptor < descriptor; });
                if (own == loaded.end() || own->descriptor != block.descriptor) shared.push_back(block);
            }
        }
        if (shared.size() > GameplayCapacity) {
            std::partial_sort(shared.begin(), shared.begin() + GameplayCapacity, shared.end(), [](const auto& a, const auto& b) {
                return a.gameplay_samples != b.gameplay_samples ? a.gameplay_samples > b.gameplay_samples
                                                               : a.descriptor < b.descriptor;
            });
            shared.resize(GameplayCapacity);
            std::sort(shared.begin(), shared.end(), [](const auto& a, const auto& b) { return a.descriptor < b.descriptor; });
        }
        shared_status.assign(shared.size(), WarmStatus::Budget);
        status.assign(loaded.size(), WarmStatus::Budget);
        return loaded.size() + shared.size();
    }

    struct Candidate {
        std::size_t index;
        std::uint8_t priority;
        bool shared;
    };
    const Record& GetRecord(const Candidate& candidate) const {
        return candidate.shared ? shared[candidate.index] : loaded[candidate.index];
    }
    WarmStatus& GetStatus(const Candidate& candidate) {
        return candidate.shared ? shared_status[candidate.index] : status[candidate.index];
    }
    std::vector<Candidate> WarmPlan() const {
        std::vector<Candidate> plan;
        plan.reserve(loaded.size() + shared.size());
        for (std::size_t i = 0; i < loaded.size(); ++i) {
            const auto& block = loaded[i];
            std::uint8_t rank = block.gameplay_samples != 0 ? 2 : 0;
            if (rank == 0 && catalog) {
                const auto peer = std::lower_bound(catalog->priority.begin(), catalog->priority.end(), block.descriptor,
                    [](const auto& r, auto descriptor) { return r.descriptor < descriptor; });
                if (peer != catalog->priority.end() && peer->descriptor == block.descriptor &&
                    peer->code_hash == block.code_hash && peer->code_bytes == block.code_bytes) rank = 1;
            }
            plan.push_back({i, rank, false});
        }
        for (std::size_t i = 0; i < shared.size(); ++i) plan.push_back({i, 1, true});
        std::stable_sort(plan.begin(), plan.end(), [&](const auto& a, const auto& b) {
            return a.priority != b.priority ? a.priority > b.priority
                 : GetRecord(a).gameplay_samples > GetRecord(b).gameplay_samples;
        });
        return plan;
    }

    Miss Classify(const Record& relative) const {
        const auto found = std::lower_bound(loaded.begin(), loaded.end(), relative.descriptor,
            [](const auto& r, auto descriptor) { return r.descriptor < descriptor; });
        if (found != loaded.end() && found->descriptor == relative.descriptor) {
            if (found->code_hash != relative.code_hash || found->code_bytes != relative.code_bytes)
                return Miss::CodeChanged;
            switch (status[static_cast<std::size_t>(found - loaded.begin())]) {
            case WarmStatus::Budget: return Miss::Budget;
            case WarmStatus::Rejected: return Miss::Rejected;
            case WarmStatus::Accepted: return Miss::Recompiled;
            case WarmStatus::RecordOnly: return Miss::RecordOnly;
            }
        }
        const auto peer = std::lower_bound(shared.begin(), shared.end(), relative.descriptor,
            [](const auto& r, auto descriptor) { return r.descriptor < descriptor; });
        if (peer != shared.end() && peer->descriptor == relative.descriptor) {
            if (peer->code_hash != relative.code_hash || peer->code_bytes != relative.code_bytes) return Miss::CodeChanged;
            switch (shared_status[static_cast<std::size_t>(peer - shared.begin())]) {
            case WarmStatus::Budget: return Miss::Budget;
            case WarmStatus::Rejected: return Miss::Rejected;
            case WarmStatus::Accepted: return Miss::Recompiled;
            case WarmStatus::RecordOnly: return Miss::RecordOnly;
            }
        }
        if (catalog) {
            for (std::size_t n = 0; n < catalog->descriptors.size(); ++n) {
                const auto& keys = catalog->descriptors[n];
                if (n != core && std::binary_search(keys.begin(), keys.end(), relative.descriptor))
                    return Miss::OtherCore;
            }
            if (std::binary_search(catalog->pcs.begin(), catalog->pcs.end(), layout->Pc(relative.descriptor)))
                return Miss::FpcrVariant;
        }
        return Miss::Unlearned;
    }

    void Observe(const Dynarmic::BlockProfile& block) {
        // Reject invalid state bits before State() masks them away (including single-step).
        if (!ValidRecord(*layout, Record{block})) {
            return;
        }
        const auto pc = layout->Pc(block.descriptor);
        const bool gameplay = capture_active.load(std::memory_order_relaxed);
        if (pc < base || !Contains(CodeStart(pc), block.code_bytes)) {
            miss_counters[core][static_cast<std::size_t>(Miss::OutsideExecutable)].fetch_add(1, std::memory_order_relaxed);
            return;
        }
        Record relative{block};
        relative.descriptor = layout->State(block.descriptor) | (pc - base);
        // Read rejects a whole file over one bad record: never save one.
        if (!ValidRecord(*layout, relative)) {
            return;
        }
        miss_counters[core][static_cast<std::size_t>(Classify(relative))].fetch_add(1, std::memory_order_relaxed);
        relative.gameplay_samples = gameplay ? 1 : 0;
        auto& destination = gameplay ? gameplay_observed : observed;
        const auto capacity = gameplay ? GameplayCapacity : MaxRecords - GameplayCapacity;
        if (destination.size() == capacity) {
            ++dropped;
            return;
        }
        destination.push_back(relative);
    }
};

} // namespace Core::JitPrewarm
