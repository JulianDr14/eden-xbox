// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <limits>
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
    if (count == 0) {
        throw std::invalid_argument("D3D12: descriptor heap size must not be zero");
    }
    if (shader_visible && type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV &&
        type != D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER) {
        throw std::invalid_argument("D3D12: RTV/DSV heaps cannot be shader visible");
    }
    if (shader_visible && type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER &&
        count > D3D12_MAX_SHADER_VISIBLE_SAMPLER_HEAP_SIZE) {
        throw std::invalid_argument("D3D12: shader-visible sampler heap exceeds 2048 slots");
    }
    if (shader_visible && type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV &&
        count > D3D12_MAX_SHADER_VISIBLE_DESCRIPTOR_HEAP_SIZE_TIER_1) {
        throw std::invalid_argument(
            "D3D12: shader-visible CBV/SRV/UAV heap exceeds one million slots");
    }
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
      stride{device_->GetDescriptorHandleIncrementSize(type_)} {
    if (page_size == 0) {
        throw std::invalid_argument("D3D12: offline descriptor page size must not be zero");
    }
}

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
    LOG_INFO(Render, "D3D12: offline descriptor page created (type {}, {} slots)",
             static_cast<u32>(type), page_size);
}

// --- DescriptorRing ---------------------------------------------------------------------------

DescriptorRing::DescriptorRing(ID3D12Device* device_, Scheduler& scheduler_, u32 capacity_)
    : device{device_}, scheduler{scheduler_},
      heap{CreateHeap(device_, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, capacity_, true)},
      cpu_base{heap->GetCPUDescriptorHandleForHeapStart()},
      gpu_base{heap->GetGPUDescriptorHandleForHeapStart()}, capacity{capacity_},
      stride{device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)} {
    LOG_INFO(Render, "D3D12: shader-visible descriptor ring ready ({} slots)", capacity);
}

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
    if (descriptors.size() > std::numeric_limits<u32>::max()) {
        throw std::length_error("D3D12: descriptor upload is too large");
    }
    const DescriptorRange range = Allocate(static_cast<u32>(descriptors.size()));
    D3D12_CPU_DESCRIPTOR_HANDLE dst = range.cpu;
    for (const D3D12_CPU_DESCRIPTOR_HANDLE src : descriptors) {
        device->CopyDescriptorsSimple(1, dst, src, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        dst.ptr += stride;
    }
    CheckRemovedAfterDescriptor(device, [&] {
        return fmt::format("copying {} view descriptors to the shader-visible heap",
                           descriptors.size());
    });
    if (!logged_upload) {
        LOG_INFO(Render, "D3D12: descriptor ring upload active ({} descriptors, tick {})",
                 descriptors.size(), scheduler.CurrentTick());
        logged_upload = true;
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

// --- GuestDescriptorQueue ---------------------------------------------------------------------

GuestDescriptorQueue::GuestDescriptorQueue(ID3D12Device* device_, DescriptorRing& ring_)
    : device{device_}, ring{ring_}, stride{ring_.Stride()} {}

void GuestDescriptorQueue::Acquire(u32 count_) {
    count = count_;
    written = 0;
    // One spare slot past the table absorbs writes beyond what the root signature declares.
    range = count != 0 ? ring.Allocate(count + 1) : DescriptorRange{};
}

D3D12_GPU_DESCRIPTOR_HANDLE GuestDescriptorQueue::Table() {
    if (written != count && !logged_mismatch) {
        LOG_ERROR(Render, "D3D12: descriptor table filled with {} of {} descriptors", written,
                  count);
        logged_mismatch = true;
    }
    return range.gpu;
}

D3D12_CPU_DESCRIPTOR_HANDLE GuestDescriptorQueue::Next() {
    if (written >= count) {
        // More bindings than the root signature declares: the spare slot. Reported by Table().
        ++written;
        return {range.cpu.ptr + static_cast<SIZE_T>(count) * stride};
    }
    return {range.cpu.ptr + static_cast<SIZE_T>(written++) * stride};
}

void GuestDescriptorQueue::AddConstantBuffer(D3D12_GPU_VIRTUAL_ADDRESS address, u32 size) {
    if (count == 0) {
        return;
    }
    const D3D12_CONSTANT_BUFFER_VIEW_DESC desc{
        .BufferLocation = address,
        .SizeInBytes = address != 0 ? (size + 255u) & ~255u : 0u,
    };
    device->CreateConstantBufferView(&desc, Next());
    CheckRemovedAfterDescriptor(device, [&] {
        return fmt::format("CBV at 0x{:x} of {} bytes", desc.BufferLocation, desc.SizeInBytes);
    });
}

void GuestDescriptorQueue::AddStorageBuffer(ID3D12Resource* resource, u64 offset, u32 size) {
    if (count == 0) {
        return;
    }
    // Raw views address 32-bit words from a 16-byte aligned offset.
    if (resource && offset % D3D12_RAW_UAV_SRV_BYTE_ALIGNMENT != 0) {
        if (!logged_unaligned) {
            LOG_WARNING(Render, "D3D12: storage buffer at unaligned offset {} bound as null",
                        offset);
            logged_unaligned = true;
        }
        resource = nullptr;
    }
    const u32 words = (size + 3) / 4;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{
        .Format = DXGI_FORMAT_R32_TYPELESS,
        .ViewDimension = D3D12_UAV_DIMENSION_BUFFER,
        .Buffer = {.FirstElement = resource ? offset / 4 : 0,
                   .NumElements = resource ? words : 0,
                   .StructureByteStride = 0,
                   .CounterOffsetInBytes = 0,
                   .Flags = D3D12_BUFFER_UAV_FLAG_RAW},
    };
    device->CreateUnorderedAccessView(resource, nullptr, &uav, Next());
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{
        .Format = DXGI_FORMAT_R32_TYPELESS,
        .ViewDimension = D3D12_SRV_DIMENSION_BUFFER,
        .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
        .Buffer = {.FirstElement = resource ? offset / 4 : 0,
                   .NumElements = resource ? words : 0,
                   .StructureByteStride = 0,
                   .Flags = D3D12_BUFFER_SRV_FLAG_RAW},
    };
    device->CreateShaderResourceView(resource, &srv, Next());
    CheckRemovedAfterDescriptor(device, [&] {
        return fmt::format("storage buffer views (offset {} size {} null {})", offset, size,
                           resource == nullptr);
    });
}

void GuestDescriptorQueue::AddTexelBuffer(ID3D12Resource* resource, u64 offset, u32 size,
                                          DXGI_FORMAT format, u32 element_size, bool with_uav) {
    if (count == 0) {
        return;
    }
    if (format == DXGI_FORMAT_UNKNOWN || element_size == 0) {
        format = DXGI_FORMAT_R32_UINT;
        element_size = 4;
        resource = nullptr;
    }
    if (resource && offset % element_size != 0) {
        if (!logged_unaligned) {
            LOG_WARNING(Render, "D3D12: texel buffer at unaligned offset {} bound as null", offset);
            logged_unaligned = true;
        }
        resource = nullptr;
    }
    const u64 first = resource ? offset / element_size : 0;
    const u32 elements = resource ? size / element_size : 0;
    if (with_uav) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{
            .Format = format,
            .ViewDimension = D3D12_UAV_DIMENSION_BUFFER,
            .Buffer = {.FirstElement = first, .NumElements = elements},
        };
        device->CreateUnorderedAccessView(resource, nullptr, &uav, Next());
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{
        .Format = format,
        .ViewDimension = D3D12_SRV_DIMENSION_BUFFER,
        .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
        .Buffer = {.FirstElement = first, .NumElements = elements},
    };
    device->CreateShaderResourceView(resource, &srv, Next());
    CheckRemovedAfterDescriptor(device, [&] {
        return fmt::format("texel buffer views (format {} offset {} elements {} uav {} null {})",
                           static_cast<u32>(format), offset, elements, with_uav,
                           resource == nullptr);
    });
}

void GuestDescriptorQueue::AddCopy(D3D12_CPU_DESCRIPTOR_HANDLE descriptor) {
    if (count == 0) {
        return;
    }
    device->CopyDescriptorsSimple(1, Next(), descriptor, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    CheckRemovedAfterDescriptor(device, [&] {
        return fmt::format("copying a texture descriptor (0x{:x}) for a draw", descriptor.ptr);
    });
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
      stride{device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER)} {
    LOG_INFO(Render, "D3D12: shader-visible sampler heap ready ({} slots)", CAPACITY);
}

D3D12_GPU_DESCRIPTOR_HANDLE SamplerHeap::GetTable(
    std::span<const u64> keys, std::span<const D3D12_CPU_DESCRIPTOR_HANDLE> samplers) {
    if (samplers.size() > std::numeric_limits<u32>::max()) {
        throw std::length_error("D3D12: sampler table is too large");
    }
    const u32 count = static_cast<u32>(samplers.size());
    if (count == 0 || count > CAPACITY || keys.size() != samplers.size()) {
        throw std::runtime_error(fmt::format("D3D12: bad sampler table request ({})", count));
    }
    std::vector<u64> key(keys.begin(), keys.end());
    if (const auto it = tables.find(key); it != tables.end()) {
        if (!logged_reuse) {
            LOG_INFO(Render, "D3D12: sampler table deduplication active ({} samplers)", count);
            logged_reuse = true;
        }
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
    CheckRemovedAfter(device, [&] {
        return fmt::format("copying {} sampler descriptors", samplers.size());
    });
    used += count;
    tables.emplace(std::move(key), first);
    if (!logged_cache) {
        LOG_INFO(Render, "D3D12: sampler table cached ({} samplers, {} of {} slots used)", count,
                 used, CAPACITY);
        logged_cache = true;
    }
    return {gpu_base.ptr + static_cast<u64>(first) * stride};
}

} // namespace D3D12
