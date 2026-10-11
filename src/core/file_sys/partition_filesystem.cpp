// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <utility>

#include "common/logging.h"
#include "core/file_sys/partition_filesystem.h"
#include "core/file_sys/vfs/vfs_offset.h"
#include "core/loader/loader.h"

namespace FileSys {

bool PartitionFilesystem::Header::HasValidMagicValue() const {
    return magic == Common::MakeMagic('H', 'F', 'S', '0') ||
           magic == Common::MakeMagic('P', 'F', 'S', '0');
}

PartitionFilesystem::PartitionFilesystem(VirtualFile file) {
    status = Loader::ResultStatus::ErrorBadPFSHeader;
    if (!file || file->GetSize() < sizeof(Header) ||
        file->ReadObject(&pfs_header) != sizeof(Header) || !pfs_header.HasValidMagicValue()) return;
    is_hfs = pfs_header.magic == Common::MakeMagic('H', 'F', 'S', '0');
    const size_t entry_size = is_hfs ? sizeof(HFSEntry) : sizeof(PFSEntry);
    // Bound hostile metadata before allocating. Payloads can still exceed 4 GiB.
    constexpr size_t MaxMetadata = 16 * 1024 * 1024;
    const u64 metadata_size = sizeof(Header) + u64{pfs_header.num_entries} * entry_size +
                              u64{pfs_header.strtab_size};
    if (metadata_size > MaxMetadata || metadata_size > file->GetSize()) return;
    const auto data = file->ReadBytes(static_cast<size_t>(metadata_size));
    if (data.size() != metadata_size) return;
    const size_t strtab = sizeof(Header) + size_t{pfs_header.num_entries} * entry_size;
    content_offset = static_cast<size_t>(metadata_size);
    std::vector<std::pair<u64, u64>> ranges;
    for (u32 i = 0; i < pfs_header.num_entries; ++i) {
        FSEntry entry{};
        std::memcpy(&entry, data.data() + sizeof(Header) + i * entry_size, sizeof(entry));
        const size_t name_offset = entry.strtab_offset;
        if (name_offset >= pfs_header.strtab_size) return;
        const char* name_start = reinterpret_cast<const char*>(data.data() + strtab + name_offset);
        const auto* end = static_cast<const char*>(std::memchr(
            name_start, 0, size_t{pfs_header.strtab_size} - name_offset));
        if (!end || end == name_start || end - name_start > 255) return;
        std::string name{name_start, end};
        if (name == "." || name == ".." || name.find_first_of("/\\:") != std::string::npos ||
            offsets.contains(name)) return;
        const u64 offset = entry.offset, size = entry.size;
        const u64 available = file->GetSize() - content_offset;
        if (offset > available || size > available - offset) return;
        if (is_hfs) {
            HFSEntry hfs{};
            std::memcpy(&hfs, data.data() + sizeof(Header) + i * entry_size, sizeof(hfs));
            if (u64{hfs.hash_region_size} > size) return;
        }
        ranges.emplace_back(offset, offset + size);
        offsets.emplace(name, content_offset + offset);
        sizes.emplace(name, size);
        pfs_files.emplace_back(std::make_shared<OffsetVfsFile>(
            file, size, content_offset + offset, std::move(name)));
    }
    std::sort(ranges.begin(), ranges.end());
    u64 previous_end = 0;
    for (const auto& [begin, end] : ranges) {
        if (begin < previous_end && begin != end) return;
        previous_end = std::max(previous_end, end);
    }

    status = Loader::ResultStatus::Success;
}

PartitionFilesystem::~PartitionFilesystem() = default;

Loader::ResultStatus PartitionFilesystem::GetStatus() const {
    return status;
}

std::map<std::string, u64> PartitionFilesystem::GetFileOffsets() const {
    return offsets;
}

std::map<std::string, u64> PartitionFilesystem::GetFileSizes() const {
    return sizes;
}

std::vector<VirtualFile> PartitionFilesystem::GetFiles() const {
    return pfs_files;
}

std::vector<VirtualDir> PartitionFilesystem::GetSubdirectories() const {
    return {};
}

std::string PartitionFilesystem::GetName() const {
    return is_hfs ? "HFS0" : "PFS0";
}

VirtualDir PartitionFilesystem::GetParentDirectory() const {
    // TODO(DarkLordZach): Add support for nested containers.
    return nullptr;
}

} // namespace FileSys
