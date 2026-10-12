// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2021 yuzu Emulator Project
// SPDX-FileCopyrightText: 2021 Skyline Team and Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstring>
#include <stdexcept>
#include <utility>

#include "common/alignment.h"
#include "common/assert.h"
#include "common/common_types.h"
#include "common/logging.h"
#include "core/core.h"
#include "core/hle/service/nvdrv/core/container.h"
#include "core/hle/service/nvdrv/core/nvmap.h"
#include "core/hle/service/nvdrv/devices/ioctl_serialization.h"
#include "core/hle/service/nvdrv/devices/nvhost_as_gpu.h"
#include "core/hle/service/nvdrv/devices/nvhost_gpu.h"
#include "core/hle/service/nvdrv/nvdrv.h"
#include "core/hle/service/pulse_service.h"
#include "video_core/control/channel_state.h"
#include "video_core/gpu.h"
#include "video_core/memory_manager.h"
#include "video_core/rasterizer_interface.h"

namespace Service::Nvidia::Devices {

nvhost_as_gpu::nvhost_as_gpu(Core::System& system_, Module& module_, NvCore::Container& core)
    : nvdevice{system_}, module{module_}, container{core}, nvmap{core.GetNvMapFile()}, vm{},
      gmmu{} {}

nvhost_as_gpu::~nvhost_as_gpu() = default;

NvResult nvhost_as_gpu::Ioctl1(DeviceFD fd, Ioctl command, std::span<const u8> input,
                               std::span<u8> output) {
    switch (command.group) {
    case 'A':
        switch (command.cmd) {
        case 0x1:
            return WrapFixed(this, &nvhost_as_gpu::BindChannel, input, output, fd);
        case 0x2:
            return WrapFixed(this, &nvhost_as_gpu::AllocateSpace, input, output);
        case 0x3:
            return WrapFixed(this, &nvhost_as_gpu::FreeSpace, input, output);
        case 0x5:
            return WrapFixed(this, &nvhost_as_gpu::UnmapBuffer, input, output);
        case 0x6:
            return WrapFixed(this, &nvhost_as_gpu::MapBufferEx, input, output);
        case 0x8:
            return WrapFixed(this, &nvhost_as_gpu::GetVARegions1, input, output);
        case 0x9:
            return WrapFixed(this, &nvhost_as_gpu::AllocAsEx, input, output);
        case 0x14:
            return WrapVariable(this, &nvhost_as_gpu::Remap, input, output);
        default:
            break;
        }
        break;
    default:
        break;
    }

    UNIMPLEMENTED_MSG("Unimplemented ioctl={:08X}", command.raw);
    return NvResult::NotImplemented;
}

NvResult nvhost_as_gpu::Ioctl2(DeviceFD fd, Ioctl command, std::span<const u8> input,
                               std::span<const u8> inline_input, std::span<u8> output) {
    UNIMPLEMENTED_MSG("Unimplemented ioctl={:08X}", command.raw);
    return NvResult::NotImplemented;
}

NvResult nvhost_as_gpu::Ioctl3(DeviceFD fd, Ioctl command, std::span<const u8> input,
                               std::span<u8> output, std::span<u8> inline_output) {
    switch (command.group) {
    case 'A':
        switch (command.cmd) {
        case 0x8:
            return WrapFixedInlOut(this, &nvhost_as_gpu::GetVARegions3, input, output,
                                   inline_output);
        default:
            break;
        }
        break;
    default:
        break;
    }
    UNIMPLEMENTED_MSG("Unimplemented ioctl={:08X}", command.raw);
    return NvResult::NotImplemented;
}

void nvhost_as_gpu::OnOpen(NvCore::SessionId session_id, DeviceFD fd) {}
void nvhost_as_gpu::OnClose(DeviceFD fd) {}

NvResult nvhost_as_gpu::AllocAsEx(IoctlAllocAsEx& params) {
    LOG_DEBUG(Service_NVDRV, "called, big_page_size={:#x}", params.big_page_size);

    std::scoped_lock lock(mutex);

    if (vm.initialised) {
        ASSERT_MSG(false, "Cannot initialise an address space twice!");
        return NvResult::InvalidState;
    }

    if (params.big_page_size) {
        if (!std::has_single_bit(params.big_page_size)) {
            LOG_ERROR(Service_NVDRV, "Non power-of-2 big page size: {:#x}!", params.big_page_size);
            return NvResult::BadValue;
        }

        if ((params.big_page_size & VM::SUPPORTED_BIG_PAGE_SIZES) == 0) {
            LOG_ERROR(Service_NVDRV, "Unsupported big page size: {:#x}!", params.big_page_size);
            return NvResult::BadValue;
        }

        vm.big_page_size = params.big_page_size;
        vm.big_page_size_bits = static_cast<u32>(std::countr_zero(params.big_page_size));

        vm.va_range_start = params.big_page_size << VM::VA_START_SHIFT;
    }

    // If this is unspecified then default values should be used
    if (params.va_range_start) {
        vm.va_range_start = params.va_range_start;
        vm.va_range_split = params.va_range_split;
        vm.va_range_end = params.va_range_end;
    }

    const u64 max_big_page_bits = Common::Log2Ceil(vm.va_range_end);

    const auto start_pages{static_cast<u32>(vm.va_range_start >> VM::PAGE_SIZE_BITS)};
    const auto end_pages{static_cast<u32>(vm.va_range_split >> VM::PAGE_SIZE_BITS)};
    vm.small_page_allocator.emplace(start_pages, end_pages);

    const auto start_big_pages{static_cast<u32>(vm.va_range_split >> vm.big_page_size_bits)};
    const auto end_big_pages{
        static_cast<u32>((vm.va_range_end - vm.va_range_split) >> vm.big_page_size_bits)};
    vm.big_page_allocator.emplace(start_big_pages, end_big_pages);

    gmmu = std::make_unique<Tegra::MemoryManager>(system, max_big_page_bits, vm.va_range_split, vm.big_page_size_bits, VM::PAGE_SIZE_BITS);
    system.GPU().InitAddressSpace(*gmmu);
    vm.initialised = true;

    return NvResult::Success;
}

NvResult nvhost_as_gpu::AllocateSpace(IoctlAllocSpace& params) {
    LOG_DEBUG(Service_NVDRV, "called, pages={:X}, page_size={:X}, flags={:X}", params.pages,
              params.page_size, params.flags);

    std::scoped_lock lock(mutex);

    if (!vm.initialised) {
        return NvResult::BadValue;
    }

    if (params.page_size != VM::YUZU_PAGESIZE && params.page_size != vm.big_page_size) {
        return NvResult::BadValue;
    }

    if (params.page_size != vm.big_page_size &&
        ((params.flags & MappingFlags::Sparse) != MappingFlags::None)) {
        UNIMPLEMENTED_MSG("Sparse small pages are not implemented!");
        return NvResult::NotImplemented;
    }

    const u32 page_size_bits{params.page_size == VM::YUZU_PAGESIZE ? VM::PAGE_SIZE_BITS
                                                                   : vm.big_page_size_bits};

    auto& allocator{params.page_size == VM::YUZU_PAGESIZE ? *vm.small_page_allocator
                                                          : *vm.big_page_allocator};

    if ((params.flags & MappingFlags::Fixed) != MappingFlags::None) {
        allocator.AllocateFixed(static_cast<u32>(params.offset >> page_size_bits), params.pages);
    } else {
        params.offset = static_cast<u64>(allocator.Allocate(params.pages)) << page_size_bits;
        if (!params.offset) {
            ASSERT_MSG(false, "Failed to allocate free space in the GPU AS!");
            return NvResult::InsufficientMemory;
        }
    }

    u64 size{static_cast<u64>(params.pages) * params.page_size};

    if ((params.flags & MappingFlags::Sparse) != MappingFlags::None) {
        gmmu->MapSparse(params.offset, size);
    }

    allocation_map[params.offset] = {
        .mappings{},
        .size = size,
        .page_size = params.page_size,
        .sparse = (params.flags & MappingFlags::Sparse) != MappingFlags::None,
        .big_pages = params.page_size != VM::YUZU_PAGESIZE,
    };

    return NvResult::Success;
}

bool nvhost_as_gpu::FreeMappingLocked(u64 offset) noexcept {
    if (auto const it = mapping_map.find(offset); it != mapping_map.end()) {
        auto const mapping = it->second;
        if (!mapping.fixed) {
            auto& allocator{mapping.big_page ? *vm.big_page_allocator : *vm.small_page_allocator};
            u32 page_size_bits{mapping.big_page ? vm.big_page_size_bits : VM::PAGE_SIZE_BITS};
            u32 page_size{mapping.big_page ? vm.big_page_size : VM::YUZU_PAGESIZE};
            u64 aligned_size{Common::AlignUp(mapping.size, page_size)};
            allocator.Free(u32(mapping.offset >> page_size_bits), u32(aligned_size >> page_size_bits));
        }
        nvmap.UnpinHandle(mapping.handle);
        // Sparse mappings shouldn't be fully unmapped, just returned to their sparse state
        // Only FreeSpace can unmap them fully
        if (mapping.sparse_alloc) {
            gmmu->MapSparse(offset, mapping.size, mapping.big_page);
        } else {
            gmmu->Unmap(offset, mapping.size);
        }
        PulseForgetRemaps(offset, mapping.size);
        mapping_map.erase(offset);
        return true;
    }
    return false;
}

NvResult nvhost_as_gpu::FreeSpace(IoctlFreeSpace& params) {
    LOG_DEBUG(Service_NVDRV, "called, offset={:X}, pages={:X}, page_size={:X}", params.offset, params.pages, params.page_size);
    std::scoped_lock lock(mutex);
    if (!vm.initialised) {
        return NvResult::BadValue;
    }
    if (auto const it = allocation_map.find(params.offset); it != allocation_map.end()) {
        auto const allocation = it->second;
        if (allocation.page_size != params.page_size || allocation.size != (u64(params.pages) * params.page_size))
            return NvResult::BadValue;

        for (const auto mapping_offset : allocation.mappings)
            if (!FreeMappingLocked(mapping_offset))
                return NvResult::BadValue;

        // Unset sparse flag if required
        if (allocation.sparse)
            gmmu->Unmap(params.offset, allocation.size);

        auto& allocator{params.page_size == VM::YUZU_PAGESIZE ? *vm.small_page_allocator : *vm.big_page_allocator};
        u32 page_size_bits{params.page_size == VM::YUZU_PAGESIZE ? VM::PAGE_SIZE_BITS : vm.big_page_size_bits};

        allocator.Free(u32(params.offset >> page_size_bits), u32(allocation.size >> page_size_bits));
        PulseForgetRemaps(params.offset, allocation.size);
        allocation_map.erase(params.offset);
        return NvResult::Success;
    }
    return NvResult::BadValue;
}

NvResult nvhost_as_gpu::Remap(std::span<IoctlRemapEntry> entries) {
    LOG_DEBUG(Service_NVDRV, "called, num_entries={:#x}", entries.size());

    std::scoped_lock lock(mutex);

    if (!vm.initialised) {
        return NvResult::BadValue;
    }

    for (const auto& entry : entries) {
        GPUVAddr virtual_address{static_cast<u64>(entry.as_offset_big_pages)
                                 << vm.big_page_size_bits};
        u64 size{static_cast<u64>(entry.big_pages) << vm.big_page_size_bits};

        auto alloc{allocation_map.upper_bound(virtual_address)};

        if (alloc-- == allocation_map.begin() ||
            (virtual_address - alloc->first) + size > alloc->second.size) {
            LOG_WARNING(Service_NVDRV, "Cannot remap into an unallocated region!");
            return NvResult::BadValue;
        }

        if (!alloc->second.sparse) {
            LOG_WARNING(Service_NVDRV, "Cannot remap a non-sparse mapping!");
            return NvResult::BadValue;
        }

        const bool use_big_pages = alloc->second.big_pages;
        PulseForgetRemaps(virtual_address, size);
        if (!entry.handle) {
            gmmu->MapSparse(virtual_address, size, use_big_pages);
            pulse_remaps.emplace(virtual_address, PulseRemap{size, 0, 0, 0, use_big_pages});
        } else {
            auto handle{nvmap.GetHandle(entry.handle)};
            if (!handle) {
                return NvResult::BadValue;
            }

            DAddr base = nvmap.PinHandle(entry.handle, false);
            DAddr device_address{static_cast<DAddr>(
                base + (static_cast<u64>(entry.handle_offset_big_pages) << vm.big_page_size_bits))};

            gmmu->Map(virtual_address, device_address, size,
                      static_cast<Tegra::PTEKind>(entry.kind), use_big_pages);
            pulse_remaps.emplace(virtual_address,
                                 PulseRemap{size, entry.handle,
                                            static_cast<u64>(entry.handle_offset_big_pages)
                                                << vm.big_page_size_bits,
                                            entry.kind, use_big_pages});
        }
    }

    return NvResult::Success;
}

NvResult nvhost_as_gpu::MapBufferEx(IoctlMapBufferEx& params) {
    LOG_DEBUG(Service_NVDRV,
              "called, flags={:X}, nvmap_handle={:X}, buffer_offset={}, mapping_size={}"
              ", offset={:#x}",
              params.flags, params.handle, params.buffer_offset, params.mapping_size,
              params.offset);

    std::scoped_lock lock(mutex);

    if (!vm.initialised) {
        return NvResult::BadValue;
    }

    // Remaps a subregion of an existing mapping to a different PA
    if ((params.flags & MappingFlags::Remap) != MappingFlags::None) {
        if (auto const it = mapping_map.find(params.offset); it != mapping_map.end()) {
            auto const mapping = it->second;
            if (mapping.size < params.mapping_size) {
                LOG_WARNING(Service_NVDRV, "Cannot remap a partially mapped GPU address space region: {:#x}", params.offset);
                return NvResult::BadValue;
            }
            u64 gpu_address = u64(params.offset + params.buffer_offset);
            VAddr device_address{mapping.ptr + params.buffer_offset};
            gmmu->Map(gpu_address, device_address, params.mapping_size, Tegra::PTEKind(params.kind), mapping.big_page);
            PulseForgetRemaps(gpu_address, params.mapping_size);
            pulse_remaps.emplace(gpu_address,
                                 PulseRemap{params.mapping_size, mapping.handle,
                                            mapping.handle_offset + u64(params.buffer_offset),
                                            params.kind, mapping.big_page});
            return NvResult::Success;
        } else {
            LOG_WARNING(Service_NVDRV, "Cannot remap an unmapped GPU address space region: {:#x}", params.offset);
            return NvResult::BadValue;
        }
    }

    auto handle{nvmap.GetHandle(params.handle)};
    if (!handle) {
        return NvResult::BadValue;
    }

    DAddr device_address = DAddr(nvmap.PinHandle(params.handle, false) + params.buffer_offset);
    u64 size{params.mapping_size ? params.mapping_size : handle->orig_size};

    bool big_page{[&]() {
        if (Common::IsAligned(handle->align, vm.big_page_size)) {
            return true;
        } else if (Common::IsAligned(handle->align, VM::YUZU_PAGESIZE)) {
            return false;
        } else {
            ASSERT(false);
            return false;
        }
    }()};

    if ((params.flags & MappingFlags::Fixed) != MappingFlags::None) {
        auto alloc{allocation_map.upper_bound(params.offset)};

        if (alloc-- == allocation_map.begin() ||
            (params.offset - alloc->first) + size > alloc->second.size) {
            ASSERT_MSG(false, "Cannot perform a fixed mapping into an unallocated region!");
            return NvResult::BadValue;
        }

        const bool use_big_pages = alloc->second.big_pages && big_page;
        gmmu->Map(params.offset, device_address, size, static_cast<Tegra::PTEKind>(params.kind), use_big_pages);

        alloc->second.mappings.push_back(params.offset);
        mapping_map.insert_or_assign(params.offset, Mapping(params.handle, device_address, params.offset, size, true, use_big_pages, alloc->second.sparse));
    } else {
        auto& allocator{big_page ? *vm.big_page_allocator : *vm.small_page_allocator};
        u32 page_size{big_page ? vm.big_page_size : VM::YUZU_PAGESIZE};
        u32 page_size_bits{big_page ? vm.big_page_size_bits : VM::PAGE_SIZE_BITS};

        params.offset = u64(allocator.Allocate(u32(Common::AlignUp(size, page_size) >> page_size_bits))) << page_size_bits;
        if (!params.offset) {
            ASSERT_MSG(false, "Failed to allocate free space in the GPU AS!");
            return NvResult::InsufficientMemory;
        }
        gmmu->Map(params.offset, device_address, Common::AlignUp(size, page_size), Tegra::PTEKind(params.kind), big_page);
        mapping_map.insert_or_assign(params.offset, Mapping(params.handle, device_address, params.offset, size, false, big_page, false));
    }

    map_buffer_offsets.insert(params.offset);
    auto& mapping = mapping_map.at(params.offset);
    mapping.handle_offset = u64(params.buffer_offset);
    mapping.kind = params.kind;
    PulseForgetRemaps(params.offset, size);

    return NvResult::Success;
}

NvResult nvhost_as_gpu::UnmapBuffer(IoctlUnmapBuffer& params) {
    std::scoped_lock lock(mutex);
    if (auto const offset_it = map_buffer_offsets.find(params.offset); offset_it != map_buffer_offsets.end()) {
        LOG_DEBUG(Service_NVDRV, "called, offset={:#x}", params.offset);
        if (!vm.initialised) {
            return NvResult::BadValue;
        }

        auto const it = mapping_map.find(params.offset);
        auto const mapping = it->second;
        if (!mapping.fixed) {
            auto& allocator{mapping.big_page ? *vm.big_page_allocator : *vm.small_page_allocator};
            u32 page_size_bits{mapping.big_page ? vm.big_page_size_bits : VM::PAGE_SIZE_BITS};
            allocator.Free(u32(mapping.offset >> page_size_bits), u32(mapping.size >> page_size_bits));
        }

        // Sparse mappings shouldn't be fully unmapped, just returned to their sparse state
        // Only FreeSpace can unmap them fully
        if (mapping.sparse_alloc) {
            gmmu->MapSparse(params.offset, mapping.size, mapping.big_page);
        } else {
            gmmu->Unmap(params.offset, mapping.size);
        }

        nvmap.UnpinHandle(mapping.handle);
        PulseForgetRemaps(params.offset, mapping.size);
        mapping_map.erase(params.offset);
        map_buffer_offsets.erase(params.offset);
    }
    return NvResult::Success;
}

NvResult nvhost_as_gpu::BindChannel(IoctlBindChannel& params, DeviceFD fd) {
    LOG_DEBUG(Service_NVDRV, "called, fd={:X}", params.fd);

    auto gpu_channel_device = module.GetDevice<nvhost_gpu>(params.fd);
    if (!gpu_channel_device) {
        return NvResult::BadValue;
    }
    gpu_channel_device->channel_state->memory_manager = gmmu.get();
    gpu_channel_device->bound_as_fd = fd;
    return NvResult::Success;
}

void nvhost_as_gpu::PulseForgetRemaps(u64 offset, u64 size) {
    const u64 end = offset + size;
    auto it = pulse_remaps.lower_bound(offset);
    if (it != pulse_remaps.begin()) {
        const auto previous = std::prev(it);
        if (previous->first + previous->second.size > offset) it = previous;
    }
    while (it != pulse_remaps.end() && it->first < end) {
        const u64 start = it->first;
        const auto remap = it->second;
        it = pulse_remaps.erase(it);
        if (start < offset) {
            auto head = remap;
            head.size = offset - start;
            pulse_remaps.emplace(start, head);
        }
        if (start + remap.size > end) {
            auto tail = remap;
            tail.size = start + remap.size - end;
            if (tail.handle) tail.handle_offset += end - start;
            it = pulse_remaps.emplace(end, tail).first;
        }
    }
}

bool nvhost_as_gpu::PulseCapture(DeviceFD fd, Service::Pulse::StateWriter& out) const {
    std::scoped_lock lock(mutex);
    out.Bool(vm.initialised);
    if (!vm.initialised) return true;
    out.U32(vm.big_page_size);
    out.U64(vm.va_range_start);
    out.U64(vm.va_range_split);
    out.U64(vm.va_range_end);
    out.U32(static_cast<u32>(allocation_map.size()));
    for (const auto& [offset, allocation] : allocation_map) {
        out.U64(offset);
        out.U64(allocation.size);
        out.U32(allocation.page_size);
        out.Bool(allocation.sparse);
        out.Bool(allocation.big_pages);
        out.U32(static_cast<u32>(allocation.mappings.size()));
        for (const u64 mapping : allocation.mappings) out.U64(mapping);
    }
    out.U32(static_cast<u32>(mapping_map.size()));
    for (const auto& [offset, mapping] : mapping_map) {
        out.U64(offset);
        out.U32(mapping.handle);
        out.U64(mapping.handle_offset);
        out.U64(mapping.size);
        out.U32(mapping.kind);
        out.Bool(mapping.fixed);
        out.Bool(mapping.big_page);
        out.Bool(mapping.sparse_alloc);
    }
    out.U32(static_cast<u32>(map_buffer_offsets.size()));
    for (const s64 offset : map_buffer_offsets) out.S64(offset);
    out.U32(static_cast<u32>(pulse_remaps.size()));
    for (const auto& [offset, remap] : pulse_remaps) {
        out.U64(offset);
        out.U64(remap.size);
        out.U32(remap.handle);
        out.U64(remap.handle_offset);
        out.U32(remap.kind);
        out.Bool(remap.big_page);
    }
    return true;
}

void nvhost_as_gpu::PulseRestore(DeviceFD fd, Service::Pulse::StateReader& in) {
    if (!in.Bool()) return;
    IoctlAllocAsEx params{};
    params.big_page_size = in.U32();
    params.va_range_start = in.U64();
    params.va_range_split = in.U64();
    params.va_range_end = in.U64();
    if (AllocAsEx(params) != NvResult::Success) {
        throw std::invalid_argument("Pulse: GPU address space layout");
    }

    std::scoped_lock lock(mutex);
    // The session restored its nvmap handles first; each mapping pins its handle again.
    const auto device_address = [&](NvCore::NvMap::Handle::Id handle, u64 handle_offset) {
        if (!nvmap.GetHandle(handle)) {
            throw std::invalid_argument("Pulse: GPU mapping of a missing nvmap handle");
        }
        return static_cast<DAddr>(nvmap.PinHandle(handle, false) + handle_offset);
    };
    const auto page_bits = [&](bool big_page) {
        return big_page ? vm.big_page_size_bits : VM::PAGE_SIZE_BITS;
    };
    const auto allocator = [&](bool big_page) -> VM::Allocator& {
        return big_page ? *vm.big_page_allocator : *vm.small_page_allocator;
    };

    const auto allocations = in.Count(26);
    for (u32 i = 0; i < allocations; ++i) {
        const auto offset = in.U64();
        Allocation allocation{};
        allocation.size = in.U64();
        allocation.page_size = in.U32();
        allocation.sparse = in.Bool();
        allocation.big_pages = in.Bool();
        if (allocation.page_size != VM::YUZU_PAGESIZE && allocation.page_size != vm.big_page_size) {
            throw std::invalid_argument("Pulse: GPU allocation page size");
        }
        const bool big = allocation.page_size != VM::YUZU_PAGESIZE;
        allocator(big).AllocateFixed(static_cast<u32>(offset >> page_bits(big)),
                                     static_cast<u32>(allocation.size >> page_bits(big)));
        if (allocation.sparse) gmmu->MapSparse(offset, allocation.size);
        allocation.mappings.resize(in.Count(8));
        for (auto& mapping : allocation.mappings) mapping = in.U64();
        allocation_map[offset] = std::move(allocation);
    }

    const auto mappings = in.Count(35);
    for (u32 i = 0; i < mappings; ++i) {
        const auto offset = in.U64();
        const auto handle = in.U32();
        const auto handle_offset = in.U64();
        const auto size = in.U64();
        const auto kind = in.U32();
        const bool fixed = in.Bool();
        const bool big_page = in.Bool();
        const bool sparse_alloc = in.Bool();
        const auto ptr = device_address(handle, handle_offset);
        if (fixed) {
            gmmu->Map(offset, ptr, size, static_cast<Tegra::PTEKind>(kind), big_page);
        } else {
            const u64 aligned = Common::AlignUp(size, big_page ? vm.big_page_size : VM::YUZU_PAGESIZE);
            allocator(big_page).AllocateFixed(static_cast<u32>(offset >> page_bits(big_page)),
                                              static_cast<u32>(aligned >> page_bits(big_page)));
            gmmu->Map(offset, ptr, aligned, static_cast<Tegra::PTEKind>(kind), big_page);
        }
        Mapping mapping(handle, ptr, offset, size, fixed, big_page, sparse_alloc);
        mapping.handle_offset = handle_offset;
        mapping.kind = kind;
        mapping_map.insert_or_assign(offset, mapping);
    }

    const auto offsets = in.Count(8);
    for (u32 i = 0; i < offsets; ++i) map_buffer_offsets.insert(in.S64());

    const auto remaps = in.Count(33);
    for (u32 i = 0; i < remaps; ++i) {
        const auto offset = in.U64();
        PulseRemap remap{};
        remap.size = in.U64();
        remap.handle = in.U32();
        remap.handle_offset = in.U64();
        remap.kind = in.U32();
        remap.big_page = in.Bool();
        if (remap.handle) {
            gmmu->Map(offset, device_address(remap.handle, remap.handle_offset), remap.size,
                      static_cast<Tegra::PTEKind>(remap.kind), remap.big_page);
        } else {
            gmmu->MapSparse(offset, remap.size, remap.big_page);
        }
        pulse_remaps.emplace(offset, remap);
    }
}

void nvhost_as_gpu::GetVARegionsImpl(IoctlGetVaRegions& params) {
    params.buf_size = 2 * sizeof(VaRegion);

    params.regions = std::array<VaRegion, 2>{
        VaRegion{
            .offset = vm.small_page_allocator->GetVAStart() << VM::PAGE_SIZE_BITS,
            .page_size = VM::YUZU_PAGESIZE,
            ._pad0_{},
            .pages = vm.small_page_allocator->GetVALimit() - vm.small_page_allocator->GetVAStart(),
        },
        VaRegion{
            .offset = vm.big_page_allocator->GetVAStart() << vm.big_page_size_bits,
            .page_size = vm.big_page_size,
            ._pad0_{},
            .pages = vm.big_page_allocator->GetVALimit() - vm.big_page_allocator->GetVAStart(),
        },
    };
}

NvResult nvhost_as_gpu::GetVARegions1(IoctlGetVaRegions& params) {
    LOG_DEBUG(Service_NVDRV, "called, buf_addr={:X}, buf_size={:X}", params.buf_addr, params.buf_size);

    std::scoped_lock lock(mutex);

    if (!vm.initialised) {
        return NvResult::BadValue;
    }

    GetVARegionsImpl(params);

    return NvResult::Success;
}

NvResult nvhost_as_gpu::GetVARegions3(IoctlGetVaRegions& params, std::span<VaRegion> regions) {
    LOG_DEBUG(Service_NVDRV, "called, buf_addr={:X}, buf_size={:X}", params.buf_addr,
              params.buf_size);

    std::scoped_lock lock(mutex);

    if (!vm.initialised) {
        return NvResult::BadValue;
    }

    GetVARegionsImpl(params);

    const size_t num_regions = (std::min)(params.regions.size(), regions.size());
    for (size_t i = 0; i < num_regions; i++) {
        regions[i] = params.regions[i];
    }

    return NvResult::Success;
}

Kernel::KEvent* nvhost_as_gpu::QueryEvent(u32 event_id) {
    LOG_CRITICAL(Service_NVDRV, "Unknown AS GPU Event {}", event_id);
    return nullptr;
}

} // namespace Service::Nvidia::Devices
