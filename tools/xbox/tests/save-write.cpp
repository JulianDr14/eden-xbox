// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
// Standalone regression gate linked with the production UWP core/common libraries.
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <vector>
#include "common/logging.h"
#include "common/host_memory.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/file_sys/fsa/fs_i_file.h"
void AssertFatalImpl(){std::abort();}
void AssertFailSoftImpl(){std::abort();}
void AssertFailedAt(const char*){std::abort();}
void UnreachableAt(const char*){std::abort();}
namespace Common::Log { void FmtLogMessageImpl(Class,Level,const char*,unsigned,const char*,fmt::string_view,const fmt::format_args&){} }
int main(){
using namespace FileSys;
const auto path=std::filesystem::temp_directory_path()/"eden-save-write-regression.bin";
RealVfsFilesystem fs;
const auto p=path.string();
std::vector<u8> data(2307552);
for(size_t i=0;i<data.size();++i) data[i]=static_cast<u8>(i*17+3);
auto created=fs.CreateFile(p,OpenMode::ReadWrite); assert(created);
assert(created->Resize(data.size())); created.reset();
for (const auto mode : {OpenMode::Write,OpenMode::ReadWrite,OpenMode::Write|OpenMode::AllowAppend,OpenMode::All}) {
    auto reader=fs.OpenFile(p,OpenMode::Read); assert(reader && !reader->IsWritable());
    u8 byte{}; assert(reader->Read(&byte,1,0)==1);
    auto writer=fs.OpenFile(p,mode); assert(writer && writer!=reader && writer->IsWritable());
    assert(fs.OpenFile(p,mode)==writer);
    assert(writer->Write(data.data(),data.size(),0)==data.size());
    // AllowAppend must still respect the explicit offset, without truncating other bytes.
    const u8 patch=0x91; assert(writer->Write(&patch,1,11)==1);
    writer.reset(); reader.reset();
    auto verify=fs.OpenFile(p,OpenMode::Read);
    auto expected=data; expected[11]=patch;
    assert(verify->GetSize()==data.size() && verify->ReadAllBytes()==expected);
    Fsa::IFile readonly{verify};
    assert(readonly.Write(0,&patch,1,WriteOption::None).IsError());
}
// Writer-first must never turn a subsequently requested read-only VFS into a writable one.
{
    auto writer=fs.OpenFile(p,OpenMode::ReadWrite);
    auto reader=fs.OpenFile(p,OpenMode::Read);
    assert(writer!=reader && !reader->IsWritable());
}
#ifdef _WIN32
// Real UWP demand-commit backing: untouched pages must also be writable to disk.
{
    Common::HostMemory lazy(4*1024*1024,8*1024*1024);
    auto* source=lazy.BackingBasePointer();
    source[0]=0x37;
    source[data.size()-1]=0x81;
    auto writer=fs.OpenFile(p,OpenMode::ReadWrite);
    assert(writer->Write(source,data.size(),0)==data.size());
    writer.reset();
    auto reader=fs.OpenFile(p,OpenMode::Read);
    auto result=reader->ReadAllBytes();
    std::vector<u8> expected(data.size(),0);
    expected.front()=0x37;
    expected.back()=0x81;
    assert(result==expected);
}
#endif
std::filesystem::remove(path);
std::cout<<"PASS: read/write mode isolation, offset writes, 2307552-byte persistence, reopen and failed-write result\n";
}
