// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <d3d12.h>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;
static void Check(HRESULT hr) { if (FAILED(hr)) throw std::runtime_error("D3D12 failed"); }
int main() try {
    ComPtr<ID3D12Device> device;
    Check(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)));
    ComPtr<ID3D12DescriptorHeap> src,dst;
    D3D12_DESCRIPTOR_HEAP_DESC desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,4096,D3D12_DESCRIPTOR_HEAP_FLAG_NONE,0};
    Check(device->CreateDescriptorHeap(&desc,IID_PPV_ARGS(&src)));
    desc.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    Check(device->CreateDescriptorHeap(&desc,IID_PPV_ARGS(&dst)));
    const auto stride=device->GetDescriptorHandleIncrementSize(desc.Type);
    const auto source=src->GetCPUDescriptorHandleForHeapStart(),destination=dst->GetCPUDescriptorHandleForHeapStart();
    const D3D12_SHADER_RESOURCE_VIEW_DESC srv{.Format=DXGI_FORMAT_R32_UINT,.ViewDimension=D3D12_SRV_DIMENSION_BUFFER,.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,.Buffer={.FirstElement=0,.NumElements=1}};
    for (UINT i=0;i<4096;++i) device->CreateShaderResourceView(nullptr,&srv,{source.ptr+SIZE_T(i)*stride});
    for(UINT count:{1u,2u,4u,8u,16u}) {
        std::array<D3D12_CPU_DESCRIPTOR_HANDLE,16> handles{};
        for(UINT i=0;i<count;++i) handles[i]={source.ptr+SIZE_T(i)*stride};
        std::array<double,9> old{},simple{};
        for(UINT sample=0;sample<9;++sample) for(UINT pass=0;pass<2;++pass) {
            const bool optimized=(sample+pass)%2;
            const auto begin=std::chrono::steady_clock::now();
            for(UINT i=0;i<200000;++i) {
                const D3D12_CPU_DESCRIPTOR_HANDLE target{destination.ptr+SIZE_T((i%128)*16)*stride};
                if(optimized) device->CopyDescriptorsSimple(count,target,source,desc.Type);
                else device->CopyDescriptors(1,&target,&count,count,handles.data(),nullptr,desc.Type);
            }
            (optimized?simple:old)[sample]=std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now()-begin).count()/200000;
        }
        std::sort(old.begin(),old.end());std::sort(simple.begin(),simple.end());
        std::cout<<count<<" contiguous descriptors, CPU ns/batch median "<<old[4]<<" -> "<<simple[4]<<" ranges "<<old.front()<<".."<<old.back()<<" / "<<simple.front()<<".."<<simple.back()<<'\n';
    }
    Check(device->GetDeviceRemovedReason());
} catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
