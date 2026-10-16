// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <iostream>
#include <numeric>
#include <vector>
#include "video_core/renderer_d3d12/d3d12_graphics_pipeline.h"
using Key = D3D12::GraphicsPipelineCacheKey;
static size_t Old(std::vector<Key>& keys, std::vector<u32>& values, const Key& query) {
    for (size_t i=0;i<keys.size();++i) {
        if (keys[i]!=query) continue;
        const u32 result=values[i];
        if(i){std::swap(keys[i],keys[i-1]);std::swap(values[i],values[i-1]);}
        return result;
    }
    return keys.size();
}
int main() {
    std::cout<<"key bytes "<<sizeof(Key)<<"\n";
    for (u32 mode=0; mode<4; ++mode) {
        Key a{};
        a.state.dynamic_vertex_input.Assign(mode == 1);
        a.state.extended_dynamic_state.Assign(mode == 2);
        a.state.xfb_enabled.Assign(mode == 3);
        const size_t prefix=sizeof(a.unique_hashes)+a.state.Size();
        const size_t extra=offsetof(Key,depth_bias);
        for(size_t byte=0;byte<sizeof(Key);++byte) {
            Key b=a;
            reinterpret_cast<u8*>(&b)[byte]^=0x40;
            const bool expected=(byte>=prefix && byte<extra);
            // Changing a flag that changes Size() cannot be judged only by byte position.
            const bool equal=a==b;
            if (a.state.Size()==b.state.Size()) assert(equal==expected);
            std::vector<Key> keys{a}; std::vector<u32> order{0};
            assert((D3D12::FindPipelineTransition<Key>(keys,order,b)==0)==equal);
        }
    }
    for(bool tail:{false,true}) for(u32 count:{1u,8u,32u,128u}) {
        std::vector<Key> keys(count);
        for(u32 i=0;i<count;++i) {
            if(tail)keys[i].depth_bias=i+1; else keys[i].unique_hashes[0]=i+1;
        }
        // Differential validation including misses, full equality and promotion order.
        auto old=keys;
        std::vector<u32> values(count), order(count);
        std::iota(values.begin(),values.end(),0); order=values;
        for(u32 i=0;i<10000;++i) {
            Key query=keys[(i*1777u)%count];
            if(i%17==0)query.stencil_write_mask=255;
            auto a=Old(old,values,query);
            auto b=D3D12::FindPipelineTransition<Key>(keys,order,query);
            assert(a==b && values==order);
        }
        for(u32 workload:{0u,1u,2u}) {
            std::array<double,7> before{},after{};
            u64 checksum[2]{};
            constexpr u32 N=100000;
            for(u32 sample=0;sample<7;++sample)for(u32 turn=0;turn<2;++turn){
                bool optimized=(sample+turn)%2;
                old=keys;std::iota(values.begin(),values.end(),0);order=values;
                auto start=std::chrono::steady_clock::now();
                for(u32 i=0;i<N;++i) {
                    const u32 selected=workload==0?0:workload==1?i%count:(i%10?0:(i*1777u)%count);
                    const auto& query=keys[selected];
                    checksum[optimized]+=optimized?D3D12::FindPipelineTransition<Key>(keys,order,query):Old(old,values,query);
                }
                auto ns=std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now()-start).count()/N;
                (optimized?after:before)[sample]=ns;
            }
            assert(checksum[0]==checksum[1]);
            std::sort(before.begin(),before.end());std::sort(after.begin(),after.end());
            std::cout<<"tail="<<tail<<" count="<<count<<" workload="<<workload<<" median ns "<<before[3]<<" -> "<<after[3]<<" ranges "<<before.front()<<"/"<<before.back()<<" -> "<<after.front()<<"/"<<after.back()<<"\n";
        }
    }
    std::cout<<"PASS exact keys, misses, identical promotion and results\n";
}
