// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <iostream>
#include <vector>
#include <d3d12sdklayers.h>
#include "common/logging.h"
#include "common/settings.h"
#include "video_core/renderer_d3d12/d3d12_device.h"
#include "video_core/renderer_d3d12/d3d12_scheduler.h"
#include "video_core/renderer_d3d12/d3d12_resource_utils.h"
#include "common/write_combined_copy.h"
using namespace D3D12;
__declspec(noinline) static void Original(void* d,const void* s,size_t n){std::memcpy(d,s,n);}
__declspec(noinline) static void Candidate(void* d,const void* s,size_t n){Common::CopyToWriteCombined(d,s,n);}
int main(int argc, char**){
    Common::Log::Initialize();Common::Log::Start();Settings::values.renderer_debug=true;
    const bool threaded = argc > 2;
    SetAsyncRecording(threaded);
    Device device;Scheduler scheduler{device};
    ComPtr<ID3D12InfoQueue> messages;ThrowIfFailed(device.Get()->QueryInterface(IID_PPV_ARGS(&messages)),"debug");messages->ClearStoredMessages();
    constexpr size_t capacity=8*1024*1024;
    auto upload=CreateCommittedBuffer(device.Get(),capacity,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ,D3D12_RESOURCE_FLAG_NONE,"copy upload");
    auto readback=CreateCommittedBuffer(device.Get(),capacity,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_FLAG_NONE,"copy readback");
    void* mapped{};const D3D12_RANGE no_read{};ThrowIfFailed(upload->Map(0,&no_read,&mapped),"map upload");
    auto* output=static_cast<u8*>(mapped);
    std::vector<u8> source(8192),expected;
    for(size_t i=0;i<source.size();++i)source[i]=static_cast<u8>(i*37+i/19);
    // Preserve sentinels before and after every copy, including unaligned destinations and
    // non-vector tails. Only read the GPU readback; mapped upload is write-combined.
    for(size_t size:{0,1,13,15,16,17,31,32,63,64,65,127,128,255,256,257,511,512,513,1024,4096})
    for(size_t dst_offset=0;dst_offset<16;++dst_offset)for(size_t src_offset=0;src_offset<16;++src_offset){
        const size_t count=size+64,base=expected.size();assert(base+count<=capacity);
        std::memset(output+base,0xA5,count);
        Candidate(output+base+dst_offset+16,source.data()+src_offset,size);
        expected.insert(expected.end(),count,0xA5);
        std::copy_n(source.data()+src_offset,size,expected.data()+base+dst_offset+16);
    }
    // Source ends immediately before a no-access page: no SIMD or tail copy may over-read.
    auto* guarded=static_cast<u8*>(VirtualAlloc(nullptr,8192,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));assert(guarded);
    for(size_t i=0;i<4096;++i)guarded[i]=static_cast<u8>(i*11+i/7);
    DWORD previous{};assert(VirtualProtect(guarded+4096,4096,PAGE_NOACCESS,&previous));
    for(size_t n=0;n<=512;++n){
        const size_t pad=(16-expected.size()%16)%16;
        std::memset(output+expected.size(),0xA5,pad);expected.insert(expected.end(),pad,0xA5);
        const size_t base=expected.size(),count=n+32;assert(base+count<=capacity);
        std::memset(output+base,0xA5,count);
        Candidate(output+base+16,guarded+4096-n,n);
        expected.insert(expected.end(),count,0xA5);
        std::copy_n(guarded+4096-n,n,expected.data()+base+16);
    }
    VirtualFree(guarded,0,MEM_RELEASE);
    scheduler.CommandList()->CopyBufferRegion(readback.Get(),0,upload.Get(),0,expected.size());scheduler.Finish();
    void* result{};const D3D12_RANGE data{0,expected.size()};ThrowIfFailed(readback->Map(0,&data,&result),"readback");
    assert(std::equal(expected.begin(),expected.end(),static_cast<const u8*>(result)));readback->Unmap(0,&no_read);
    for(UINT64 i=0;i<messages->GetNumStoredMessages();++i){SIZE_T n{};messages->GetMessage(i,nullptr,&n);std::vector<std::byte> storage(n);auto* m=reinterpret_cast<D3D12_MESSAGE*>(storage.data());ThrowIfFailed(messages->GetMessage(i,m,&n),"message");if(m->Severity<=D3D12_MESSAGE_SEVERITY_WARNING)throw std::runtime_error(m->pDescription);}
    std::cout<<"PASS GPU async="<<threaded<<" exact "<<expected.size()<<" bytes, 5889 size/alignment/tail/guard-page cases, sentinels, debug clean\n";
    if(argc>1){upload->Unmap(0,nullptr);return 0;}
    Settings::values.renderer_debug=false;
    const HMODULE app_crt=LoadLibraryW(L"vcruntime140_app.dll");
    if(!app_crt)throw std::runtime_error("Store CRT unavailable: do not use desktop memcpy timings as Xbox proof");
    using CopyFunction=void* (__cdecl*)(void*,const void*,size_t);
    const auto app_copy=reinterpret_cast<CopyFunction>(GetProcAddress(app_crt,"memcpy"));assert(app_copy);
    for(bool store:{false,true})for(size_t size:{64,128,256,512,1024}){
        std::array<double,7> before{},after{};
        for(u32 sample=0;sample<7;++sample)for(u32 turn=0;turn<2;++turn){
            bool candidate=(sample+turn)%2;
            constexpr u32 N=100000;
            auto start=std::chrono::steady_clock::now();
            for(u32 i=0;i<N;++i){auto* dst=output+(i%1024)*1024;auto* src=source.data()+i%16;if(candidate && size<=512)Candidate(dst,src,size);else if(store)app_copy(dst,src,size);else Original(dst,src,size);}
            (candidate?after:before)[sample]=std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now()-start).count()/N;
        }
        std::sort(before.begin(),before.end());std::sort(after.begin(),after.end());
        std::cout<<"StoreCRT="<<store<<" UPLOAD "<<size<<" median ns "<<before[3]<<" -> "<<after[3]<<" range "<<before.front()<<"/"<<before.back()<<" -> "<<after.front()<<"/"<<after.back()<<"\n";
    }
    upload->Unmap(0,nullptr);
}
