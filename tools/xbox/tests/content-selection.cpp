// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdlib>
#include <cstdio>
#include "common/logging.h"
#include "core/file_sys/content_archive.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/vfs/vfs_vector.h"
void AssertFatalImpl(){std::abort();}
void AssertFailSoftImpl(){std::abort();}
void AssertFailedAt(const char*){std::abort();}
void UnreachableAt(const char*){std::abort();}
namespace Common::Log { void FmtLogMessageImpl(Class,Level,const char*,unsigned,const char*,fmt::string_view,const fmt::format_args&){} }
void Check(bool ok) { if (!ok) std::abort(); }
class FixtureProvider final : public FileSys::ContentProvider {
public:
    struct Row { FileSys::TitleType title_type; FileSys::ContentRecordType type; u64 title; u32 version; FileSys::VirtualFile file; };
    std::vector<Row> rows;
    void AddEntry(FileSys::TitleType tt,FileSys::ContentRecordType t,u64 id,FileSys::VirtualFile f) { rows.push_back({tt,t,id,0,std::move(f)}); }
    void AddEntryWithVersion(FileSys::TitleType tt,FileSys::ContentRecordType t,u64 id,u32 v,const char*,FileSys::VirtualFile f) { rows.push_back({tt,t,id,v,std::move(f)}); }
    void Refresh() override {}
    bool HasEntry(u64 id,FileSys::ContentRecordType t) const override { return bool(GetEntryRaw(id,t)); }
    std::optional<u32> GetEntryVersion(u64 id) const override { for(const auto& r:rows) if(r.title==id) return r.version; return {}; }
    FileSys::VirtualFile GetEntryUnparsed(u64 id,FileSys::ContentRecordType t) const override { return GetEntryRaw(id,t); }
    FileSys::VirtualFile GetEntryRaw(u64 id,FileSys::ContentRecordType t) const override { for(const auto& r:rows) if(r.title==id && r.type==t) return r.file; return {}; }
    std::unique_ptr<FileSys::NCA> GetEntry(u64,FileSys::ContentRecordType) const override { return {}; }
    std::vector<FileSys::ContentProviderEntry> ListEntriesFilter(std::optional<FileSys::TitleType> tt,std::optional<FileSys::ContentRecordType> t,std::optional<u64> id) const override {
        std::vector<FileSys::ContentProviderEntry> out;
        for(const auto& r:rows) if((!tt||*tt==r.title_type)&&(!t||*t==r.type)&&(!id||*id==r.title)) out.push_back({r.title,r.type});
        return out;
    }
};
int main() {
    using namespace FileSys;
    constexpr u64 app = 0x0100123400000000ULL, other = 0x0100567800000000ULL;
    FixtureProvider nand, selected;
    auto old_file = std::make_shared<VectorVfsFile>(std::vector<u8>{99});
    auto new_file = std::make_shared<VectorVfsFile>(std::vector<u8>{5});
    nand.AddEntryWithVersion(TitleType::Update, ContentRecordType::Program, app|0x800, 99, "99", old_file);
    nand.AddEntry(TitleType::AOC, ContentRecordType::Data, app|0x1001, old_file);
    nand.AddEntry(TitleType::Application, ContentRecordType::Program, other, old_file);
    selected.AddEntry(TitleType::Application, ContentRecordType::Program, app, new_file);
    selected.AddEntry(TitleType::Application, ContentRecordType::Program, app+1, new_file);
    ContentProviderUnion providers{};
    providers.SetSlot(ContentProviderUnionSlot::UserNAND, &nand);
    providers.SetSlot(ContentProviderUnionSlot::FrontendManual, &selected);
    Check(providers.HasEntry(app|0x800, ContentRecordType::Program));
    providers.SetApplicationContentOverride(app, &selected);
    Check(!providers.HasEntry(app|0x800, ContentRecordType::Program));
    Check(!providers.GetEntryRaw(app|0x1001, ContentRecordType::Data));
    Check(!providers.GetSlotForEntry(app|0x1001, ContentRecordType::Data));
    Check(providers.ListEntriesFilter(TitleType::AOC, {}, {}).empty());
    Check(providers.ListEntriesFilterOrigin({}, TitleType::AOC, {}, {}).empty());
    Check(providers.HasEntry(other, ContentRecordType::Program));
    Check(providers.HasEntry(app+1, ContentRecordType::Program));
    // Explicit downgrade must win over a higher version still present on NAND.
    selected.AddEntryWithVersion(TitleType::Update, ContentRecordType::Program, app|0x800, 5, "5", new_file);
    Check(providers.GetEntryVersion(app|0x800)==5);
    Check(providers.GetEntryRaw(app|0x800, ContentRecordType::Program)==new_file);
    const auto origin=providers.ListEntriesFilterOrigin({}, TitleType::Update, {}, {});
    Check(origin.size()==1 && origin[0].first==ContentProviderUnionSlot::FrontendManual);
    providers.SetApplicationContentOverride(app, nullptr);
    Check(providers.GetEntryRaw(app|0x800, ContentRecordType::Program)==old_file);
    std::puts("PASS content selection: exclusive lookup/enumeration/origin, DLC disabled, explicit downgrade, other apps/programs preserved");
}
