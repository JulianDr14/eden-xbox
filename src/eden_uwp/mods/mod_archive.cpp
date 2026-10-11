// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#include "eden_uwp/mods/mod_archive.h"

#include <algorithm>
#include <fstream>
#include <unordered_map>

#include <archive.h>
#include <archive_entry.h>

namespace EdenXbox::Mods {
namespace {
namespace fs = std::filesystem;

constexpr std::size_t BlockSize = 1 << 17;
/// More entries than any real mod has; a bound on what a hostile header can make us track.
constexpr std::size_t MaxEntries = 200'000;

class FileSource final : public ArchiveSource {
public:
    FileSource(std::ifstream file_, std::uint64_t size_) : file{std::move(file_)}, size{size_} {}

    std::uint64_t Size() const override {
        return size;
    }

    std::size_t Read(std::uint64_t offset, std::uint8_t* out, std::size_t length) override {
        if (offset >= size) return 0;
        file.clear();
        file.seekg(static_cast<std::streamoff>(offset));
        file.read(reinterpret_cast<char*>(out), static_cast<std::streamsize>(length));
        return static_cast<std::size_t>(file.gcount());
    }

private:
    std::ifstream file;
    std::uint64_t size;
};

/// One volume as libarchive's callbacks see it.
struct Volume {
    ArchiveSource* source{};
    std::uint64_t position{};
    std::vector<std::uint8_t> buffer;
};

la_ssize_t ReadVolume(archive*, void* data, const void** out) {
    auto& volume = *static_cast<Volume*>(data);
    *out = volume.buffer.data();
    const auto read = volume.source->Read(volume.position, volume.buffer.data(), volume.buffer.size());
    volume.position += read;
    return static_cast<la_ssize_t>(read);
}

la_int64_t SeekVolume(archive*, void* data, la_int64_t offset, int whence) {
    auto& volume = *static_cast<Volume*>(data);
    const auto size = static_cast<la_int64_t>(volume.source->Size());
    const la_int64_t base = whence == SEEK_SET   ? 0
                            : whence == SEEK_CUR ? static_cast<la_int64_t>(volume.position)
                                                 : size;
    const la_int64_t target = base + offset;
    if (target < 0) return ARCHIVE_FATAL;
    volume.position = static_cast<std::uint64_t>(std::min(target, size));
    return static_cast<la_int64_t>(volume.position);
}

la_int64_t SkipVolume(archive*, void* data, la_int64_t request) {
    auto& volume = *static_cast<Volume*>(data);
    const auto left = volume.source->Size() - std::min(volume.position, volume.source->Size());
    const auto skipped = std::min<std::uint64_t>(static_cast<std::uint64_t>(std::max<la_int64_t>(request, 0)), left);
    volume.position += skipped;
    return static_cast<la_int64_t>(skipped);
}

int SwitchVolume(archive*, void*, void* next) {
    if (next != nullptr) static_cast<Volume*>(next)->position = 0;
    return ARCHIVE_OK;
}

/// An open libarchive reader over the volumes, with the four formats a mod comes in.
class Reader {
public:
    explicit Reader(std::vector<std::unique_ptr<ArchiveSource>>& sources) : handle{archive_read_new()} {
        archive_read_support_format_zip(handle);
        archive_read_support_format_7zip(handle);
        archive_read_support_format_rar(handle);
        archive_read_support_format_rar5(handle);
        volumes.reserve(sources.size());
        for (auto& source : sources) {
            volumes.push_back({source.get(), 0, std::vector<std::uint8_t>(BlockSize)});
        }
        for (std::size_t i = 0; i < volumes.size(); ++i) {
            if (i == 0) archive_read_set_callback_data(handle, &volumes[i]);
            else archive_read_append_callback_data(handle, &volumes[i]);
        }
        archive_read_set_read_callback(handle, ReadVolume);
        archive_read_set_seek_callback(handle, SeekVolume);
        archive_read_set_skip_callback(handle, SkipVolume);
        archive_read_set_switch_callback(handle, SwitchVolume);
        opened = !volumes.empty() && archive_read_open1(handle) == ARCHIVE_OK;
    }

    ~Reader() {
        archive_read_free(handle);
    }

    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;

    archive* Get() const {
        return handle;
    }

    bool Opened() const {
        return opened;
    }

    std::string Message() const {
        const char* message = archive_error_string(handle);
        return message != nullptr ? message : "";
    }

    /// Why reading stopped: a password, or anything else.
    ArchiveError Failure(archive_entry* entry) const {
        const bool encrypted = archive_read_has_encrypted_entries(handle) > 0 ||
                               (entry != nullptr && archive_entry_is_encrypted(entry));
        return encrypted ? ArchiveError::Encrypted : ArchiveError::Unreadable;
    }

private:
    archive* handle;
    std::vector<Volume> volumes;
    bool opened{};
};

/// The entry's path, '/'-separated and without a leading "./".
std::string EntryPath(archive_entry* entry) {
    const char* utf8 = archive_entry_pathname_utf8(entry);
    std::string path = utf8 != nullptr ? utf8 : "";
    if (path.empty() && archive_entry_pathname(entry) != nullptr) path = archive_entry_pathname(entry);
    std::replace(path.begin(), path.end(), '\\', '/');
    while (path.starts_with("./")) path.erase(0, 2);
    return path;
}

bool IsFile(archive_entry* entry) {
    return archive_entry_filetype(entry) == AE_IFREG;
}

fs::path Utf8Path(const std::string& text) {
    return fs::path{std::u8string{text.begin(), text.end()}};
}

} // namespace

std::unique_ptr<ArchiveSource> OpenFileSource(const fs::path& path) {
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    std::ifstream file{path, std::ios::binary};
    if (ec || !file) return nullptr;
    return std::make_unique<FileSource>(std::move(file), size);
}

ArchiveListing ListArchive(std::vector<std::unique_ptr<ArchiveSource>>& volumes, std::stop_token stop) {
    ArchiveListing listing;
    Reader reader{volumes};
    if (!reader.Opened()) {
        listing.error = ArchiveError::Unreadable;
        listing.detail = reader.Message();
        return listing;
    }
    archive_entry* entry = nullptr;
    std::size_t entries = 0;
    int status = ARCHIVE_OK;
    while ((status = archive_read_next_header(reader.Get(), &entry)) == ARCHIVE_OK ||
           status == ARCHIVE_WARN) {
        if (stop.stop_requested()) {
            listing.error = ArchiveError::Cancelled;
            return listing;
        }
        if (++entries > MaxEntries) {
            listing.error = ArchiveError::TooLarge;
            return listing;
        }
        if (archive_entry_is_encrypted(entry)) {
            listing.error = ArchiveError::Encrypted;
            return listing;
        }
        if (!IsFile(entry)) continue;
        const auto size = archive_entry_size_is_set(entry) ? archive_entry_size(entry) : 0;
        listing.items.push_back({EntryPath(entry), static_cast<std::uint64_t>(std::max<la_int64_t>(size, 0))});
    }
    if (status != ARCHIVE_EOF) {
        listing.error = reader.Failure(entry);
        listing.detail = reader.Message();
        return listing;
    }
    if (const char* format = archive_format_name(reader.Get())) listing.format = format;
    return listing;
}

ArchiveError ExtractArchive(std::vector<std::unique_ptr<ArchiveSource>>& volumes,
                            const std::vector<LayoutFile>& files, const fs::path& destination,
                            std::uint64_t max_bytes, std::stop_token stop,
                            const ArchiveProgress& progress, std::string* detail) {
    const auto fail = [&](ArchiveError error, std::string message = {}) {
        if (detail != nullptr) *detail = std::move(message);
        return error;
    };
    std::unordered_map<std::string, const LayoutFile*> wanted;
    std::uint64_t total = 0;
    for (const auto& file : files) {
        if (!SafeRelativePath(file.to)) return fail(ArchiveError::Unreadable, "unsafe path " + file.to);
        wanted.emplace(file.from, &file);
        total += file.size;
    }
    Reader reader{volumes};
    if (!reader.Opened()) return fail(ArchiveError::Unreadable, reader.Message());

    const std::vector<std::uint8_t> zeros(BlockSize, 0);
    std::uint64_t written = 0;
    archive_entry* entry = nullptr;
    int status = ARCHIVE_OK;
    // `wanted` shrinks as files are written; reading stops as soon as it is empty.
    while (!wanted.empty() &&
           ((status = archive_read_next_header(reader.Get(), &entry)) == ARCHIVE_OK || status == ARCHIVE_WARN)) {
        if (!IsFile(entry)) continue;
        const auto match = wanted.find(EntryPath(entry));
        if (match == wanted.end()) continue;
        const LayoutFile& file = *match->second;
        wanted.erase(match);

        const auto target = destination / Utf8Path(file.to);
        const auto partial = fs::path{target}.concat(L".part");
        std::error_code ec;
        fs::create_directories(target.parent_path(), ec);
        std::ofstream out{partial, std::ios::binary | std::ios::trunc};
        if (!out) return fail(ArchiveError::Write, "cannot create " + file.to);
        const void* block = nullptr;
        std::size_t size = 0;
        la_int64_t offset = 0;
        std::uint64_t file_written = 0;
        int data = ARCHIVE_OK;
        while ((data = archive_read_data_block(reader.Get(), &block, &size, &offset)) == ARCHIVE_OK ||
               data == ARCHIVE_WARN) {
            if (stop.stop_requested()) return fail(ArchiveError::Cancelled);
            // Sparse entries leave holes: fill them with zeros up to the block's offset.
            const auto at = static_cast<std::uint64_t>(std::max<la_int64_t>(offset, 0));
            const std::uint64_t hole = at > file_written ? at - file_written : 0;
            if (written + hole + size > max_bytes) return fail(ArchiveError::TooLarge);
            for (std::uint64_t left = hole; left > 0;) {
                const auto step = std::min<std::uint64_t>(left, zeros.size());
                out.write(reinterpret_cast<const char*>(zeros.data()), static_cast<std::streamsize>(step));
                left -= step;
            }
            out.write(static_cast<const char*>(block), static_cast<std::streamsize>(size));
            if (!out) return fail(ArchiveError::Write, "cannot write " + file.to);
            file_written += hole + size;
            written += hole + size;
            if (progress) progress(written, std::max(total, written));
        }
        if (data != ARCHIVE_EOF) return fail(reader.Failure(entry), reader.Message());
        out.close();
        if (!out) return fail(ArchiveError::Write, "cannot write " + file.to);
        fs::rename(partial, target, ec);
        if (ec) return fail(ArchiveError::Write, "cannot place " + file.to);
    }
    if (wanted.empty()) return ArchiveError::None;
    if (status != ARCHIVE_EOF) return fail(reader.Failure(entry), reader.Message());
    return fail(ArchiveError::Unreadable, "missing " + wanted.begin()->first);
}

} // namespace EdenXbox::Mods
