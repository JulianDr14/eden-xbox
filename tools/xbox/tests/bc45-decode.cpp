// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>
#include <bc_decoder.h>
#include "video_core/texture_cache/bc45_decode.h"

using Clock = std::chrono::steady_clock;
using namespace VideoCommon;
template<unsigned Channels, bool Signed>
void Reference(const std::uint8_t* src, std::uint8_t* dst, size_t width, size_t height, size_t row) {
    for (size_t y = 0; y < height; y += 4)
        for (size_t x = 0; x < width; x += 4) {
            const auto* block = src + (y / 4 * row / 4 + x / 4) * Channels * 8;
            auto* out = dst + (y * width + x) * Channels;
            if constexpr(Channels == 1) bcn::DecodeBc4(block, out, x, y, width, height, Signed);
            else bcn::DecodeBc5(block, out, x, y, width, height, Signed);
        }
}
template<unsigned Channels, bool Signed>
void Exhaustive() {
    for (unsigned endpoints = 0; endpoints < 65536; ++endpoints) {
        std::array<std::uint8_t, 17> input{};
        for (unsigned c = 0; c < Channels; ++c) {
            std::uint64_t bits = c ? (endpoints ^ 65535) : endpoints;
            for (unsigned texel = 0; texel < 16; ++texel)
                bits |= static_cast<std::uint64_t>((texel + c * 3) & 7) << (16 + texel * 3);
            std::memcpy(input.data() + 1 + c * 8, &bits, 8);
        }
        // Odd pointer and extra columns detect writes beyond each block row.
        std::array<std::uint8_t, 81> expected, result;
        expected.fill(0xcd); result.fill(0xcd);
        if constexpr(Channels == 1) bcn::DecodeBc4(input.data()+1,expected.data()+1,0,0,8,4,Signed);
        else bcn::DecodeBc5(input.data()+1,expected.data()+1,0,0,8,4,Signed);
        Bc45::DecodeBlock<Channels,Signed>(input.data()+1,result.data()+1,8*Channels);
        assert(expected == result);
    }
}
template<unsigned Channels,bool Signed>
void Images() {
    std::uint32_t random=777;
    for (size_t width: {4,8,128}) for(size_t height: {4,12,128*48}) for(size_t extra:{0,4,12}) {
        const size_t row=width+extra;
        std::vector<std::uint8_t> input(row*height/16*Channels*8+1);
        for(auto& b:input){random^=random<<13;random^=random>>17;random^=random<<5;b=static_cast<std::uint8_t>(random);}
        std::vector<std::uint8_t> a(width*height*Channels+2,0xcd),b=a;
        Reference<Channels,Signed>(input.data()+1,a.data()+1,width,height,row);
        Bc45::DecodeFullBlocks<Channels,Signed>(input.data()+1,b.data()+1,width,height,row);
        assert(a==b);
    }
}
template<bool Signed>
void Bench() {
    constexpr size_t width=128,height=128*48;
    std::vector<std::uint8_t> input(width*height), output(width*height*2);
    std::uint32_t seed=123;
    for(auto& b:input){seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;b=static_cast<std::uint8_t>(seed);}
    std::vector<double> old_times,new_times;
    for(int i=0;i<9;++i){
        auto start=Clock::now();Reference<2,Signed>(input.data(),output.data(),width,height,width);
        auto end=Clock::now();old_times.push_back(std::chrono::duration<double,std::milli>(end-start).count());
        start=Clock::now();Bc45::DecodeFullBlocks<2,Signed>(input.data(),output.data(),width,height,width);
        end=Clock::now();new_times.push_back(std::chrono::duration<double,std::milli>(end-start).count());
    }
    std::sort(old_times.begin(),old_times.end());std::sort(new_times.begin(),new_times.end());
    std::cout<<(Signed?"SNORM":"UNORM")<<" BC5 128x128x48: old "<<old_times[4]<<" ms, candidate "<<new_times[4]<<" ms, speedup "<<old_times[4]/new_times[4]<<"x\n";
}
int main(){
    Exhaustive<1,false>();Exhaustive<1,true>();Exhaustive<2,false>();Exhaustive<2,true>();
    Images<1,false>();Images<1,true>();Images<2,false>();Images<2,true>();
    std::cout<<"262144 endpoint/block cases +108 strided/unaligned images: PASS\n";
    Bench<false>();Bench<true>();
}
