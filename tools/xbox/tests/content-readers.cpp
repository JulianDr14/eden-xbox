// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
// Production readers with synthetic bytes; no keys, firmware or game data.
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include "common/logging.h"
#include "core/file_sys/nca_metadata.h"
#include "core/file_sys/partition_filesystem.h"
#include "core/file_sys/vfs/vfs_vector.h"
#include "core/loader/loader.h"
void AssertFatalImpl(){std::abort();}
void AssertFailSoftImpl(){std::abort();}
void AssertFailedAt(const char*){std::abort();}
void UnreachableAt(const char*){std::abort();}
namespace Common::Log { void FmtLogMessageImpl(Class,Level,const char*,unsigned,const char*,fmt::string_view,const fmt::format_args&){} }
using namespace FileSys;
void Check(bool ok) { if (!ok) std::abort(); }
template <class T> void Put(std::vector<u8>& bytes, size_t offset, const T& object) {
    Check(offset + sizeof(T) <= bytes.size()); std::memcpy(bytes.data()+offset, &object, sizeof(T));
}
bool PfsOk(std::vector<u8> bytes) {
    PartitionFilesystem pfs{std::make_shared<VectorVfsFile>(std::move(bytes))};
    return pfs.GetStatus() == Loader::ResultStatus::Success;
}
std::vector<u8> Pfs(bool hfs = false) {
    const size_t table = hfs ? 64 : 24;
    std::vector<u8> bytes(16+table+2+4);
    Put(bytes, 0, hfs ? u32{0x30534648} : u32{0x30534650});
    Put(bytes, 4, u32{1}); Put(bytes, 8, u32{2});
    Put(bytes, 24, u64{4}); bytes[16+table]='a';
    return bytes;
}
int main() {
    for (bool hfs : {false,true}) {
        const auto valid=Pfs(hfs); Check(PfsOk(valid));
        for (size_t size=0;size<valid.size();++size) {
            auto bad=valid; bad.resize(size); Check(!PfsOk(bad));
        }
        auto bad=valid; Put(bad,4,u32{0xffffffff}); Check(!PfsOk(bad));
        bad=valid; Put(bad,8,u32{0xffffffff}); Check(!PfsOk(bad));
        bad=valid; Put(bad,16,u64{~u64{}}); Check(!PfsOk(bad));
        bad=valid; Put(bad,24,u64{~u64{}}); Check(!PfsOk(bad));
        bad=valid; Put(bad,32,u32{2}); Check(!PfsOk(bad));
        bad=valid; bad[16+(hfs?64:24)+1]='x'; Check(!PfsOk(bad));
        bad=valid; bad[16+(hfs?64:24)]='/'; Check(!PfsOk(bad));
    }
    for (auto type : {TitleType::Update,TitleType::AOC}) {
        CNMTHeader header{}; header.title_id=0x0100123400000800ULL;
        header.title_version=65536; header.type=type; header.table_offset=24;
        header.number_content_entries=1; header.number_meta_entries=1;
        OptionalHeader ext{}; ext.title_id=0x0100123400000000ULL; ext.minimum_version=65536;
        ContentRecord content{}; content.type=ContentRecordType::Data;
        content.size[4]=1; content.size[5]=0x80;
        MetaRecord meta{}; meta.title_id=0x123456789ULL; meta.title_version=99;
        CNMT generated{header,ext,{content},{meta}};
        const auto bytes=generated.Serialize();
        CNMT parsed{std::make_shared<VectorVfsFile>(bytes)};
        Check(parsed.IsValid() && parsed.GetApplicationId()==u64{ext.title_id});
        Check(parsed.GetContentRecords()[0].GetSize()==(u64{1}<<32));
        Check(parsed.GetContentRecords()[0].GetAttributes()==0x80);
        Check(parsed.GetMetaRecords()[0].title_id==meta.title_id);
        Check(parsed.Serialize()==bytes);
        for(size_t size=0;size<bytes.size();++size) {
            auto bad=bytes; bad.resize(size);
            Check(!CNMT{std::make_shared<VectorVfsFile>(bad)}.IsValid());
        }
        auto bad=bytes; Put(bad,14,u16{65535});
        Check(!CNMT{std::make_shared<VectorVfsFile>(bad)}.IsValid());
        bad=bytes; bad[32+24+54]=0xff;
        Check(!CNMT{std::make_shared<VectorVfsFile>(bad)}.IsValid());
    }
    std::puts("PASS content readers: truncated PFS/HFS/CNMT, ranges, overflow, names, attributes, metadata offset");
}
