// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstring>
#include "common/common_types.h"
#include "common/logging.h"
#include "common/swap.h"
#include "core/file_sys/nca_metadata.h"
#include "core/file_sys/vfs/vfs.h"

namespace FileSys {

CNMT::CNMT(VirtualFile file) {
    if (!file || file->ReadObject(&header) != sizeof(header)) return;
    const size_t extended_size = header.table_offset;
    const u64 table_start = sizeof(header) + extended_size;
    const u64 content_end = table_start + u64{header.number_content_entries} * sizeof(ContentRecord);
    const u64 end = content_end + u64{header.number_meta_entries} * sizeof(MetaRecord);
    if (end > file->GetSize() || extended_size > 4096) return;
    const size_t required = header.type == TitleType::Update ? 0x18 :
        (header.type == TitleType::Application || header.type == TitleType::AOC ? 0x10 : 0);
    if (extended_size < required) return;
    extended_header = file->ReadBytes(extended_size, sizeof(header));
    if (extended_header.size() != extended_size) return;
    if (extended_size >= sizeof(opt_header))
        std::memcpy(&opt_header, extended_header.data(), sizeof(opt_header));
    content_records.resize(header.number_content_entries);
    meta_records.resize(header.number_meta_entries);
    if ((!content_records.empty() && file->ReadArray(content_records.data(), content_records.size(), table_start)
            != content_records.size() * sizeof(ContentRecord)) ||
        (!meta_records.empty() && file->ReadArray(meta_records.data(), meta_records.size(), content_end)
            != meta_records.size() * sizeof(MetaRecord))) {
        content_records.clear(); meta_records.clear(); return;
    }
    for (const auto& record : content_records)
        if (record.type >= ContentRecordType::Count) return;
    valid = true;
}

CNMT::CNMT(CNMTHeader header_, OptionalHeader opt_header_,
           std::vector<ContentRecord> content_records_, std::vector<MetaRecord> meta_records_)
    : valid(true), header(std::move(header_)), opt_header(std::move(opt_header_)),
      content_records(std::move(content_records_)), meta_records(std::move(meta_records_)) {
    const size_t required = header.type == TitleType::Update ? 0x18 :
        (header.type >= TitleType::Application && header.type <= TitleType::AOC ? 0x10 : 0);
    header.table_offset = static_cast<u16>(std::max<size_t>(header.table_offset, required));
    extended_header.resize(header.table_offset);
    if (extended_header.size() >= sizeof(opt_header))
        std::memcpy(extended_header.data(), &opt_header, sizeof(opt_header));
}

std::optional<PatchMetaExtendedHeader> CNMT::GetPatchHeader() const {
    if (!valid || header.type != TitleType::Update || extended_header.size() < sizeof(PatchMetaExtendedHeader)) return {};
    PatchMetaExtendedHeader result{}; std::memcpy(&result, extended_header.data(), sizeof(result)); return result;
}
std::optional<AddOnContentMetaExtendedHeader> CNMT::GetAddOnContentHeader() const {
    if (!valid || header.type != TitleType::AOC || extended_header.size() < 0x10) return {};
    AddOnContentMetaExtendedHeader result{};
    std::memcpy(&result, extended_header.data(), std::min(extended_header.size(), sizeof(result))); return result;
}
std::optional<u64> CNMT::GetApplicationId() const {
    if (!valid || (header.type != TitleType::Update && header.type != TitleType::AOC)) return {};
    return u64{opt_header.title_id};
}
u32 CNMT::GetRequiredApplicationVersion() const {
    if (!valid || extended_header.size() < 0x10) return 0;
    u32_le value{};
    const size_t offset = header.type == TitleType::AOC ? 8 : 12;
    if (header.type != TitleType::AOC && header.type != TitleType::Application) return 0;
    std::memcpy(&value, extended_header.data() + offset, sizeof(value));
    return value;
}
u32 CNMT::GetRequiredSystemVersion() const {
    if (!valid || extended_header.size() < 12 ||
        (header.type != TitleType::Update && header.type != TitleType::Application)) return 0;
    u32_le value{}; std::memcpy(&value, extended_header.data() + 8, sizeof(value)); return value;
}

u64 CNMT::GetDataPatchId() const {
    if (!valid || header.type != TitleType::AOC || extended_header.size() < 0x18) return 0;
    u64_le value{}; std::memcpy(&value, extended_header.data() + 0x10, sizeof(value)); return value;
}
CNMT::~CNMT() = default;

const CNMTHeader& CNMT::GetHeader() const {
    return header;
}

u64 CNMT::GetTitleID() const {
    return header.title_id;
}

u32 CNMT::GetTitleVersion() const {
    return header.title_version;
}

TitleType CNMT::GetType() const {
    return header.type;
}

const std::vector<ContentRecord>& CNMT::GetContentRecords() const {
    return content_records;
}

const std::vector<MetaRecord>& CNMT::GetMetaRecords() const {
    return meta_records;
}

bool CNMT::UnionRecords(const CNMT& other) {
    bool change = false;
    for (const auto& rec : other.content_records) {
        const auto iter = std::find_if(content_records.begin(), content_records.end(),
                                       [&rec](const ContentRecord& r) {
                                           return r.nca_id == rec.nca_id && r.type == rec.type;
                                       });
        if (iter == content_records.end()) {
            content_records.emplace_back(rec);
            ++header.number_content_entries;
            change = true;
        }
    }
    for (const auto& rec : other.meta_records) {
        const auto iter =
            std::find_if(meta_records.begin(), meta_records.end(), [&rec](const MetaRecord& r) {
                return r.title_id == rec.title_id && r.title_version == rec.title_version &&
                       r.type == rec.type;
            });
        if (iter == meta_records.end()) {
            meta_records.emplace_back(rec);
            ++header.number_meta_entries;
            change = true;
        }
    }
    return change;
}

std::vector<u8> CNMT::Serialize() const {
    if (!valid) return {};
    const size_t dead_zone = sizeof(CNMTHeader) + size_t{header.table_offset};
    std::vector<u8> out(dead_zone + content_records.size() * sizeof(ContentRecord) +
                        meta_records.size() * sizeof(MetaRecord));
    std::memcpy(out.data(), &header, sizeof(header));
    if (!extended_header.empty())
        std::memcpy(out.data() + sizeof(header), extended_header.data(), extended_header.size());

    u64_le offset = header.table_offset;

    for (const auto& rec : content_records) {
        memcpy(out.data() + offset + sizeof(CNMTHeader), &rec, sizeof(ContentRecord));
        offset += sizeof(ContentRecord);
    }

    for (const auto& rec : meta_records) {
        memcpy(out.data() + offset + sizeof(CNMTHeader), &rec, sizeof(MetaRecord));
        offset += sizeof(MetaRecord);
    }

    return out;
}
} // namespace FileSys
