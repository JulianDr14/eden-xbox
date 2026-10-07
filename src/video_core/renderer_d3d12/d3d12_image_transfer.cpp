// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video_core/renderer_d3d12/d3d12_texture_cache_internal.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <utility>

#include "common/alignment.h"
#include "common/bug_tracker.h"
#include "common/div_ceil.h"
#include "common/logging.h"
#include "video_core/frame_trace.h"
#include "video_core/perf_counters.h"
#include "video_core/renderer_d3d12/d3d12_blit_image.h"
#include "video_core/renderer_d3d12/d3d12_log.h"
#include "video_core/renderer_d3d12/d3d12_resource_utils.h"
#include "video_core/renderer_d3d12/d3d12_scheduler.h"
#include "video_core/surface.h"

// Uploads to and downloads from images through staging and guest buffers.

namespace D3D12 {

using namespace TextureDetail;

Image::CopyLayout Image::Layout(const BufferImageCopy& copy) const {
    const u32 block_w = VideoCore::Surface::DefaultBlockWidth(format.copy_format);
    const u32 block_h = VideoCore::Surface::DefaultBlockHeight(format.copy_format);
    const u32 bytes = VideoCore::Surface::BytesPerBlock(format.copy_format);
    const u32 row_length = copy.buffer_row_length != 0 ? copy.buffer_row_length
                                                       : copy.image_extent.width;
    const u32 image_height = copy.buffer_image_height != 0 ? copy.buffer_image_height
                                                           : copy.image_extent.height;
    CopyLayout layout{};
    layout.row_bytes = std::max(1U, Common::DivCeil(row_length, block_w)) * bytes;
    layout.row_pitch = AlignPitch(layout.row_bytes);
    layout.width = Common::AlignUp(copy.image_extent.width, block_w);
    layout.height = Common::AlignUp(copy.image_extent.height, block_h);
    layout.rows = std::max(1U, layout.height / block_h);
    layout.depth = info.type == ImageType::e3D ? std::max(1U, copy.image_extent.depth) : 1U;
    const u32 staging_rows = std::max(layout.rows, Common::DivCeil(image_height, block_h));
    layout.tight_slice = static_cast<u64>(layout.row_bytes) * staging_rows;
    layout.padded_slice = static_cast<u64>(layout.row_pitch) * layout.rows;
    return layout;
}

u64 Image::TransferBytes(std::span<const BufferImageCopy> copies) const {
    u64 bytes = 0;
    for (const auto& copy : copies) {
        const CopyLayout layout = Layout(copy);
        bytes += layout.tight_slice * layout.depth *
                 static_cast<u64>(std::max(1, copy.image_subresource.num_layers));
    }
    return bytes;
}

void Image::LogConvertedUpload(const u8* data, const CopyLayout& layout,
                               const BufferImageCopy& copy) const {
    // The CPU-decoded (ASTC) uploads of the first frames: a hash of what the decoder produced and
    // how much of it is transparent, comparable between the console and the PC.
    static std::atomic<u32> logged{0};
    constexpr u32 MAX_LOGGED = 48;
    if (logged.fetch_add(1, std::memory_order_relaxed) >= MAX_LOGGED) {
        return;
    }
    const u64 size = layout.tight_slice * layout.depth;
    u32 hash = 2166136261U;
    u64 transparent = 0;
    const bool rgba8 = format.copy_format == PixelFormat::A8B8G8R8_UNORM;
    for (u64 i = 0; i < size; ++i) {
        hash = (hash ^ data[i]) * 16777619U;
        if (rgba8 && i % 4 == 3 && data[i] == 0) {
            ++transparent;
        }
    }
    const u64 texels = rgba8 ? std::max<u64>(1, size / 4) : 1;
    LOG_INFO(Render,
             "D3D12: converted upload {} {}x{} level {} @{:x}: {} bytes, hash {:08x}, {}% "
             "transparent",
             info.format, copy.image_extent.width, copy.image_extent.height,
             copy.image_subresource.base_level, gpu_addr, size, hash,
             rgba8 ? transparent * 100 / texels : 0);
}

bool Image::AreCopyCompatible(const Image& a, const Image& b) noexcept {
    if (!a.format.converted && !b.format.converted) {
        return true;
    }
    const PixelFormat left = a.format.copy_format;
    const PixelFormat right = b.format.copy_format;
    return VideoCore::Surface::DefaultBlockWidth(left) ==
               VideoCore::Surface::DefaultBlockWidth(right) &&
           VideoCore::Surface::DefaultBlockHeight(left) ==
               VideoCore::Surface::DefaultBlockHeight(right) &&
           VideoCore::Surface::BytesPerBlock(left) == VideoCore::Surface::BytesPerBlock(right);
}

bool Image::CanTransfer() const {
    if (!resource) {
        return false;
    }
    if (!format.supported) {
        BUG_TRACK_KEY(UnsupportedFormat, static_cast<u64>(info.format),
                      "texture format {} has no DXGI mapping; its contents are not transferred "
                      "({}x{})",
                      info.format, info.size.width, info.size.height);
        WarnOnceLog(logged_unsupported_transfer, "texture format {} has no DXGI mapping yet; its "
                    "contents are not transferred", info.format);
        return false;
    }
    if (IsDepthStencilPlanar()) {
        // D3D12 stores depth and stencil in separate planes; the guest packs them together, so
        // the pack shaders move them (UploadDepthStencil, DownloadDepthStencil).
        const BlitImageHelper* const helper = runtime->blit_helper;
        if (!helper || !helper->CanPackDepthStencil() || info.num_samples > 1 ||
            !GuestDepthStencilLayout(info.format)) {
            WarnOnce(logged_depth_stencil_transfer, "depth-stencil ({}, {} samples) contents "
                     "are not transferred: {}", info.format, info.num_samples,
                     info.num_samples > 1 ? "multisampled" : "no pack shaders");
            return false;
        }
    }
    return true;
}

void Image::UploadMemory(ID3D12Resource* buffer, size_t base_offset,
                         std::span<const BufferImageCopy> copies) {
    UploadMemoryImpl(buffer, base_offset, nullptr, copies);
}

void Image::UploadMemoryImpl(ID3D12Resource* buffer, size_t base_offset, u8* mapped_at_base,
                             std::span<const BufferImageCopy> copies) {
    if (!CanTransfer() || copies.empty()) {
        return;
    }
    VideoCore::Perf::Add(VideoCore::Perf::Counter::TextureUploads, 1);
    VideoCore::Perf::Add(VideoCore::Perf::Counter::TextureUploadBytes, TransferBytes(copies));
    if (IsDepthStencilPlanar()) {
        UploadDepthStencil(buffer, base_offset, copies);
        return;
    }
    auto* const commands = runtime->scheduler.CommandList();
    bool unmap_source = false;
    if (!mapped_at_base && HeapType(buffer) == D3D12_HEAP_TYPE_UPLOAD) {
        VideoCore::Perf::ScopedNsTimer timer{VideoCore::Perf::Counter::TextureUploadMapNs};
        void* mapped{};
        const D3D12_RANGE no_read{0, 0};
        ThrowIfFailed(buffer->Map(0, &no_read, &mapped), "Map (texture upload source)");
        mapped_at_base = static_cast<u8*>(mapped) + base_offset;
        unmap_source = true;
        VideoCore::Perf::AddDetailed(VideoCore::Perf::Counter::TextureUploadMaps, 1);
    }
    const bool cpu_visible = mapped_at_base != nullptr;
    Transition(D3D12_RESOURCE_STATE_COPY_DEST);
    bool promoted_source = false;
    for (const auto& copy : copies) {
        const CopyLayout layout = Layout(copy);
        const u32 layers = static_cast<u32>(std::max(1, copy.image_subresource.num_layers));
        if (format.converted && mapped_at_base) {
            LogConvertedUpload(mapped_at_base + copy.buffer_offset, layout, copy);
        }
        for (u32 layer = 0; layer < layers; ++layer) {
            const u64 source_offset = base_offset + copy.buffer_offset +
                                      static_cast<u64>(layer) * layout.depth * layout.tight_slice;
            D3D12_TEXTURE_COPY_LOCATION src{.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
            src.PlacedFootprint.Footprint = {.Format = footprint_format, .Width = layout.width,
                .Height = layout.height, .Depth = layout.depth, .RowPitch = layout.row_pitch};
            ComPtr<ID3D12Resource> transfer;
            if (layout.IsFootprint(source_offset)) {
                // The staging layout already is a valid footprint.
                src.pResource = buffer;
                src.PlacedFootprint.Offset = source_offset;
                promoted_source = !cpu_visible;
            } else if (mapped_at_base) {
                // Repack on the CPU into a pitch-aligned staging allocation.
                const StagingBufferRef packed = runtime->UploadStagingBuffer(
                    static_cast<size_t>(layout.padded_slice * layout.depth));
                {
                    const VideoCore::FrameTrace::ScopedSpan trace_repack{
                        VideoCore::FrameTrace::Event::TextureUploadRepack, gpu_addr};
                    VideoCore::Perf::ScopedNsTimer timer{
                        VideoCore::Perf::Counter::TextureUploadRepackNs};
                    for (u32 z = 0; z < layout.depth; ++z) {
                        for (u32 row = 0; row < layout.rows; ++row) {
                            std::memcpy(packed.mapped_span.data() + z * layout.padded_slice +
                                            static_cast<u64>(row) * layout.row_pitch,
                                        mapped_at_base + copy.buffer_offset +
                                            static_cast<u64>(layer) * layout.depth *
                                                layout.tight_slice +
                                            z * layout.tight_slice +
                                            static_cast<u64>(row) * layout.row_bytes,
                                        layout.row_bytes);
                        }
                    }
                }
                VideoCore::Perf::AddDetailed(VideoCore::Perf::Counter::TextureUploadRepacks, 1);
                src.pResource = packed.buffer;
                src.PlacedFootprint.Offset = packed.offset;
            } else {
                // GPU-only source (DMA from the buffer cache): repack row by row on the GPU.
                transfer = runtime->transfer_buffers.Acquire(layout.padded_slice * layout.depth);
                for (u32 z = 0; z < layout.depth; ++z) {
                    for (u32 row = 0; row < layout.rows; ++row) {
                        commands->CopyBufferRegion(
                            transfer.Get(), z * layout.padded_slice +
                                                static_cast<u64>(row) * layout.row_pitch,
                            buffer, source_offset + z * layout.tight_slice +
                                        static_cast<u64>(row) * layout.row_bytes,
                            layout.row_bytes);
                    }
                }
                TransitionBuffer(commands, transfer.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                 D3D12_RESOURCE_STATE_COPY_SOURCE);
                src.pResource = transfer.Get();
                src.PlacedFootprint.Offset = 0;
                promoted_source = true;
            }
            const D3D12_TEXTURE_COPY_LOCATION dst{
                .pResource = resource.Get(), .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
                .SubresourceIndex = Subresource(copy.image_subresource.base_level,
                                                copy.image_subresource.base_layer +
                                                    static_cast<s32>(layer))};
            {
                VideoCore::Perf::ScopedNsTimer timer{
                    VideoCore::Perf::Counter::TextureUploadRecordNs};
                commands->CopyTextureRegion(&dst, copy.image_offset.x, copy.image_offset.y,
                                            copy.image_offset.z, &src, nullptr);
            }
            VideoCore::Perf::AddDetailed(VideoCore::Perf::Counter::TextureUploadCopies, 1);
            if (transfer) {
                runtime->transfer_buffers.Release(std::move(transfer));
            }
        }
    }
    if (unmap_source) {
        const D3D12_RANGE no_write{0, 0};
        buffer->Unmap(0, &no_write);
    } else if (promoted_source) {
        DecayIfDefault(commands, buffer, D3D12_RESOURCE_STATE_COPY_SOURCE);
    }
}

void Image::UploadMemory(const StagingBufferRef& map, std::span<const BufferImageCopy> copies) {
    ASSERT(map.usage == MemoryUsage::Upload);
    // The staging pool keeps this resource mapped for its complete lifetime. Passing the pointer
    // avoids a Map/GetHeapProperties/Unmap round trip for every small texture upload.
    UploadMemoryImpl(map.buffer, map.offset, map.mapped_span.data(), copies);
}

void Image::DownloadMemory(ID3D12Resource* buffer, size_t offset,
                           std::span<const BufferImageCopy> copies) {
    std::array<ID3D12Resource*, 1> buffers{buffer};
    std::array<size_t, 1> offsets{offset};
    DownloadMemory(buffers, offsets, copies);
}

void Image::DownloadMemory(std::span<ID3D12Resource*> buffers, std::span<size_t> offsets,
                           std::span<const BufferImageCopy> copies) {
    if (!CanTransfer() || copies.empty()) {
        return;
    }
    VideoCore::Perf::Add(VideoCore::Perf::Counter::TextureDownloads, 1);
    VideoCore::Perf::Add(VideoCore::Perf::Counter::TextureDownloadBytes, TransferBytes(copies));
    if (IsDepthStencilPlanar()) {
        DownloadDepthStencil(buffers, offsets, copies);
        return;
    }
    Transition(D3D12_RESOURCE_STATE_COPY_SOURCE);
    auto* const commands = runtime->scheduler.CommandList();
    // As in the Vulkan backend: every buffer receives every copy, each at its own base offset.
    for (size_t target = 0; target < buffers.size(); ++target) {
        ID3D12Resource* const buffer = buffers[target];
        bool promoted = false;
        for (const auto& copy : copies) {
            const CopyLayout layout = Layout(copy);
            const u32 layers = static_cast<u32>(std::max(1, copy.image_subresource.num_layers));
            for (u32 layer = 0; layer < layers; ++layer) {
                const u64 dest_offset = offsets[target] + copy.buffer_offset +
                                        static_cast<u64>(layer) * layout.depth * layout.tight_slice;
                const D3D12_TEXTURE_COPY_LOCATION src{
                    .pResource = resource.Get(), .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
                    .SubresourceIndex = Subresource(copy.image_subresource.base_level,
                                                    copy.image_subresource.base_layer +
                                                        static_cast<s32>(layer))};
                const D3D12_BOX box{
                    static_cast<u32>(copy.image_offset.x), static_cast<u32>(copy.image_offset.y),
                    static_cast<u32>(copy.image_offset.z),
                    static_cast<u32>(copy.image_offset.x) + layout.width,
                    static_cast<u32>(copy.image_offset.y) + layout.height,
                    static_cast<u32>(copy.image_offset.z) + layout.depth};
                D3D12_TEXTURE_COPY_LOCATION dst{.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
                dst.PlacedFootprint.Footprint = {.Format = footprint_format, .Width = layout.width,
                    .Height = layout.height, .Depth = layout.depth, .RowPitch = layout.row_pitch};
                promoted = true;
                if (layout.IsFootprint(dest_offset)) {
                    dst.pResource = buffer;
                    dst.PlacedFootprint.Offset = dest_offset;
                    commands->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
                    continue;
                }
                // The destination wants tight rows: go through a padded buffer, then row copies.
                ComPtr<ID3D12Resource> transfer =
                    runtime->transfer_buffers.Acquire(layout.padded_slice * layout.depth);
                dst.pResource = transfer.Get();
                dst.PlacedFootprint.Offset = 0;
                commands->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
                TransitionBuffer(commands, transfer.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                 D3D12_RESOURCE_STATE_COPY_SOURCE);
                for (u32 z = 0; z < layout.depth; ++z) {
                    for (u32 row = 0; row < layout.rows; ++row) {
                        commands->CopyBufferRegion(
                            buffer, dest_offset + z * layout.tight_slice +
                                        static_cast<u64>(row) * layout.row_bytes,
                            transfer.Get(), z * layout.padded_slice +
                                                static_cast<u64>(row) * layout.row_pitch,
                            layout.row_bytes);
                    }
                }
                runtime->transfer_buffers.Release(std::move(transfer));
            }
        }
        if (promoted) {
            DecayIfDefault(commands, buffer, D3D12_RESOURCE_STATE_COPY_DEST);
        }
    }
}

u64 Image::PlanGcDownload(std::span<const BufferImageCopy> copies,
                           std::vector<GcCopy>& plan) const {
    constexpr u64 Budget = 8ULL * 1024 * 1024;
    constexpr size_t MaxRegions = 256;
    const auto fallback = [&plan]() -> u64 { plan.clear(); return 0; };
    if (!CanTransfer() || IsDepthStencilPlanar() || info.num_samples > 1 ||
        format.copy_format != info.format || copies.empty()) return fallback();
    const auto desc = resource->GetDesc();
    u64 source_end = 0, tight_end = 0;
    for (const auto& copy : copies) {
        if (copy.buffer_offset != tight_end || copy.image_offset.x != 0 ||
            copy.image_offset.y != 0 || copy.image_offset.z != 0) return fallback();
        const auto layout = Layout(copy);
        if (layout.tight_slice != u64{layout.row_bytes} * layout.rows) return fallback();
        const u32 layers = static_cast<u32>(std::max(1, copy.image_subresource.num_layers));
        if (layers > MaxRegions - plan.size()) return fallback();
        for (u32 layer = 0; layer < layers; ++layer) {
            const u32 subresource = Subresource(copy.image_subresource.base_level,
                copy.image_subresource.base_layer + static_cast<s32>(layer));
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
            u32 rows{};
            u64 row_bytes{};
            runtime->device.Get()->GetCopyableFootprints(&desc, subresource, 1,
                                                         Common::AlignUp(source_end, u64{512}),
                                                         &footprint, &rows, &row_bytes, nullptr);
            if (footprint.Footprint.Width != layout.width ||
                footprint.Footprint.Height != layout.height ||
                footprint.Footprint.Depth != layout.depth || rows != layout.rows ||
                row_bytes != layout.row_bytes) return fallback();
            const auto region = PlanGcReadbackRegion(source_end, tight_end, layout.row_bytes,
                footprint.Footprint.RowPitch, rows, layout.depth, Budget);
            if (!region || region->source_offset != footprint.Offset ||
                region->tight_bytes > unswizzled_size_bytes) return fallback();
            plan.push_back({*region, footprint, subresource});
            source_end = region->source_offset + region->source_bytes;
            tight_end += region->tight_bytes;
            if (tight_end > unswizzled_size_bytes) return fallback();
        }
        if (tight_end - copy.buffer_offset != copy.buffer_size) return fallback();
    }
    return source_end;
}

void Image::RecordGcDownload(const StagingBufferRef& map, std::span<const GcCopy> plan) {
    VideoCore::Perf::Add(VideoCore::Perf::Counter::TextureDownloads, 1);
    Transition(D3D12_RESOURCE_STATE_COPY_SOURCE);
    auto* const commands = runtime->scheduler.CommandList();
    for (const auto& copy : plan) {
        VideoCore::Perf::Add(VideoCore::Perf::Counter::TextureDownloadBytes, copy.rows.tight_bytes);
        const D3D12_TEXTURE_COPY_LOCATION src{.pResource = resource.Get(),
            .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, .SubresourceIndex = copy.subresource};
        D3D12_TEXTURE_COPY_LOCATION dst{.pResource = map.buffer,
            .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, .PlacedFootprint = copy.footprint};
        dst.PlacedFootprint.Offset += map.offset;
        commands->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
}

void Image::DownloadMemory(const StagingBufferRef& map, std::span<const BufferImageCopy> copies) {
    DownloadMemory(map.buffer, map.offset, copies);
}

} // namespace D3D12
