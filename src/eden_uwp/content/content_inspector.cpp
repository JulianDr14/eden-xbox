// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#include "eden_uwp/content/content_inspector.h"
#include <array>
#include <span>
#include <cctype>
#include <map>
#include <set>
#include <openssl/evp.h>
#include "core/crypto/key_manager.h"
#include "core/file_sys/card_image.h"
#include "core/file_sys/common_funcs.h"
#include "core/file_sys/content_archive.h"
#include "core/file_sys/control_metadata.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/romfs.h"
#include "core/file_sys/submission_package.h"
#include "core/file_sys/partition_filesystem.h"
#include "core/loader/loader.h"

namespace EdenXbox::Content {
namespace {
using namespace FileSys;
std::string Hex(std::span<const u8> bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result;
    for (auto b : bytes) { result += digits[b >> 4]; result += digits[b & 15]; }
    return result;
}
Error Hash(const VirtualFile& file, std::array<u8, 32>& digest, std::stop_token stop,
           const Progress& progress, u64& done, u64 total) {
    if (!file) return Error::Unavailable;
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx{EVP_MD_CTX_new(), EVP_MD_CTX_free};
    if (!ctx || EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) != 1) return Error::Io;
    std::vector<u8> buffer(1024 * 1024);
    const auto size = file->GetSize();
    for (size_t offset = 0; offset < size;) {
        if (stop.stop_requested()) return Error::Cancelled;
        const auto bytes = std::min(buffer.size(), size - offset);
        if (file->Read(buffer.data(), bytes, offset) != bytes) return Error::Unavailable;
        if (EVP_DigestUpdate(ctx.get(), buffer.data(), bytes) != 1) return Error::Io;
        offset += bytes; done += bytes;
        if (progress) progress(done, total);
    }
    unsigned length{};
    return EVP_DigestFinal_ex(ctx.get(), digest.data(), &length) == 1 && length == digest.size() ?
        Error::None : Error::Io;
}
bool Gather(const VirtualDir& dir, std::vector<VirtualFile>& files, unsigned depth = 0) {
    if (!dir || depth > 3) return false;
    for (auto& file : dir->GetFiles()) {
        if (files.size() >= 4096) return false;
        files.push_back(std::move(file));
    }
    for (const auto& sub : dir->GetSubdirectories()) if (!Gather(sub, files, depth + 1)) return false;
    return true;
}
bool UsableStatus(Loader::ResultStatus status, bool update) {
    return status == Loader::ResultStatus::Success ||
        (update && status == Loader::ResultStatus::ErrorMissingBKTRBaseRomFS);
}
} // namespace

Error VerifyContent(InspectedContent& content, std::stop_token stop, Progress progress) {
    if (content.error != Error::None) return content.error;
    u64 done{}, total{};
    for (const auto& item : content.files) total += item.file->GetSize();
    for (const auto& item : content.files) {
        std::array<u8, 32> actual{};
        if (auto error = Hash(item.file, actual, stop, progress, done, total); error != Error::None)
            return content.error = error;
        if (actual != item.record.hash) return content.error = Error::HashMismatch;
    }
    content.revision.verified = true;
    return Error::None;
}

Inspection InspectPackage(FileSys::VfsFilesystem& vfs, const std::string& source,
                          bool verify, std::stop_token stop, Progress progress) {
    using namespace FileSys;
    Inspection result;
    std::vector<VirtualFile> raw;
    if (vfs.GetEntryType(source) == VfsEntryType::Directory) {
        if (!Gather(vfs.OpenDirectory(source, OpenMode::Read), raw)) return {Error::Invalid, {}};
    } else {
        const auto file = vfs.OpenFile(source, OpenMode::Read);
        if (!file) return {Error::Unavailable, {}};
        auto extension = file->GetExtension();
        std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return std::tolower(c); });
        if (extension == "nsp") {
            PartitionFilesystem nsp{file};
            if (nsp.GetStatus() != Loader::ResultStatus::Success) return {Error::Invalid, {}};
            raw = nsp.GetFiles();
        } else if (extension == "xci") {
            XCI xci{file};
            if (xci.GetStatus() != Loader::ResultStatus::Success || !xci.GetSecurePartitionNSP())
                return {Error::Invalid, {}};
            raw = xci.GetSecurePartitionNSP()->GetFiles();
        } else if (extension == "nca") raw.push_back(file);
        else return {Error::Unsupported, {}};
    }
    if (raw.size() > 4096) return {Error::Invalid, {}};
    std::map<std::string, VirtualFile> by_id;
    for (const auto& f : raw) {
        if (stop.stop_requested()) return {Error::Cancelled, {}};
        auto name = f->GetName();
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return std::tolower(c); });
        if (name.ends_with(".nca") && name.size() >= 36) {
            const auto id = name.substr(0, 32);
            if (!std::all_of(id.begin(), id.end(), [](unsigned char c) { return std::isxdigit(c); }) ||
                !by_id.emplace(id, f).second) return {Error::Invalid, {}};
        }
        if (f->GetExtension() == "tik") {
            if (f->GetSize() > 4096) return {Error::Invalid, {}};
            const auto ticket = Core::Crypto::Ticket::Read(f);
            if (ticket.IsValid()) Core::Crypto::KeyManager::Instance().AddTicket(ticket);
        }
    }
    bool missing_keys = false;
    for (const auto& [meta_id, file] : by_id) {
        if (stop.stop_requested()) return {Error::Cancelled, {}};
        NCA meta{file};
        if (meta.GetStatus() != Loader::ResultStatus::Success) { missing_keys = true; continue; }
        if (meta.GetType() != NCAContentType::Meta) continue;
        for (const auto& dir : meta.GetSubdirectories()) for (const auto& cnmt_file : dir->GetFiles()) {
            if (cnmt_file->GetExtension() != "cnmt") continue;
            auto cnmt = std::make_shared<CNMT>(cnmt_file);
            if (!cnmt->IsValid()) return {Error::Invalid, {}};
            if (cnmt->GetType() == TitleType::Application) continue;
            InspectedContent content;
            content.meta = file; content.cnmt = cnmt;
            auto& r = content.revision;
            r.source = source; r.title = cnmt->GetTitleID(); r.version = cnmt->GetTitleVersion();
            r.kind = cnmt->GetType() == TitleType::Update ? Kind::Update : Kind::Dlc;
            r.application = cnmt->GetApplicationId().value_or(0);
            r.required_application = cnmt->GetRequiredApplicationVersion();
            r.required_system = cnmt->GetRequiredSystemVersion();
            if ((cnmt->GetType() != TitleType::Update && cnmt->GetType() != TitleType::AOC) ||
                !r.application || cnmt->GetDataPatchId() != 0 || GetBaseTitleID(r.title) != r.application ||
                (r.kind == Kind::Update && r.title != GetUpdateTitleID(r.application)))
                content.error = Error::Unsupported;
            std::set<std::array<u8, 16>> seen;
            std::set<RightsId> rights;
            bool payload = false;
            r.bytes = file->GetSize();
            for (const auto& record : cnmt->GetContentRecords()) {
                if (record.type == ContentRecordType::DeltaFragment) continue;
                if (!seen.insert(record.nca_id).second) { content.error = Error::Invalid; break; }
                auto found = by_id.find(Hex(record.nca_id));
                if (found == by_id.end() || found->second->GetSize() != record.GetSize()) {
                    content.error = Error::Incomplete; break;
                }
                NCA nca{found->second};
                if (!UsableStatus(nca.GetStatus(), r.kind == Kind::Update)) {
                    content.error = Error::MissingKeys; break;
                }
                const auto program = nca.GetTitleId();
                if (GetBaseTitleID(program) != r.application || (r.kind == Kind::Dlc && program != r.title) ||
                    (GetCRTypeFromNCAType(nca.GetType()) != record.type && !(nca.GetType() == NCAContentType::Manual && record.type == ContentRecordType::LegalInformation))) {
                    content.error = Error::WrongGame; break;
                }
                payload |= record.type == (r.kind == Kind::Update ? ContentRecordType::Program : ContentRecordType::Data);
                rights.insert(nca.GetRightsId());
                content.files.push_back({record, found->second, program});
                r.bytes += record.GetSize();
                if (record.type == ContentRecordType::Control && nca.GetRomFS()) {
                    const auto control_dir = ExtractRomFS(nca.GetRomFS());
                    const auto control_file = control_dir ? control_dir->GetFile("control.nacp") : nullptr;
                    if (control_file && control_file->GetSize() >= sizeof(RawNACP)) {
                        NACP nacp{control_file};
                        r.name = nacp.GetApplicationName(); r.display_version = nacp.GetVersionString();
                    }
                }
            }
            if (!payload && content.error == Error::None) content.error = Error::Incomplete;
            for (const auto& f : raw) if (f->GetExtension() == "tik") {
                const auto ticket = Core::Crypto::Ticket::Read(f);
                if (ticket.IsValid() && rights.contains(ticket.GetData().rights_id)) {
                    content.tickets.push_back(f); r.bytes += f->GetSize();
                }
            }
            // Meta identity binds the payload hash list; this is integrity, not independent authenticity.
            std::array<u8, 32> digest{}; u64 done{};
            if (auto error = Hash(file, digest, stop, {}, done, file->GetSize()); error != Error::None)
                return {error, {}};
            std::array<u8, 44> identity{};
            std::copy(digest.begin(), digest.end(), identity.begin());
            for (size_t i=0; i<8; ++i) identity[32+i] = static_cast<u8>(r.title >> (8*i));
            for (size_t i=0; i<4; ++i) identity[40+i] = static_cast<u8>(r.version >> (8*i));
            unsigned length{};
            if (EVP_Digest(identity.data(), identity.size(), digest.data(), &length, EVP_sha256(), nullptr) != 1 || length != digest.size()) return {Error::Io, {}};
            r.id = Hex(digest);
            if (vfs.GetEntryType(source) == VfsEntryType::File)
                if (const auto f = vfs.OpenFile(source, OpenMode::Read)) r.source_size = f->GetSize();
            if (verify && content.error == Error::None) VerifyContent(content, stop, progress);
            result.contents.push_back(std::move(content));
        }
    }
    if (result.contents.empty()) result.error = missing_keys ? Error::MissingKeys : source.ends_with(".nca") ? Error::Incomplete : Error::Unsupported;
    return result;
}

void RegisterContent(FileSys::ManualContentProvider& provider, const InspectedContent& content) {
    using namespace FileSys;
    const auto& r = content.revision;
    const auto type = r.kind == Kind::Update ? TitleType::Update : TitleType::AOC;
    provider.AddEntryWithVersion(type, ContentRecordType::Meta, r.title, r.version, r.display_version, content.meta);
    for (const auto& file : content.files)
        provider.AddEntryWithVersion(type, file.record.type, r.kind == Kind::Update ? GetUpdateTitleID(file.program) : r.title, r.version, r.display_version, file.file);
}
} // namespace EdenXbox::Content
