// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <deque>
#include <mutex>
#include <span>
#include <unordered_map>
#include <vector>

#include "video_core/renderer_d3d12/d3d12_device.h"

namespace D3D12 {

class Scheduler;

/// Persistent descriptors in non-shader-visible ("offline") heaps: the SRVs/UAVs of image views,
/// RTVs/DSVs of attachments and samplers. Grows in pages; freed slots are reused.
///
/// A slot may be freed as soon as its last use has been recorded: CopyDescriptors and
/// OMSetRenderTargets read descriptors on the CPU timeline, when they are called.
class CpuDescriptorAllocator {
public:
    explicit CpuDescriptorAllocator(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type,
                                    u32 page_size = 1024);

    CpuDescriptorAllocator(const CpuDescriptorAllocator&) = delete;
    CpuDescriptorAllocator& operator=(const CpuDescriptorAllocator&) = delete;

    [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE Allocate();
    void Free(D3D12_CPU_DESCRIPTOR_HANDLE handle);

private:
    void AddPage();

    ID3D12Device* device;
    D3D12_DESCRIPTOR_HEAP_TYPE type;
    u32 page_size;
    u32 stride;
    std::vector<ComPtr<ID3D12DescriptorHeap>> pages;
    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> free_list;
    std::mutex mutex;
};

struct DescriptorRange {
    D3D12_CPU_DESCRIPTOR_HANDLE cpu;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu;
};

/// The one shader-visible CBV/SRV/UAV heap, used as a ring: each draw or dispatch takes a
/// contiguous range for its descriptor tables and copies the offline descriptors into it.
/// Ranges are retired by scheduler tick; a full ring flushes and waits for the oldest range.
class DescriptorRing {
public:
    explicit DescriptorRing(ID3D12Device* device, Scheduler& scheduler, u32 capacity);

    DescriptorRing(const DescriptorRing&) = delete;
    DescriptorRing& operator=(const DescriptorRing&) = delete;

    [[nodiscard]] DescriptorRange Allocate(u32 count);

    /// Allocates count slots and copies the given offline descriptors into them.
    [[nodiscard]] D3D12_GPU_DESCRIPTOR_HANDLE Upload(
        std::span<const D3D12_CPU_DESCRIPTOR_HANDLE> descriptors);

    [[nodiscard]] ID3D12DescriptorHeap* Heap() const {
        return heap.Get();
    }
    [[nodiscard]] u32 Stride() const {
        return stride;
    }

private:
    bool TryAllocate(u32 count, u32& offset);
    void Retire();

    ID3D12Device* device;
    Scheduler& scheduler;
    ComPtr<ID3D12DescriptorHeap> heap;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_base;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_base;
    u32 capacity;
    u32 stride;
    u32 head{}; ///< next free slot
    u32 tail{}; ///< oldest slot the GPU may still read
    std::deque<std::pair<u64, u32>> in_flight; ///< (tick, end offset), oldest first
    bool logged_upload{};
};

/// The CBV/SRV/UAV table of one draw or dispatch, written in root-signature order straight into a
/// range of the ring (see d3d12_root_signature.h for the slot order). Buffer views are created in
/// place; image views are copied from their offline descriptors.
///
/// Acquire() must come before any command of the draw is recorded: allocating from a full ring
/// flushes the command list.
class GuestDescriptorQueue {
public:
    explicit GuestDescriptorQueue(ID3D12Device* device, DescriptorRing& ring);

    GuestDescriptorQueue(const GuestDescriptorQueue&) = delete;
    GuestDescriptorQueue& operator=(const GuestDescriptorQueue&) = delete;

    /// Starts a table of count descriptors (none when count is zero).
    void Acquire(u32 count);

    /// GPU handle of the table; checks (and warns once) that it was filled exactly.
    [[nodiscard]] D3D12_GPU_DESCRIPTOR_HANDLE Table();

    /// A CBV; address 0 writes a null one. size is rounded up to 256 bytes.
    void AddConstantBuffer(D3D12_GPU_VIRTUAL_ADDRESS address, u32 size);
    /// A raw UAV, then a raw SRV of the same range (null ones when resource is null).
    void AddStorageBuffer(ID3D12Resource* resource, u64 offset, u32 size);
    /// A typed SRV (texture buffer), preceded by a typed UAV for image buffers. Null views when
    /// resource is null or the offset is not a whole number of elements.
    void AddTexelBuffer(ID3D12Resource* resource, u64 offset, u32 size, DXGI_FORMAT format,
                        u32 element_size, bool with_uav);
    /// Copies of offline descriptors (texture SRVs, image UAVs).
    void AddCopy(D3D12_CPU_DESCRIPTOR_HANDLE descriptor);

private:
    D3D12_CPU_DESCRIPTOR_HANDLE Next();

    ID3D12Device* device;
    DescriptorRing& ring;
    u32 stride;
    DescriptorRange range{};
    u32 count{};
    u32 written{};
    bool logged_mismatch{};
    bool logged_unaligned{};
};

/// The one shader-visible sampler heap. D3D12 caps it at 2048 entries on every tier, too few for
/// a per-draw ring, so whole sampler tables are deduplicated by content: games reuse the same
/// combinations constantly. When the heap fills up it is reset after the GPU goes idle.
class SamplerHeap {
public:
    static constexpr u32 CAPACITY = D3D12_MAX_SHADER_VISIBLE_SAMPLER_HEAP_SIZE;

    explicit SamplerHeap(ID3D12Device* device, Scheduler& scheduler);

    SamplerHeap(const SamplerHeap&) = delete;
    SamplerHeap& operator=(const SamplerHeap&) = delete;

    /// Returns a table holding the given offline samplers, in order. keys must identify each
    /// sampler uniquely for the lifetime of the renderer (they are never reused).
    [[nodiscard]] D3D12_GPU_DESCRIPTOR_HANDLE GetTable(
        std::span<const u64> keys, std::span<const D3D12_CPU_DESCRIPTOR_HANDLE> samplers);

    [[nodiscard]] ID3D12DescriptorHeap* Heap() const {
        return heap.Get();
    }

private:
    struct KeyHash {
        size_t operator()(const std::vector<u64>& key) const noexcept;
    };

    ID3D12Device* device;
    Scheduler& scheduler;
    ComPtr<ID3D12DescriptorHeap> heap;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_base;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_base;
    u32 stride;
    u32 used{};
    std::unordered_map<std::vector<u64>, u32, KeyHash> tables; ///< keys -> first slot
    bool logged_cache{};
    bool logged_reuse{};
};

} // namespace D3D12
