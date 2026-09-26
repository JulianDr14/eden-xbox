// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <stdexcept>

#include <fmt/format.h>

#include "common/cityhash.h"
#include "common/logging.h"
#include "video_core/renderer_d3d12/d3d12_descriptor_heap.h"
#include "video_core/renderer_d3d12/d3d12_scheduler.h"

namespace D3D12 {

namespace {

ComPtr<ID3D12DescriptorHeap> CreateHeap(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type,
                                        u32 count, bool shader_visible) {
    const D3D12_DESCRIPTOR_HEAP_DESC desc{
        .Type = type,
        .NumDescriptors = count,
        .Flags = shader_visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
                                : D3D12_DESCRIPTOR_HEAP_FLAG_NONE,
        .NodeMask = 0,
    };
    ComPtr<ID3D12DescriptorHeap> heap;
    ThrowIfFailed(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap)),
                  "CreateDescriptorHeap");
    return heap;
}

} // Anonymous namespace

// --- CpuDescriptorAllocator -------------------------------------------------------------------

CpuDescriptorAllocator::CpuDescriptorAllocator(ID3D12Device* device_,
                                               D3D12_DESCRIPTOR_HEAP_TYPE type_, u32 page_size_)
    : device{device_}, type{type_}, page_size{page_size_},
      stride{device_->GetDescriptorHandleIncrementSize(type_)} {}

D3D12_CPU_DESCRIPTOR_HANDLE CpuDescriptorAllocator::Allocate() {
    std::scoped_lock lock{mutex};
    if (free_list.empty()) {
        AddPage();
    }
    const D3D12_CPU_DESCRIPTOR_HANDLE handle = free_list.back();
    free_list.pop_back();
    return handle;
}

void CpuDescriptorAllocator::Free(D3D12_CPU_DESCRIPTOR_HANDLE handle) {
    if (handle.ptr == 0) {
        return;
    }
    std::scoped_lock lock{mutex};
    free_list.push_back(handle);
}

void CpuDescriptorAllocator::AddPage() {
    ComPtr<ID3D12DescriptorHeap> page = CreateHeap(device, type, page_size, false);
    const D3D12_CPU_DESCRIPTOR_HANDLE base = page->GetCPUDescriptorHandleForHeapStart();
    // Pushed in reverse so slots are handed out in ascending order.
    for (u32 i = page_size; i-- > 0;) {
        free_list.push_back({base.ptr + static_cast<SIZE_T>(i) * stride});
    }
    pages.push_back(std::move(page));
}

// --- DescriptorRing ---------------------------------------------------------------------------

DescriptorRing::DescriptorRing(ID3D12Device* device_, Scheduler& scheduler_, u32 capacity_)
    : device{device_}, scheduler{scheduler_},
      heap{CreateHeap(device_, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, capacity_, true)},
      cpu_base{heap->GetCPUDescriptorHandleForHeapStart()},
      gpu_base{heap->GetGPUDescriptorHandleForHeapStart()}, capacity{capacity_},
      stride{device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)} {}

DescriptorRange DescriptorRing::Allocate(u32 count) {
    if (count == 0 || count > capacity) {
        throw std::runtime_error(fmt::format("D3D12: bad descriptor ring request ({})", count));
    }
    u32 offset{};
    while (!TryAllocate(count, offset)) {
        // Full: wait for the oldest range (flushing it first if it is still being recorded).
        LOG_DEBUG(Render, "D3D12: descriptor ring full, waiting for tick {}",
                  in_flight.front().first);
        scheduler.Wait(in_flight.front().first);
    }
    const u64 tick = scheduler.CurrentTick();
    if (!in_flight.empty() && in_flight.back().first == tick) {
        in_flight.back().second = head;
    } else {
        in_flight.emplace_back(tick, head);
    }
    return {
        .cpu = {cpu_base.ptr + static_cast<SIZE_T>(offset) * stride},
        .gpu = {gpu_base.ptr + static_cast<u64>(offset) * stride},
    };
}

D3D12_GPU_DESCRIPTOR_HANDLE DescriptorRing::Upload(
    std::span<const D3D12_CPU_DESCRIPTOR_HANDLE> descriptors) {
    const DescriptorRange range = Allocate(static_cast<u32>(descriptors.size()));
    D3D12_CPU_DESCRIPTOR_HANDLE dst = range.cpu;
    for (const D3D12_CPU_DESCRIPTOR_HANDLE src : descriptors) {
        device->CopyDescriptorsSimple(1, dst, src, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        dst.ptr += stride;
    }
    return range.gpu;
}

bool DescriptorRing::TryAllocate(u32 count, u32& offset) {
    Retire();
    if (in_flight.empty()) {
        head = 0;
        tail = 0;
    }
    const bool empty = in_flight.empty();
    if (empty || head > tail) {
        // Free space is [head, capacity) and then [0, tail).
        if (capacity - head >= count) {
            offset = head;
        } else if (empty || count < tail) {
            offset = 0; // wrap; the skipped end is reclaimed with the range before it
        } else {
            return false;
        }
    } else if (head < tail && tail - head > count) {
        offset = head;
    } else {
        return false; // head == tail with ranges in flight: completely full
    }
    head = offset + count;
    return true;
}

void DescriptorRing::Retire() {
    while (!in_flight.empty() && scheduler.IsFree(in_flight.front().first)) {
        tail = in_flight.front().second;
        in_flight.pop_front();
    }
}

// --- SamplerHeap ------------------------------------------------------------------------------

size_t SamplerHeap::KeyHash::operator()(const std::vector<u64>& key) const noexcept {
    return static_cast<size_t>(Common::CityHash64(reinterpret_cast<const char*>(key.data()),
                                                  key.size() * sizeof(u64)));
}

SamplerHeap::SamplerHeap(ID3D12Device* device_, Scheduler& scheduler_)
    : device{device_}, scheduler{scheduler_},
      heap{CreateHeap(device_, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, CAPACITY, true)},
      cpu_base{heap->GetCPUDescriptorHandleForHeapStart()},
      gpu_base{heap->GetGPUDescriptorHandleForHeapStart()},
      stride{device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER)} {}

D3D12_GPU_DESCRIPTOR_HANDLE SamplerHeap::GetTable(
    std::span<const u64> keys, std::span<const D3D12_CPU_DESCRIPTOR_HANDLE> samplers) {
    const u32 count = static_cast<u32>(samplers.size());
    if (count == 0 || count > CAPACITY || keys.size() != samplers.size()) {
        throw std::runtime_error(fmt::format("D3D12: bad sampler table request ({})", count));
    }
    std::vector<u64> key(keys.begin(), keys.end());
    if (const auto it = tables.find(key); it != tables.end()) {
        return {gpu_base.ptr + static_cast<u64>(it->second) * stride};
    }
    if (used + count > CAPACITY) {
        // Tables already recorded may still be read by the GPU: only reuse the heap once idle.
        LOG_INFO(Render, "D3D12: sampler heap full ({} tables), resetting", tables.size());
        scheduler.Finish();
        tables.clear();
        used = 0;
    }
    const u32 first = used;
    D3D12_CPU_DESCRIPTOR_HANDLE dst{cpu_base.ptr + static_cast<SIZE_T>(first) * stride};
    for (const D3D12_CPU_DESCRIPTOR_HANDLE src : samplers) {
        device->CopyDescriptorsSimple(1, dst, src, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
        dst.ptr += stride;
    }
    used += count;
    tables.emplace(std::move(key), first);
    return {gpu_base.ptr + static_cast<u64>(first) * stride};
}

} // namespace D3D12
