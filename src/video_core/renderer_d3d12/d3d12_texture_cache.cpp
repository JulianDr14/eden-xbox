// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video_core/renderer_d3d12/d3d12_texture_cache_internal.h"

#include <algorithm>
#include <atomic>
#include <optional>
#include <stdexcept>
#include <utility>

#include <fmt/format.h>

#include "common/alignment.h"
#include "common/logging.h"
#include "video_core/frame_trace.h"
#include "video_core/perf_counters.h"
#include "video_core/renderer_d3d12/d3d12_barrier_batch.h"
#include "video_core/renderer_d3d12/d3d12_blit_image.h"
#include "video_core/renderer_d3d12/d3d12_scheduler.h"
#include "video_core/surface.h"
#include "video_core/texture_cache/render_targets.h"
#include "video_core/texture_cache/samples_helper.h"
#include "video_core/texture_cache/texture_cache.h"

namespace D3D12 {

using namespace TextureDetail;


void SetBcArrayDecode(bool enabled) {
    decode_bc_arrays.store(enabled, std::memory_order_relaxed);
}

void SetAstcGpuDecode(bool enabled) {
    gpu_astc_decode.store(enabled, std::memory_order_relaxed);
}


void SetAstcGpuFresh(bool enabled) {
    gpu_astc_fresh.store(enabled, std::memory_order_relaxed);
}

void SetAstcGpuSync(bool enabled) {
    gpu_astc_sync.store(enabled, std::memory_order_relaxed);
}

void SetAstcGpuVerify(bool enabled) {
    gpu_astc_verify.store(enabled ? 64 : 0, std::memory_order_relaxed);
}

TextureCacheRuntime::TextureCacheRuntime(const Device& device_, Scheduler& scheduler_,
                                         StagingBufferPool& staging_,
                                         TransferBufferPool& transfer_buffers_,
                                         CpuDescriptorAllocator& views,
                                         CpuDescriptorAllocator& samplers,
                                         CpuDescriptorAllocator& rtvs,
                                         CpuDescriptorAllocator& dsvs)
    : device{device_}, scheduler{scheduler_}, staging{staging_}, view_descriptors{views},
      sampler_descriptors{samplers}, rtv_descriptors{rtvs}, dsv_descriptors{dsvs},
      texture_allocator{device_, scheduler_}, transfer_buffers{transfer_buffers_} {
    null_rtv = rtv_descriptors.Allocate();
    const D3D12_RENDER_TARGET_VIEW_DESC null_desc{
        .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
        .ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D,
        .Texture2D = {.MipSlice = 0, .PlaneSlice = 0},
    };
    device.Get()->CreateRenderTargetView(nullptr, &null_desc, null_rtv);
    D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
    supports_min_max_filter =
        SUCCEEDED(device.Get()->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options,
                                                    sizeof(options))) &&
        options.TiledResourcesTier >= D3D12_TILED_RESOURCES_TIER_2;
    LOG_INFO(Render, "D3D12: texture cache runtime ready (min/max sampler reduction {})",
             supports_min_max_filter ? "yes" : "no");
}

TextureCacheRuntime::~TextureCacheRuntime() {
    scheduler.DrainForShutdown();
    LOG_INFO(Render, "D3D12: {}", texture_allocator.Report());
    LOG_INFO(Render, "D3D12: GC readbacks queued {}, ready {}, stale {}, sync {}, pending {} KiB, peak {} KiB",
             gc_queued, gc_ready, gc_stale, gc_sync, gc_pending_bytes / 1024,
             gc_peak_pending_bytes / 1024);
}

FormatInfo TextureCacheRuntime::Format(PixelFormat format) const { return NativeFormat(format); }
void TextureCacheRuntime::Finish() { scheduler.Finish(); }
bool TextureCacheRuntime::CanAccelerateImageUpload(Image& image) const noexcept {
    return image.IsGpuDecoded() && blit_helper && blit_helper->CanDecodeAstc() &&
           (image.TransferFormat().copy_format != PixelFormat::BC3_UNORM ||
            blit_helper->CanEncodeBc3());
}
StagingBufferRef TextureCacheRuntime::UploadStagingBuffer(size_t size, bool deferred) {
    return staging.Request(size, MemoryUsage::Upload, deferred);
}
StagingBufferRef TextureCacheRuntime::DownloadStagingBuffer(size_t size, bool deferred) {
    return staging.Request(size, MemoryUsage::Download, deferred);
}
void TextureCacheRuntime::FreeDeferredStagingBuffer(StagingBufferRef& ref) { staging.FreeDeferred(ref); }
void TextureCacheRuntime::ReleaseGcReadback(StagingBufferRef& map) {
    if (!map.buffer) return;
    gc_pending_bytes -= map.capacity;
    staging.FreeDeferred(map); // Remains fence-protected even if an obsolete copy is still in flight.
    map.buffer = nullptr;
}

bool TextureCacheRuntime::PrepareGcDownload(Image& image,
                                           std::span<const BufferImageCopy> copies,
                                           StagingBufferRef& map) {
    const VideoCore::FrameTrace::ScopedSpan trace_prepare{
        VideoCore::FrameTrace::Event::TextureGcPrepare, image.gpu_addr};
    constexpr u64 PendingBudget = 8ULL * 1024 * 1024;
    constexpr u64 RecoveryHeadroom = 64ULL * 1024 * 1024;
    const bool recover_now = pressure_snapshot.app_limit != 0 &&
                             pressure_snapshot.AppFree() < RecoveryHeadroom;
    auto& pending = image.gc_readback;
    if (pending && (pending->modification_tick != image.modification_tick ||
                    pending->write_version != image.write_version ||
                    True(image.flags & VideoCommon::ImageFlagBits::CpuModified))) {
        pending.reset();
        ++gc_stale;
        VideoCore::FrameTrace::Mark(VideoCore::FrameTrace::Event::TextureGcReadback, 2, image.gpu_addr);
    }
    if (True(image.flags & VideoCommon::ImageFlagBits::CpuModified)) return false;
    if (pending) {
        if (!scheduler.IsFree(pending->tick)) {
            if (!recover_now) return false;
            VideoCore::FrameTrace::Mark(VideoCore::FrameTrace::Event::TextureGcSyncReason, 0, image.gpu_addr);
            {
                const VideoCore::FrameTrace::ScopedSpan trace_wait{
                    VideoCore::FrameTrace::Event::TextureGcWait, image.gpu_addr};
                scheduler.Wait(pending->tick);
            }
            ++gc_sync;
            VideoCore::FrameTrace::Mark(VideoCore::FrameTrace::Event::TextureGcReadback, 3, image.gpu_addr);
        }
        if (!pending->copies.empty() && !pending->compacted) {
            const VideoCore::FrameTrace::ScopedSpan trace_compact{
                VideoCore::FrameTrace::Event::TextureGcCompact, image.gpu_addr};
            // Map again after the fence to invalidate CPU caches where needed.
            // The pool's persistent map keeps the pointer alive after this nested Unmap.
            const D3D12_RANGE read_range{pending->map.offset,
                                        pending->map.offset + pending->footprint_bytes};
            void* data{};
            ThrowIfFailed(pending->map.buffer->Map(0, &read_range, &data), "Map GC footprint readback");
            const std::span<u8> bytes{static_cast<u8*>(data) + pending->map.offset,
                                      pending->map.mapped_span.size()};
            for (const auto& copy : pending->copies) CompactGcReadback(bytes, copy.rows);
            const D3D12_RANGE written{pending->map.offset,
                                       pending->map.offset + image.unswizzled_size_bytes};
            pending->map.buffer->Unmap(0, &written);
            pending->compacted = true;
        }
        map = pending->map;
        map.mapped_span = map.mapped_span.first(image.unswizzled_size_bytes);
        ++gc_ready;
        VideoCore::FrameTrace::Mark(VideoCore::FrameTrace::Event::TextureGcReadback, 1, image.gpu_addr);
        return true;
    }
    const u64 size = image.unswizzled_size_bytes;
    // Large/unsupported transfers and real allocation emergencies preserve the original
    // recovery path. Bound pinned readback memory by the dedicated pool's actual power-of-two size.
    if (recover_now || size > PendingBudget || !image.CanTransfer()) {
        VideoCore::FrameTrace::Mark(VideoCore::FrameTrace::Event::TextureGcSyncReason,
                                    recover_now ? 1 : size > PendingBudget ? 2 : 3, image.gpu_addr);
        {
            const VideoCore::FrameTrace::ScopedSpan trace_staging{
                VideoCore::FrameTrace::Event::TextureGcStaging, image.gpu_addr};
            map = DownloadStagingBuffer(size);
        }
        {
            const VideoCore::FrameTrace::ScopedSpan trace_copy{
                VideoCore::FrameTrace::Event::TextureGcCopy, image.gpu_addr};
            image.DownloadMemory(map, copies);
        }
        {
            const VideoCore::FrameTrace::ScopedSpan trace_wait{
                VideoCore::FrameTrace::Event::TextureGcWait, image.gpu_addr};
            Finish();
        }
        ++gc_sync;
        VideoCore::FrameTrace::Mark(VideoCore::FrameTrace::Event::TextureGcReadback, 3, image.gpu_addr);
        return true;
    }
    auto readback = std::make_unique<Image::GcReadback>();
    readback->footprint_bytes = image.PlanGcDownload(copies, readback->copies);
    const u64 allocation_size = std::max(size, readback->footprint_bytes);
    // A reused buffer may be larger than this estimate; the sum form cannot underflow if the
    // pending total ends a bucket above the budget.
    const u64 reserved = DedicatedStagingSize(allocation_size);
    if (gc_pending_bytes + reserved > PendingBudget) {
        VideoCore::FrameTrace::Mark(VideoCore::FrameTrace::Event::TextureGcReadback, 4, image.gpu_addr);
        return false;
    }
    readback->runtime = this;
    {
        const VideoCore::FrameTrace::ScopedSpan trace_staging{
            VideoCore::FrameTrace::Event::TextureGcStaging, image.gpu_addr};
        readback->map = DownloadStagingBuffer(allocation_size, true);
    }
    gc_pending_bytes += readback->map.capacity;
    gc_peak_pending_bytes = std::max(gc_peak_pending_bytes, gc_pending_bytes);
    {
        const VideoCore::FrameTrace::ScopedSpan trace_copy{
            VideoCore::FrameTrace::Event::TextureGcCopy, image.gpu_addr};
        if (readback->copies.empty()) {
            image.DownloadMemory(readback->map, copies);
        } else {
            image.RecordGcDownload(readback->map, readback->copies);
            VideoCore::FrameTrace::Mark(VideoCore::FrameTrace::Event::TextureGcFootprint,
                                        readback->footprint_bytes, image.gpu_addr);
        }
    }
    // DownloadMemory can write back a reinterpreted view; capture the version AFTER recording.
    readback->write_version = image.write_version;
    readback->modification_tick = image.modification_tick;
    readback->tick = scheduler.CurrentTick();
    pending = std::move(readback);
    ++gc_queued;
    VideoCore::FrameTrace::Mark(VideoCore::FrameTrace::Event::TextureGcReadback, 0, image.gpu_addr);
    return false;
}

void TextureCacheRuntime::CompleteGcDownload(Image& image) {
    image.gc_readback.reset();
}
void TextureCacheRuntime::TickFrame() {
    staging.TickFrame();
    if (!pressure_sampled) {
        pressure_snapshot = device.QueryCacheMemoryPressure();
        pressure_level = cache_pressure.Update(pressure_snapshot);
    }
    texture_allocator.TrimEmptyHeaps(pressure_level != CachePressure::Normal);
    transfer_buffers.Trim(pressure_level != CachePressure::Normal);
    if (VideoCore::FrameTrace::Active()) {
        const auto stats = texture_allocator.GetStats();
        VideoCore::FrameTrace::Mark(VideoCore::FrameTrace::Event::TextureHeapUsage,
                                    stats.heap_bytes, stats.reserved_bytes);
        VideoCore::FrameTrace::Mark(VideoCore::FrameTrace::Event::TextureHeapFree,
                                    stats.free_bytes, stats.largest_free_range);
        VideoCore::FrameTrace::Mark(VideoCore::FrameTrace::Event::TextureHeapPending,
                                    stats.pending_bytes, gc_pending_bytes);
    }
    pressure_sampled = false;
}
std::optional<VideoCommon::TextureGcPolicy> TextureCacheRuntime::GetTextureGcPolicy(bool second_pass) {
    if (!second_pass && !pressure_sampled) {
        pressure_sampled = true;
        pressure_snapshot = device.QueryCacheMemoryPressure();
        pressure_level = cache_pressure.Update(pressure_snapshot);
    }
    if (!second_pass) {
        VideoCore::FrameTrace::Mark(VideoCore::FrameTrace::Event::TextureGcBudget,
                                    pressure_snapshot.app_limit ? pressure_snapshot.AppFree() : UINT64_MAX,
                                    static_cast<u64>(pressure_level));
    }
    if (!pressure_snapshot.Known()) return std::nullopt;
    return CachePressureController::Policy(pressure_level, second_pass,
        CachePressureController::RequiresImmediateRecovery(pressure_snapshot));
}
u64 TextureCacheRuntime::GetDeviceLocalMemory() const { return device.CacheMemoryBudget(); }
u64 TextureCacheRuntime::GetDeviceMemoryUsage() const {
    return pressure_sampled ? device.CacheMemoryUsage(pressure_snapshot) : device.CacheMemoryUsage();
}

bool TextureCacheRuntime::SupportsView(DXGI_FORMAT format, D3D12_FORMAT_SUPPORT1 support1,
                                       D3D12_FORMAT_SUPPORT2 support2) const {
    if (format == DXGI_FORMAT_UNKNOWN) {
        return false;
    }
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{.Format = format};
    {
        std::scoped_lock lock{format_support_mutex};
        if (const auto it = format_support.find(format); it != format_support.end()) {
            support = it->second;
        } else {
            const HRESULT hr = device.Get()->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,
                                                                 &support, sizeof(support));
            if (FAILED(hr)) {
                support.Support1 = D3D12_FORMAT_SUPPORT1_NONE;
                support.Support2 = D3D12_FORMAT_SUPPORT2_NONE;
            }
            LOG_INFO(Render, "D3D12: format {} support 0x{:08x} / 0x{:08x} (hr 0x{:08x})",
                     static_cast<u32>(format), static_cast<u32>(support.Support1),
                     static_cast<u32>(support.Support2), static_cast<u32>(hr));
            format_support.emplace(format, support);
        }
    }
    const bool reported = (support.Support1 & support1) == support1 &&
                          (support.Support2 & support2) == support2;
    // What feature level 11.0 guarantees counts even when the driver does not report it: the
    // console answers "no typed UAV loads" for R8G8B8A8_UNORM, which D3D12 requires.
    const bool wants_render_target = (support1 & D3D12_FORMAT_SUPPORT1_RENDER_TARGET) != 0;
    const bool wants_uav_store = (support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) != 0;
    const D3D12_FORMAT_SUPPORT1 other1 = support1 & ~D3D12_FORMAT_SUPPORT1_RENDER_TARGET;
    const D3D12_FORMAT_SUPPORT2 other2 = support2 & ~D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE;
    const bool guaranteed = other1 == D3D12_FORMAT_SUPPORT1_NONE &&
                            other2 == D3D12_FORMAT_SUPPORT2_NONE &&
                            (!wants_render_target || RequiredRenderTarget(format)) &&
                            (!wants_uav_store || RequiredTypedUavStore(format));
    return reported || guaranteed;
}
Image::Image(TextureCacheRuntime& runtime_, const VideoCommon::ImageInfo& info_, GPUVAddr gpu_addr_,
             VAddr cpu_addr_)
    : VideoCommon::ImageBase{info_, gpu_addr_, cpu_addr_}, runtime{&runtime_},
      format{runtime_.Format(info_.format)} {
    if (decode_bc_arrays.load(std::memory_order_relaxed) && info_.type == ImageType::e2D &&
        info_.resources.layers > 1 && info_.num_samples == 1) {
        if (const std::optional<FormatInfo> decoded = DecodedBcFormat(info_.format)) {
            format = *decoded;
        }
    }
    if (VideoCore::Surface::IsPixelFormatASTC(info_.format)) {
        // Per image: arrays may stay RGBA8 when the rest is recompressed (see
        // VideoCommon::SetAstcArrayRecompression).
        format = AstcFormat(info_.format, VideoCommon::AstcRecompressionFor(info_));
        // GPU mode decodes RGBA8 images straight into the image. BC3 ones, arrays included, go
        // through the bounded scratch texture and the GPU BC3 encoder, one layer at a time.
        const BlitImageHelper* const helper = runtime->blit_helper;
        gpu_decoded = gpu_astc_decode.load(std::memory_order_relaxed) && helper &&
                      helper->CanDecodeAstc() &&
                      info_.type == ImageType::e2D && info_.size.depth == 1 &&
                      info_.num_samples == 1 &&
                      (format.copy_format == PixelFormat::A8B8G8R8_UNORM ||
                       (format.copy_format == PixelFormat::BC3_UNORM && helper->CanEncodeBc3()));
    }
    if (format.converted) {
        flags |= VideoCommon::ImageFlagBits::Converted | VideoCommon::ImageFlagBits::CostlyLoad;
    }
    if (gpu_decoded) {
        flags |= VideoCommon::ImageFlagBits::AcceleratedUpload;
    }
    const auto type = VideoCore::Surface::GetFormatType(info_.format);
    const bool is_color = type == SurfaceType::ColorTexture;
    const bool is_msaa = info_.num_samples > 1;
    D3D12_RESOURCE_FLAGS resource_flags{};
    if (is_color && info_.type != ImageType::e1D &&
        runtime->SupportsView(format.view, D3D12_FORMAT_SUPPORT1_RENDER_TARGET)) {
        resource_flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    }
    // Depth-stencil resources may only be 1D/2D.
    if (!is_color && info_.type != ImageType::e3D) {
        resource_flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    }
    // Multisampled resources cannot have UAVs.
    // The ASTC decoder writes through an R8G8B8A8_UNORM UAV, sRGB images included.
    const bool decoder_writes_image =
        gpu_decoded && format.copy_format == PixelFormat::A8B8G8R8_UNORM;
    if (is_color && !is_msaa &&
        (decoder_writes_image || runtime->SupportsView(format.view, D3D12_FORMAT_SUPPORT1_NONE,
                                              D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE))) {
        resource_flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    }
    const auto [samples_x, samples_y] = VideoCommon::SamplesLog2(info_.num_samples);
    u64 width = std::max<u64>(1, info_.size.width >> samples_x);
    u32 height = std::max(1U, info_.size.height >> samples_y);
    // D3D12 requires block-compressed top levels to be whole blocks; the guest's may not be.
    const u32 block_w = VideoCore::Surface::DefaultBlockWidth(format.copy_format);
    const u32 block_h = VideoCore::Surface::DefaultBlockHeight(format.copy_format);
    if (block_w > 1 || block_h > 1) {
        width = Common::AlignUp(width, block_w);
        height = Common::AlignUp(height, block_h);
    }
    const D3D12_RESOURCE_DESC desc{
        .Dimension = Dimension(info_.type), .Alignment = 0, .Width = width, .Height = height,
        .DepthOrArraySize = static_cast<u16>(info_.type == ImageType::e3D ? info_.size.depth
                                                                          : info_.resources.layers),
        .MipLevels = static_cast<u16>(info_.resources.levels), .Format = format.resource,
        .SampleDesc = {.Count = info_.num_samples, .Quality = 0},
        .Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN, .Flags = resource_flags,
    };
    {
        VideoCore::Perf::ScopedTimer timer{VideoCore::Perf::Counter::ResourceCreateUs,
                                           VideoCore::Perf::Counter::ResourcesCreated};
        auto allocated = runtime->texture_allocator.Create(desc, D3D12_RESOURCE_STATE_COMMON);
        resource = std::move(allocated.resource);
        resource_allocation = std::move(allocated.allocation);
        needs_placed_init = allocated.placed &&
                            (resource_flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                                               D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) != 0;
        if (!resource) {
            throw std::runtime_error("D3D12: texture allocator returned no resource");
        }
    }
    CheckRemovedAfter(runtime->device.Get(), [&] {
        return fmt::format("creating image {} ({} dim {} {}x{}x{} levels {} samples {} flags 0x{:x})",
                           info_.format, static_cast<u32>(desc.Format),
                           static_cast<u32>(desc.Dimension), desc.Width, desc.Height,
                           desc.DepthOrArraySize, desc.MipLevels, desc.SampleDesc.Count,
                           static_cast<u32>(desc.Flags));
    });
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    runtime->device.Get()->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr,
                                                 nullptr);
    footprint_format = footprint.Footprint.Format;
}

Image::Image(const VideoCommon::NullImageParams& params) : VideoCommon::ImageBase{params} {}

Image::GcReadback::~GcReadback() {
    if (runtime) runtime->ReleaseGcReadback(map);
}

Image::Image(Image&&) noexcept = default;

Image::~Image() {
    if (runtime && resource) {
        runtime->texture_allocator.DeferRelease(
            {std::move(resource), std::move(resource_allocation), resource_allocation != nullptr});
    }
    if (runtime && slice_array) {
        runtime->scheduler.DeferRelease(std::move(slice_array));
    }
    if (runtime && reinterpreted) {
        runtime->scheduler.DeferRelease(std::move(reinterpreted));
    }
}

Image& Image::operator=(Image&& other) noexcept {
    if (this != &other) {
        gc_readback.reset();
        if (runtime && resource) {
            runtime->texture_allocator.DeferRelease(
                {std::move(resource), std::move(resource_allocation),
                 resource_allocation != nullptr});
        }
        if (runtime && slice_array) {
            runtime->scheduler.DeferRelease(std::move(slice_array));
        }
        if (runtime && reinterpreted) {
            runtime->scheduler.DeferRelease(std::move(reinterpreted));
        }
        static_cast<VideoCommon::ImageBase&>(*this) = std::move(other);
        allocation_tick = other.allocation_tick;
        runtime = other.runtime;
        gc_readback = std::move(other.gc_readback);
        resource = std::move(other.resource);
        resource_allocation = std::move(other.resource_allocation);
        format = other.format;
        gpu_decoded = other.gpu_decoded;
        footprint_format = other.footprint_format;
        state = other.state;
        needs_placed_init = other.needs_placed_init;
        write_version = other.write_version;
        depth_feedback = std::move(other.depth_feedback);
        depth_feedback_failed = other.depth_feedback_failed;
        slice_array = std::move(other.slice_array);
        slice_array_state = other.slice_array_state;
        slice_array_version = other.slice_array_version;
        reinterpreted = std::move(other.reinterpreted);
        reinterpreted_state = other.reinterpreted_state;
        reinterpreted_version = other.reinterpreted_version;
        reinterpreted_ahead = other.reinterpreted_ahead;
    }
    return *this;
}

Image* Image::PrepareDepthFeedback() {
    if (depth_feedback_failed || !runtime || !resource) {
        return nullptr;
    }
    if (!depth_feedback) {
        depth_feedback = std::make_unique<Image>(*runtime, info, 0, 0);
        if (!depth_feedback->Handle()) {
            depth_feedback.reset();
            depth_feedback_failed = true;
            return nullptr;
        }
        LOG_INFO(Render, "D3D12: depth feedback snapshot allocated ({} {}x{}, {} samples)",
                 info.format, info.size.width, info.size.height, info.num_samples);
    }
    // CopyResource preserves every mip, layer and both depth/stencil planes, including MSAA.
    // The direct queue orders earlier draws, this copy and the next draw; no CPU fence is needed.
    Transition(D3D12_RESOURCE_STATE_COPY_SOURCE);
    depth_feedback->Transition(D3D12_RESOURCE_STATE_COPY_DEST);
    runtime->scheduler.CommandList()->CopyResource(depth_feedback->Handle(), Handle());
    depth_feedback->Transition(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Transition(D3D12_RESOURCE_STATE_DEPTH_WRITE);
    ++runtime->depth_feedback_copies;
    return depth_feedback.get();
}

u32 Image::Subresource(s32 level, s32 layer, u32 plane) const noexcept {
    const u32 levels = static_cast<u32>(info.resources.levels);
    const u32 layers = info.type == ImageType::e3D ? 1U : static_cast<u32>(info.resources.layers);
    return static_cast<u32>(level) + static_cast<u32>(layer) * levels + plane * levels * layers;
}

void Image::Transition(D3D12_RESOURCE_STATES next, BarrierBatch* batch) {
    if (!resource) return;
    if (reinterpreted_ahead || needs_placed_init) {
        // Both record commands on the image: its pending barriers must come first.
        if (batch) {
            batch->Flush();
        }
        if (reinterpreted_ahead) {
            WriteBackReinterpreted();
        }
        if (needs_placed_init) {
            InitializePlacedResource();
        }
    }
    BarrierBatch local{runtime->scheduler};
    BarrierBatch& barriers = batch ? *batch : local;
    if (state == next) {
        // Writes in one state are unordered without a barrier (see Buffer::Transition).
        if (next == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
            ++write_version;
            barriers.Uav(resource.Get());
        } else if (next == D3D12_RESOURCE_STATE_COPY_DEST) {
            ++write_version;
            barriers.Transition(resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                D3D12_RESOURCE_STATE_COMMON);
            barriers.Transition(resource.Get(), D3D12_RESOURCE_STATE_COMMON,
                                D3D12_RESOURCE_STATE_COPY_DEST);
        }
    } else {
        constexpr D3D12_RESOURCE_STATES WRITE_STATES =
            D3D12_RESOURCE_STATE_RENDER_TARGET | D3D12_RESOURCE_STATE_UNORDERED_ACCESS |
            D3D12_RESOURCE_STATE_DEPTH_WRITE | D3D12_RESOURCE_STATE_COPY_DEST |
            D3D12_RESOURCE_STATE_RESOLVE_DEST;
        if ((next & WRITE_STATES) != 0) {
            ++write_version;
        }
        barriers.Transition(resource.Get(), state, next);
        state = next;
    }
    if (!batch) {
        local.Flush();
    }
}

void Image::InitializePlacedResource() {
    needs_placed_init = false;
    const bool depth =
        (resource->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0;
    const D3D12_RESOURCE_STATES target =
        depth ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET;
    const D3D12_RESOURCE_BARRIER barriers[2]{
        {.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING,
         .Aliasing = {.pResourceBefore = nullptr, .pResourceAfter = resource.Get()}},
        {.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
         .Transition = {.pResource = resource.Get(),
                        .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                        .StateBefore = state, .StateAfter = target}},
    };
    auto* const commands = runtime->scheduler.CommandList();
    commands->ResourceBarrier(2, barriers);
    // The heap range may hold another resource's data; RT/DS metadata must be initialized.
    commands->DiscardResource(resource.Get(), nullptr);
    state = target;
    ++write_version;
}

ID3D12Resource* Image::SliceArray() {
    if (slice_array || !resource || info.type != ImageType::e3D) {
        return slice_array.Get();
    }
    D3D12_RESOURCE_DESC desc = resource->GetDesc();
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    const D3D12_HEAP_PROPERTIES heap{.Type = D3D12_HEAP_TYPE_DEFAULT};
    ThrowIfFailed(runtime->device.Get()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                  D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&slice_array)),
                  "Create 3D image slice array");
    CheckRemovedAfter(runtime->device.Get(), [&] {
        return fmt::format("creating the slice array of 3D image {} (format {} {}x{}x{} levels {})",
                           info.format, static_cast<u32>(desc.Format), desc.Width, desc.Height,
                           desc.DepthOrArraySize, desc.MipLevels);
    });
    slice_array_state = D3D12_RESOURCE_STATE_COMMON;
    slice_array_version = 0;
    LOG_INFO(Render, "D3D12: 3D image {} {}x{}x{} @{:x} read as a 2D array through a slice copy",
             info.format, desc.Width, desc.Height, desc.DepthOrArraySize, gpu_addr);
    return slice_array.Get();
}

ID3D12Resource* Image::Reinterpreted(DXGI_FORMAT family) {
    if (reinterpreted) {
        return reinterpreted->GetDesc().Format == family ? reinterpreted.Get() : nullptr;
    }
    if (!resource) {
        return nullptr;
    }
    D3D12_RESOURCE_DESC desc = resource->GetDesc();
    if (desc.SampleDesc.Count > 1) {
        return nullptr; // multisampled texels cannot go through a buffer
    }
    desc.Format = family;
    desc.Flags &= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    const D3D12_HEAP_PROPERTIES heap{.Type = D3D12_HEAP_TYPE_DEFAULT};
    ThrowIfFailed(runtime->device.Get()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                  D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&reinterpreted)),
                  "Create reinterpreted image");
    CheckRemovedAfter(runtime->device.Get(), [&] {
        return fmt::format("creating the DXGI {} copy of image {}", static_cast<u32>(family),
                           info.format);
    });
    reinterpreted_state = D3D12_RESOURCE_STATE_COMMON;
    reinterpreted_version = 0;
    reinterpreted_ahead = false;
    LOG_INFO(Render, "D3D12: image {} {}x{} @{:x} viewed as DXGI {} through a copy", info.format,
             desc.Width, desc.Height, gpu_addr, static_cast<u32>(family));
    return reinterpreted.Get();
}

void Image::RefreshReinterpreted() {
    if (!reinterpreted || reinterpreted_ahead || reinterpreted_version == write_version ||
        !CanTransfer()) {
        return;
    }
    Transition(D3D12_RESOURCE_STATE_COPY_SOURCE);
    TransitionReinterpreted(D3D12_RESOURCE_STATE_COPY_DEST);
    CopyThroughBuffer(resource.Get(), reinterpreted.Get());
    reinterpreted_version = write_version;
}

void Image::WriteBackReinterpreted() {
    // Cleared first: the Transition below comes back here otherwise.
    reinterpreted_ahead = false;
    TransitionReinterpreted(D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(D3D12_RESOURCE_STATE_COPY_DEST);
    CopyThroughBuffer(reinterpreted.Get(), resource.Get());
    reinterpreted_version = write_version;
}

void Image::ReadReinterpreted() {
    if (!reinterpreted) {
        return;
    }
    RefreshReinterpreted();
    TransitionReinterpreted(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}

void Image::RenderToReinterpreted() {
    if (!reinterpreted) {
        return;
    }
    RefreshReinterpreted();
    TransitionReinterpreted(D3D12_RESOURCE_STATE_RENDER_TARGET);
    reinterpreted_ahead = true;
}

void Image::TransitionReinterpreted(D3D12_RESOURCE_STATES next) {
    if (reinterpreted_state != next) {
        TransitionBuffer(runtime->scheduler.CommandList(), reinterpreted.Get(),
                         reinterpreted_state, next);
        reinterpreted_state = next;
    }
}

void Image::CopyThroughBuffer(ID3D12Resource* src, ID3D12Resource* dst) {
    // Same texel size, so both resources share the buffer layout: each subresource goes to its
    // footprint in the source's format and comes back in the destination's. The caller has put
    // src in COPY_SOURCE and dst in COPY_DEST.
    const D3D12_RESOURCE_DESC src_desc = src->GetDesc();
    const DXGI_FORMAT dst_format = dst->GetDesc().Format;
    const u32 count =
        src_desc.MipLevels * (src_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
                                  ? 1U
                                  : src_desc.DepthOrArraySize);
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(count);
    u64 total_bytes = 0;
    runtime->device.Get()->GetCopyableFootprints(&src_desc, 0, count, 0, footprints.data(),
                                                 nullptr, nullptr, &total_bytes);
    ComPtr<ID3D12Resource> transfer = runtime->transfer_buffers.Acquire(total_bytes);
    auto* const commands = runtime->scheduler.CommandList();
    for (u32 subresource = 0; subresource < count; ++subresource) {
        const D3D12_TEXTURE_COPY_LOCATION from{
            .pResource = src, .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
            .SubresourceIndex = subresource};
        const D3D12_TEXTURE_COPY_LOCATION to{.pResource = transfer.Get(),
                                             .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
                                             .PlacedFootprint = footprints[subresource]};
        commands->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    }
    TransitionBuffer(commands, transfer.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                     D3D12_RESOURCE_STATE_COPY_SOURCE);
    for (u32 subresource = 0; subresource < count; ++subresource) {
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = footprints[subresource];
        footprint.Footprint.Format = dst_format;
        const D3D12_TEXTURE_COPY_LOCATION from{.pResource = transfer.Get(),
                                               .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
                                               .PlacedFootprint = footprint};
        const D3D12_TEXTURE_COPY_LOCATION to{
            .pResource = dst, .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
            .SubresourceIndex = subresource};
        commands->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    }
    runtime->transfer_buffers.Release(std::move(transfer));
}

void Image::RefreshSliceArray() {
    if (!SliceArray() || slice_array_version == write_version || !CanTransfer()) {
        return;
    }
    // D3D12 copies between a 3D and a 2D texture only through a buffer: every slice of every
    // level goes to the footprint of its array subresource, then from there into the array.
    const D3D12_RESOURCE_DESC desc = slice_array->GetDesc();
    const u32 levels = desc.MipLevels;
    const u32 layers = desc.DepthOrArraySize;
    const u32 count = levels * layers;
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(count);
    u64 total_bytes = 0;
    runtime->device.Get()->GetCopyableFootprints(&desc, 0, count, 0, footprints.data(), nullptr,
                                                 nullptr, &total_bytes);
    ComPtr<ID3D12Resource> transfer = runtime->transfer_buffers.Acquire(total_bytes);
    auto* const commands = runtime->scheduler.CommandList();
    Transition(D3D12_RESOURCE_STATE_COPY_SOURCE);
    const auto slices = [&](u32 level) { return std::min(layers, std::max(1U, layers >> level)); };
    for (u32 level = 0; level < levels; ++level) {
        for (u32 z = 0; z < slices(level); ++z) {
            const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint = footprints[level + z * levels];
            const D3D12_TEXTURE_COPY_LOCATION src{
                .pResource = resource.Get(), .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
                .SubresourceIndex = level};
            const D3D12_TEXTURE_COPY_LOCATION dst{.pResource = transfer.Get(),
                                                  .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
                                                  .PlacedFootprint = footprint};
            const D3D12_BOX box{0, 0, z, footprint.Footprint.Width, footprint.Footprint.Height,
                                z + 1};
            commands->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
        }
    }
    TransitionBuffer(commands, transfer.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                     D3D12_RESOURCE_STATE_COPY_SOURCE);
    if (slice_array_state != D3D12_RESOURCE_STATE_COPY_DEST) {
        TransitionBuffer(commands, slice_array.Get(), slice_array_state,
                         D3D12_RESOURCE_STATE_COPY_DEST);
    }
    for (u32 level = 0; level < levels; ++level) {
        for (u32 z = 0; z < slices(level); ++z) {
            const u32 subresource = level + z * levels;
            const D3D12_TEXTURE_COPY_LOCATION src{.pResource = transfer.Get(),
                                                  .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
                                                  .PlacedFootprint = footprints[subresource]};
            const D3D12_TEXTURE_COPY_LOCATION dst{
                .pResource = slice_array.Get(), .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
                .SubresourceIndex = subresource};
            commands->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }
    }
    slice_array_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    TransitionBuffer(commands, slice_array.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                     slice_array_state);
    runtime->transfer_buffers.Release(std::move(transfer));
    slice_array_version = write_version;
}
void TextureCacheRuntime::TransitionImageLayout(Image& image) {
    image.Transition(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}
template class VideoCommon::TextureCache<TextureCacheParams>;

} // namespace D3D12
