// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/alignment.h"
#include "common/assert.h"
#include "common/literals.h"
#include "common/logging.h"
#include "video_core/renderer_d3d12/d3d12_resource_allocator.h"
#include "video_core/renderer_d3d12/d3d12_heap_packing.h"
#include "video_core/renderer_d3d12/d3d12_scheduler.h"

#include <algorithm>
#include <array>
#include <bit>
#include <map>
#include <mutex>
#include <vector>

namespace D3D12 {

using namespace Common::Literals;

namespace {
constexpr u64 DEFAULT_BLOCK_SIZE = 64_MiB;

enum class HeapClass : u8 { Texture, RenderTarget };

HeapClass ClassOf(const D3D12_RESOURCE_DESC& desc) {
    constexpr D3D12_RESOURCE_FLAGS RT_DS = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                                           D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    return (desc.Flags & RT_DS) != 0 ? HeapClass::RenderTarget : HeapClass::Texture;
}

D3D12_HEAP_FLAGS FlagsOf(HeapClass heap_class) {
    return heap_class == HeapClass::RenderTarget ? D3D12_HEAP_FLAG_ALLOW_ONLY_RT_DS_TEXTURES
                                                 : D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES;
}
} // Anonymous namespace

struct TextureResourceAllocator::State {
    struct Block {
        ComPtr<ID3D12Heap> heap;
        HeapClass heap_class{};
        u64 size{};
        std::map<u64, u64> free_ranges; // offset -> bytes
    };

    mutable std::mutex mutex;
    std::vector<Block> blocks;
    u64 heap_bytes{};
    u64 live_bytes{};
    u64 peak_bytes{};
    u64 placed_resources{};
    u64 committed_fallbacks{};
    u64 trimmed_heaps{};
    u64 trimmed_bytes{};
    u64 pending_bytes{};

    void Free(size_t block_index, u64 offset, u64 size, bool pending = false) {
        std::scoped_lock lock{mutex};
        if (block_index >= blocks.size()) {
            return;
        }
        const u64 released_size = size;
        auto& ranges = blocks[block_index].free_ranges;
        auto next = ranges.lower_bound(offset);
        if (next != ranges.begin()) {
            auto prev = std::prev(next);
            if (prev->first + prev->second == offset) {
                offset = prev->first;
                size += prev->second;
                ranges.erase(prev);
            }
        }
        next = ranges.lower_bound(offset);
        if (next != ranges.end() && offset + size == next->first) {
            size += next->second;
            ranges.erase(next);
        }
        ranges.emplace(offset, size);
        ASSERT(live_bytes >= released_size);
        live_bytes -= released_size;
        if (pending) pending_bytes -= released_size;
    }
};

struct TextureResourceAllocator::Allocation {
    std::shared_ptr<State> state;
    size_t block_index{};
    u64 offset{};
    u64 size{};
    bool pending{}; // guarded by State::mutex

    ~Allocation() {
        if (state) {
            state->Free(block_index, offset, size, pending);
        }
    }
};

TextureResourceAllocator::TextureResourceAllocator(const Device& device_, Scheduler& scheduler_)
    : device{device_}, scheduler{scheduler_}, state{std::make_shared<State>()} {}

TextureResourceAllocator::~TextureResourceAllocator() = default;

TextureResourceAllocator::Resource TextureResourceAllocator::Create(
    const D3D12_RESOURCE_DESC& desc, D3D12_RESOURCE_STATES initial_state,
    const D3D12_CLEAR_VALUE* clear_value) {
    const D3D12_RESOURCE_ALLOCATION_INFO info = device.Get()->GetResourceAllocationInfo(0, 1, &desc);
    if (info.SizeInBytes == UINT64_MAX || info.Alignment == 0) {
        return {};
    }
    const HeapClass heap_class = ClassOf(desc);
    size_t selected = SIZE_MAX;
    u64 selected_offset{};
    u64 best_waste = UINT64_MAX;

    {
        std::scoped_lock lock{state->mutex};
        for (size_t index = 0; index < state->blocks.size() && best_waste != 0; ++index) {
            auto& block = state->blocks[index];
            if (!block.heap || block.heap_class != heap_class) {
                continue;
            }
            for (const auto& [offset, bytes] : block.free_ranges) {
                const auto fit = HeapPacking::Fit(offset, bytes, info.SizeInBytes, info.Alignment);
                if (fit && fit->waste < best_waste) {
                    selected = index;
                    selected_offset = fit->offset;
                    best_waste = fit->waste;
                    if (!best_waste) break;
                }
            }
        }

        if (selected == SIZE_MAX) {
            const u64 wanted = HeapPacking::BlockSize(info.SizeInBytes, info.Alignment,
                                                      DEFAULT_BLOCK_SIZE);
            if (!wanted) return {};
            const D3D12_HEAP_DESC heap_desc{
                .SizeInBytes = wanted,
                .Properties = {.Type = D3D12_HEAP_TYPE_DEFAULT},
                .Alignment = 0,
                .Flags = FlagsOf(heap_class),
            };
            State::Block block{.heap_class = heap_class, .size = wanted};
            const HRESULT heap_hr = device.Get()->CreateHeap(&heap_desc, IID_PPV_ARGS(&block.heap));
            if (SUCCEEDED(heap_hr)) {
                block.free_ranges.emplace(0, wanted);
                state->heap_bytes += wanted;
                // Tombstones preserve outstanding Allocation indices. Reuse only slots with
                // no heap: Trim proves there are no live or pending allocations in that slot.
                const auto vacant = std::find_if(state->blocks.begin(), state->blocks.end(),
                                                [](const auto& b) { return !b.heap; });
                if (vacant == state->blocks.end()) {
                    state->blocks.emplace_back(std::move(block));
                    selected = state->blocks.size() - 1;
                } else {
                    selected = static_cast<size_t>(vacant - state->blocks.begin());
                    *vacant = std::move(block);
                }
                selected_offset = 0;
                LOG_INFO(Render, "D3D12: texture heap pool added {} MiB {} block ({} MiB total)",
                         wanted / 1_MiB,
                         heap_class == HeapClass::RenderTarget ? "RT/DS" : "texture",
                         state->heap_bytes / 1_MiB);
            } else {
                LOG_WARNING(Render, "D3D12: CreateHeap for texture pool failed 0x{:08x}; using committed resources",
                            static_cast<u32>(heap_hr));
            }
        }

        if (selected != SIZE_MAX) {
            auto& ranges = state->blocks[selected].free_ranges;
            auto containing = ranges.upper_bound(selected_offset);
            if (containing != ranges.begin()) {
                --containing;
            }
            const u64 range_offset = containing->first;
            const u64 range_size = containing->second;
            ranges.erase(containing);
            if (selected_offset > range_offset) {
                ranges.emplace(range_offset, selected_offset - range_offset);
            }
            const u64 end = selected_offset + info.SizeInBytes;
            if (end < range_offset + range_size) {
                ranges.emplace(end, range_offset + range_size - end);
            }
            state->live_bytes += info.SizeInBytes;
            state->peak_bytes = std::max(state->peak_bytes, state->live_bytes);
        }
    }

    Resource result;
    if (selected != SIZE_MAX) {
        const HRESULT placed_hr = device.Get()->CreatePlacedResource(
            state->blocks[selected].heap.Get(), selected_offset, &desc, initial_state, clear_value,
            IID_PPV_ARGS(&result.resource));
        if (SUCCEEDED(placed_hr)) {
            result.allocation = std::make_shared<Allocation>();
            result.allocation->state = state;
            result.allocation->block_index = selected;
            result.allocation->offset = selected_offset;
            result.allocation->size = info.SizeInBytes;
            result.placed = true;
            std::scoped_lock lock{state->mutex};
            ++state->placed_resources;
            return result;
        }
        state->Free(selected, selected_offset, info.SizeInBytes);
        LOG_WARNING(Render, "D3D12: CreatePlacedResource failed 0x{:08x}; using committed resource",
                    static_cast<u32>(placed_hr));
    }

    const D3D12_HEAP_PROPERTIES heap{.Type = D3D12_HEAP_TYPE_DEFAULT};
    ThrowIfFailed(device.Get()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                         initial_state, clear_value,
                                                         IID_PPV_ARGS(&result.resource)),
                  "Create committed texture fallback");
    {
        std::scoped_lock lock{state->mutex};
        ++state->committed_fallbacks;
    }
    return result;
}

void TextureResourceAllocator::DeferRelease(Resource&& resource) {
    if (!resource.resource) {
        return;
    }
    auto allocation = std::move(resource.allocation);
    if (allocation) {
        std::scoped_lock lock{allocation->state->mutex};
        if (!allocation->pending) {
            allocation->pending = true;
            allocation->state->pending_bytes += allocation->size;
        }
    }
    scheduler.DeferRelease(std::move(resource.resource),
                           [allocation = std::move(allocation)]() mutable { allocation.reset(); });
}

void TextureResourceAllocator::TrimEmptyHeaps(bool under_pressure) {
    std::scoped_lock lock{state->mutex};
    std::array<bool, 2> warm{};
    for (auto& block : state->blocks) {
        if (!block.heap || block.free_ranges.size() != 1) continue;
        const auto& [offset, size] = *block.free_ranges.begin();
        if (offset != 0 || size != block.size) continue;
        const size_t type = static_cast<size_t>(block.heap_class);
        if (!under_pressure && !warm[type]) {
            warm[type] = true;
            continue;
        }
        state->heap_bytes -= block.size;
        state->trimmed_bytes += block.size;
        ++state->trimmed_heaps;
        block.heap.Reset();
        block.free_ranges.clear();
        block.size = 0;
    }
}

std::string TextureResourceAllocator::Report() const {
    std::scoped_lock lock{state->mutex};
    return fmt::format("texture heaps {} MiB, live {} MiB, peak {} MiB, placed {}, fallback {}, trimmed {} ({} MiB)",
                       state->heap_bytes / 1_MiB, state->live_bytes / 1_MiB,
                       state->peak_bytes / 1_MiB, state->placed_resources,
                       state->committed_fallbacks, state->trimmed_heaps, state->trimmed_bytes / 1_MiB);
}

TextureResourceAllocator::Stats TextureResourceAllocator::GetStats() const {
    std::scoped_lock lock{state->mutex};
    Stats result{.heap_bytes = state->heap_bytes, .reserved_bytes = state->live_bytes,
                 .pending_bytes = state->pending_bytes};
    for (const auto& block : state->blocks) {
        for (const auto& [offset, bytes] : block.free_ranges) {
            result.free_bytes += bytes;
            result.largest_free_range = std::max(result.largest_free_range, bytes);
        }
    }
    return result;
}

} // namespace D3D12
