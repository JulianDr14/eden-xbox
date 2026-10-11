// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <iterator>
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/common_funcs.h"
#include "core/file_sys/content_archive.h"
namespace FileSys {
ContentProvider::~ContentProvider() = default;

bool ContentProvider::HasEntry(ContentProviderEntry entry) const {
    return HasEntry(entry.title_id, entry.type);
}

ContentProviderUnion::~ContentProviderUnion() = default;

void ContentProviderUnion::SetApplicationContentOverride(u64 application_id, ContentProvider* provider) {
    selected_application = GetBaseTitleID(application_id);
    selected_provider = provider;
}
ContentProvider* ContentProviderUnion::OverrideFor(u64 title_id) const {
    return selected_application && GetBaseTitleID(title_id) == *selected_application ? selected_provider : nullptr;
}

void ContentProviderUnion::SetSlot(ContentProviderUnionSlot slot, ContentProvider* provider) {
    providers[size_t(slot)] = provider;
}

void ContentProviderUnion::Refresh() {
    for (auto e : providers)
        if (e != nullptr)
            e->Refresh();
}

bool ContentProviderUnion::HasEntry(u64 title_id, ContentRecordType type) const {
    if (const auto* chosen = OverrideFor(title_id)) return chosen->HasEntry(title_id, type);
    for (auto const e : providers)
        if (e && e->HasEntry(title_id, type))
            return true;
    return false;
}

std::optional<u32> ContentProviderUnion::GetEntryVersion(u64 title_id) const {
    if (const auto* chosen = OverrideFor(title_id)) return chosen->GetEntryVersion(title_id);
    for (auto const e : providers) {
        if (e == nullptr)
            continue;
        if (auto const res = e->GetEntryVersion(title_id); res != std::nullopt)
            return res;
    }
    return std::nullopt;
}

VirtualFile ContentProviderUnion::GetEntryUnparsed(u64 title_id, ContentRecordType type) const {
    if (const auto* chosen = OverrideFor(title_id)) return chosen->GetEntryUnparsed(title_id, type);
    for (auto const e : providers) {
        if (e == nullptr)
            continue;
        if (auto const res = e->GetEntryUnparsed(title_id, type); res != nullptr)
            return res;
    }
    return nullptr;
}

VirtualFile ContentProviderUnion::GetEntryRaw(u64 title_id, ContentRecordType type) const {
    if (const auto* chosen = OverrideFor(title_id)) return chosen->GetEntryRaw(title_id, type);
    for (auto const e : providers) {
        if (e == nullptr)
            continue;
        if (auto const res = e->GetEntryRaw(title_id, type); res != nullptr)
            return res;
    }
    return nullptr;
}

std::unique_ptr<NCA> ContentProviderUnion::GetEntry(u64 title_id, ContentRecordType type) const {
    if (const auto* chosen = OverrideFor(title_id)) return chosen->GetEntry(title_id, type);
    for (auto const e : providers) {
        if (e == nullptr)
            continue;
        if (auto res = e->GetEntry(title_id, type); res != nullptr)
            return res;
    }
    return nullptr;
}

std::vector<ContentProviderEntry> ContentProviderUnion::ListEntriesFilter(std::optional<TitleType> title_type, std::optional<ContentRecordType> record_type, std::optional<u64> title_id) const {
    std::vector<ContentProviderEntry> out;
    for (auto const& e : providers) {
        if (e != nullptr) {
            auto const vec = e->ListEntriesFilter(title_type, record_type, title_id);
            std::copy_if(vec.begin(), vec.end(), std::back_inserter(out),
                [this](const auto& entry) { return !OverrideFor(entry.title_id); });
        }
    }
    if (selected_provider) {
        for (const auto& entry : selected_provider->ListEntriesFilter(title_type, record_type, title_id))
            if (OverrideFor(entry.title_id)) out.push_back(entry);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::vector<std::pair<ContentProviderUnionSlot, ContentProviderEntry>> ContentProviderUnion::ListEntriesFilterOrigin(std::optional<ContentProviderUnionSlot> origin, std::optional<TitleType> title_type, std::optional<ContentRecordType> record_type, std::optional<u64> title_id) const {
    std::vector<std::pair<ContentProviderUnionSlot, ContentProviderEntry>> out;

    for (size_t i = 0; i < providers.size(); ++i) {
        auto const& e = providers[i];
        if (e == nullptr)
            continue;
        if (origin.has_value() && *origin != ContentProviderUnionSlot(i))
            continue;
        auto const vec = e->ListEntriesFilter(title_type, record_type, title_id);
        for (const auto& entry : vec)
            if (!OverrideFor(entry.title_id)) out.emplace_back(ContentProviderUnionSlot(i), entry);
    }

    if (selected_provider && (!origin || *origin == ContentProviderUnionSlot::FrontendManual)) {
        for (const auto& entry : selected_provider->ListEntriesFilter(title_type, record_type, title_id))
            if (OverrideFor(entry.title_id)) out.emplace_back(ContentProviderUnionSlot::FrontendManual, entry);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::optional<ContentProviderUnionSlot> ContentProviderUnion::GetSlotForEntry(u64 title_id, ContentRecordType type) const {
    if (const auto* chosen = OverrideFor(title_id)) {
        if (chosen->HasEntry(title_id, type)) return ContentProviderUnionSlot::FrontendManual;
        return {};
    }
    for (size_t i = 0; i < providers.size(); ++i) {
        auto const& e = providers[i];
        if (e != nullptr && e->HasEntry(title_id, type))
            return {ContentProviderUnionSlot(i)};
    }
    return std::nullopt;
}

const ExternalContentProvider* ContentProviderUnion::GetExternalProvider(u64 title_id) const {
    return OverrideFor(title_id) ? nullptr : static_cast<const ExternalContentProvider*>(providers[size_t(ContentProviderUnionSlot::External)]);
}

} // namespace FileSys
