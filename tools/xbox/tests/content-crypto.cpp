// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <thread>
#include "common/logging.h"
#include "core/crypto/ctr_encryption_layer.h"
#include "core/file_sys/fssystem/fssystem_aes_ctr_storage.h"
#include "core/file_sys/fssystem/fssystem_aes_xts_storage.h"
#include "core/file_sys/vfs/vfs_vector.h"
void AssertFatalImpl(){std::abort();}
void AssertFailSoftImpl(){std::abort();}
void AssertFailedAt(const char*){std::abort();}
void UnreachableAt(const char*){std::abort();}
namespace Common::Log { void FmtLogMessageImpl(Class,Level,const char*,unsigned,const char*,fmt::string_view,const fmt::format_args&){} }
void Check(bool ok) { if (!ok) std::abort(); }
using namespace Core::Crypto;
using namespace FileSys;
template <class Reader> void Concurrent(Reader& reader, const std::vector<u8>& plain, bool unaligned) {
    std::atomic<bool> good{true}; std::vector<std::jthread> workers;
    for (unsigned thread=0; thread<4; ++thread) workers.emplace_back([&,thread] {
        std::vector<u8> data(512);
        for (unsigned i=0; i<5000; ++i) {
            const size_t offset = ((i*17+thread*31)%30)*512 + (unaligned ? thread : 0);
            if (reader.Read(data.data(),data.size(),offset)!=data.size() ||
                !std::equal(data.begin(),data.end(),plain.begin()+offset)) good=false;
        }
    });
    workers.clear(); Check(good);
}
int main() {
    Key128 key{}; for(size_t i=0;i<key.size();++i) key[i]=static_cast<u8>(i+1);
    std::array<u8,16> iv{};
    std::vector<u8> plain(16384); for(size_t i=0;i<plain.size();++i) plain[i]=static_cast<u8>(i*17+(i>>8));
    auto encrypted=plain;
    AESCipher<Key128> cipher{key,Mode::CTR}; cipher.SetIV(iv);
    cipher.Transcode(encrypted.data(),encrypted.size(),encrypted.data(),Op::Encrypt);
    auto file=std::make_shared<VectorVfsFile>(encrypted);
    AesCtrStorage storage{file,key.data(),key.size(),iv.data(),iv.size()};
    Concurrent(storage,plain,false);
    CTREncryptionLayer layer{file,key,0}; layer.SetIV(iv); Concurrent(layer,plain,true);
    file->Resize(511); std::array<u8,512> out{}; Check(storage.Read(out.data(),512,0)==0);
    Check(layer.Read(out.data(),512,0)==511);
    Key256 xts_key{}; for(size_t i=0;i<xts_key.size();++i) xts_key[i]=static_cast<u8>(i+1);
    encrypted=plain; AESCipher<Key256> xts{xts_key,Mode::XTS};
    for(size_t sector=0;sector<plain.size()/512;++sector) {
        iv.fill(0); iv.back()=static_cast<u8>(sector); xts.SetIV(iv);
        xts.Transcode(encrypted.data()+sector*512,512,encrypted.data()+sector*512,Op::Encrypt);
    }
    iv.fill(0); file=std::make_shared<VectorVfsFile>(encrypted);
    AesXtsStorage xs{file,xts_key.data(),xts_key.data()+16,xts_key.size(),iv.data(),iv.size(),512};
    Concurrent(xs,plain,false);
    file->Resize(511); Check(xs.Read(out.data(),512,0)==0);
    std::puts("PASS content crypto: 60000 concurrent CTR/unaligned CTR/XTS reads and short-read propagation");
}
