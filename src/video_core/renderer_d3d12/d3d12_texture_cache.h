// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include <eden_spirv_to_dxil.h>

#include "shader_recompiler/shader_info.h"
#include "video_core/renderer_d3d12/d3d12_cache_policy.h"
#include "video_core/renderer_d3d12/d3d12_descriptor_heap.h"
#include "video_core/renderer_d3d12/d3d12_gc_readback.h"
#include "video_core/renderer_d3d12/d3d12_resource_allocator.h"
#include "video_core/renderer_d3d12/d3d12_transfer_buffer_pool.h"
#include "video_core/renderer_d3d12/d3d12_staging_buffer_pool.h"
#include "video_core/texture_cache/image_view_base.h"
#include "video_core/texture_cache/texture_cache_base.h"

namespace D3D12 {

using Common::SlotVector;
using VideoCommon::ImageId;
using VideoCommon::NUM_RT;
using VideoCommon::Region2D;

class BarrierBatch;
class BlitImageHelper;
class Device;
class Scheduler;
class Image;
class ImageView;
class Framebuffer;
class Sampler;

/// A depth buffer bound read-only while shaders sample it: D3D12 cannot read a resource in
/// DEPTH_WRITE (the reads return garbage, zeros on the PC), so both uses share this state.
constexpr D3D12_RESOURCE_STATES DEPTH_SAMPLED_STATE =
    D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

struct FormatInfo {
    DXGI_FORMAT resource;
    DXGI_FORMAT view;
    DXGI_FORMAT srv;
    DXGI_FORMAT dsv = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT stencil_srv = DXGI_FORMAT_UNKNOWN;
    /// The CPU decodes the guest data first (ASTC): staging holds copy_format blocks, not guest ones.
    bool converted{};
    /// No DXGI equivalent yet: the image exists (so the cache keeps working) but is not transferred.
    bool supported = true;
    /// Pixel format whose block layout the staging data uses (the guest format unless converted).
    VideoCore::Surface::PixelFormat copy_format = VideoCore::Surface::PixelFormat::Invalid;
};

/// DXGI formats of a guest pixel format (the texture cache's table; texel buffers use it too).
[[nodiscard]] FormatInfo SurfaceFormat(VideoCore::Surface::PixelFormat format);

class TextureCacheRuntime {
    friend Image;
    friend ImageView;
    friend Framebuffer;
    friend Sampler;

public:
    TextureCacheRuntime(const Device& device, Scheduler& scheduler, StagingBufferPool& staging,
                        CpuDescriptorAllocator& views, CpuDescriptorAllocator& samplers,
                        CpuDescriptorAllocator& rtvs, CpuDescriptorAllocator& dsvs);
    ~TextureCacheRuntime();

    void RunSelfTest();
    void Finish();

    /// Where shader blits go (set by the renderer once the helper exists; null means skip).
    void SetBlitHelper(BlitImageHelper* helper) noexcept { blit_helper = helper; }
    StagingBufferRef UploadStagingBuffer(size_t size, bool deferred = false);
    StagingBufferRef DownloadStagingBuffer(size_t size, bool deferred = false);
    void FreeDeferredStagingBuffer(StagingBufferRef& ref);
    /// Maintenance readback: false keeps the image until its copy fence completes. CPU-demand
    /// downloads retain their synchronous path. The returned map is pinned until CompleteGcDownload.
    bool PrepareGcDownload(Image& image, std::span<const VideoCommon::BufferImageCopy> copies,
                           StagingBufferRef& map);
    void CompleteGcDownload(Image& image);
    void ReleaseGcReadback(StagingBufferRef& map);
    void TickFrame();
    [[nodiscard]] CacheMemorySnapshot BeginMemoryGuardFrame() {
        pressure_snapshot = device.QueryCacheMemoryPressure();
        pressure_level = cache_pressure.Update(pressure_snapshot);
        pressure_sampled = true;
        return pressure_snapshot;
    }
    u64 GetDeviceLocalMemory() const;
    u64 GetDeviceMemoryUsage() const;
    std::optional<VideoCommon::TextureGcPolicy> GetTextureGcPolicy(bool second_pass);
    [[nodiscard]] u64 DepthFeedbackCopies() const noexcept { return depth_feedback_copies; }
    bool CanReportMemoryUsage() const { return true; }
    std::optional<size_t> GetSamplerHeapBudget() const { return SamplerHeap::CAPACITY; }

    void CopyImage(Image& dst, Image& src, std::span<const VideoCommon::ImageCopy> copies);
    void CopyImageMSAA(Image& dst, Image& src, std::span<const VideoCommon::ImageCopy> copies);
    /// CopyImage between resource formats of different DXGI families: through a buffer.
    void CopyThroughBuffer(Image& dst, Image& src, std::span<const VideoCommon::ImageCopy> copies);
    bool ShouldReinterpret(Image&, Image&) const noexcept { return false; }
    void ReinterpretImage(Image& dst, Image& src,
                          std::span<const VideoCommon::ImageCopy> copies);
    void ConvertImage(Framebuffer*, ImageView&, ImageView&);
    void BlitImage(Framebuffer*, ImageView&, ImageView&, const Region2D&, const Region2D&,
                   Tegra::Engines::Fermi2D::Filter, Tegra::Engines::Fermi2D::Operation);

    bool CanAccelerateImageUpload(Image& image) const noexcept;
    bool CanUploadMSAA() const noexcept { return false; }
    /// Decodes an ASTC image with the compute shader (images flagged AcceleratedUpload): map
    /// holds the guest's swizzled blocks.
    void AccelerateImageUpload(Image& image, const StagingBufferRef& map,
                               std::span<const VideoCommon::SwizzleParameters> swizzles, u32, u32);
    void InsertUploadMemoryBarrier() {}
    /// Clears are recorded immediately; nothing is deferred.
    void FlushDeferredClear() {}
    bool CanDownloadMsaa(const VideoCommon::ImageInfo&) const noexcept { return false; }
    void TransitionImageLayout(Image& image);
    bool HasBrokenTextureViewFormats() const noexcept { return false; }
    bool HasNativeBgr() const noexcept { return true; }
    bool HasNativeASTC() const noexcept { return false; }
    void BarrierFeedbackLoop() {}

    [[nodiscard]] FormatInfo Format(VideoCore::Surface::PixelFormat format) const;
    /// Whether a typed view format supports all the given capabilities on this device.
    [[nodiscard]] bool SupportsView(DXGI_FORMAT format, D3D12_FORMAT_SUPPORT1 support1,
                                    D3D12_FORMAT_SUPPORT2 support2 = D3D12_FORMAT_SUPPORT2_NONE) const;

    /// RTV (attachment < NUM_RT) or DSV (attachment == NUM_RT) of `count` layers from `first`,
    /// counted from the attachment view's base, for a clear that names layers. Free it with
    /// FreeLayerTarget once recorded. Null when the attachment cannot be viewed by layer.
    [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE LayerTarget(const Framebuffer& framebuffer,
                                                          size_t attachment, u32 first,
                                                          u32 count);
    void FreeLayerTarget(D3D12_CPU_DESCRIPTOR_HANDLE handle, bool depth);

    /// RTV of no resource, bound in the render target slots a framebuffer leaves empty.
    [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE NullRenderTarget() const noexcept {
        return null_rtv;
    }

private:
    void EnsureAstcRgbaScratch(u32 width, u32 height);
    void EnsureAstcBc3Scratch(u64 size);
    /// A blit whose source and destination are the same image (a mip chain built level by level,
    /// a copy between layers or regions): the source region goes through a scratch texture.
    void BlitWithinImage(ImageView& dst, ImageView& src, const Region2D& dst_region,
                         const Region2D& src_region, Tegra::Engines::Fermi2D::Filter filter);
    void EnsureSelfBlitScratch(DXGI_FORMAT format, u32 width, u32 height);
    void TransitionSelfBlitScratch(D3D12_RESOURCE_STATES next);

    const Device& device;
    Scheduler& scheduler;
    StagingBufferPool& staging;
    CpuDescriptorAllocator& view_descriptors;
    CpuDescriptorAllocator& sampler_descriptors;
    CpuDescriptorAllocator& rtv_descriptors;
    CpuDescriptorAllocator& dsv_descriptors;
    TextureResourceAllocator texture_allocator;
    TransferBufferPool transfer_buffers;
    CachePressureController cache_pressure;
    CachePressure pressure_level{};
    CacheMemorySnapshot pressure_snapshot{};
    bool pressure_sampled{};
    u64 gc_pending_bytes{};
    u64 gc_peak_pending_bytes{};
    u64 gc_queued{}, gc_ready{}, gc_stale{}, gc_sync{};
    D3D12_CPU_DESCRIPTOR_HANDLE null_rtv{};
    BlitImageHelper* blit_helper{};
    /// Reused by single-layer ASTC uploads. Bands cap the live decode/encode workspace at 40 MiB.
    ComPtr<ID3D12Resource> astc_rgba_scratch;
    ComPtr<ID3D12Resource> astc_bc3_scratch;
    D3D12_RESOURCE_STATES astc_rgba_state{D3D12_RESOURCE_STATE_COMMON};
    D3D12_RESOURCE_STATES astc_bc3_state{D3D12_RESOURCE_STATE_COMMON};
    u32 astc_rgba_width{};
    u32 astc_rgba_height{};
    u64 astc_bc3_size{};
    /// Source copy for BlitWithinImage, kept for the next blit of the same family.
    ComPtr<ID3D12Resource> self_blit_scratch;
    D3D12_RESOURCE_STATES self_blit_state{D3D12_RESOURCE_STATE_COMMON};
    DXGI_FORMAT self_blit_format{DXGI_FORMAT_UNKNOWN};
    u32 self_blit_width{};
    u32 self_blit_height{};
    /// MIN/MAX sampler reductions need tiled resources tier 2; the Xbox Series reports tier 1 and
    /// creating such a sampler there removes the device (DXGI_ERROR_INVALID_CALL).
    bool supports_min_max_filter{};
    u64 depth_feedback_copies{};
    /// FORMAT_SUPPORT answers by format, each logged once (see SupportsView).
    mutable std::mutex format_support_mutex;
    mutable std::unordered_map<DXGI_FORMAT, D3D12_FEATURE_DATA_FORMAT_SUPPORT> format_support;
};

class Image : public VideoCommon::ImageBase {
    friend TextureCacheRuntime;
public:
    Image(TextureCacheRuntime& runtime, const VideoCommon::ImageInfo& info, GPUVAddr gpu_addr,
          VAddr cpu_addr);
    explicit Image(const VideoCommon::NullImageParams& params);
    /// The resource outlives the Image until the GPU is done with it.
    ~Image();

    Image(const Image&) = delete;
    Image& operator=(const Image&) = delete;
    Image(Image&&) noexcept;
    Image& operator=(Image&& other) noexcept;

    void UploadMemory(ID3D12Resource* buffer, size_t offset,
                      std::span<const VideoCommon::BufferImageCopy> copies);
    void UploadMemory(const StagingBufferRef& map,
                      std::span<const VideoCommon::BufferImageCopy> copies);
    void DownloadMemory(ID3D12Resource* buffer, size_t offset,
                        std::span<const VideoCommon::BufferImageCopy> copies);
    void DownloadMemory(std::span<ID3D12Resource*> buffers, std::span<size_t> offsets,
                        std::span<const VideoCommon::BufferImageCopy> copies);
    void DownloadMemory(const StagingBufferRef& map,
                        std::span<const VideoCommon::BufferImageCopy> copies);

    [[nodiscard]] ID3D12Resource* Handle() const noexcept { return resource.Get(); }
    [[nodiscard]] DXGI_FORMAT ResourceFormat() const noexcept { return format.resource; }
    [[nodiscard]] DXGI_FORMAT ViewFormat() const noexcept { return format.view; }
    /// Format of plane 0's copy footprints (the resource format, typeless included).
    [[nodiscard]] DXGI_FORMAT FootprintFormat() const noexcept { return footprint_format; }
    /// How the guest data is laid out for transfers (converted formats: what the CPU decodes to).
    [[nodiscard]] const FormatInfo& TransferFormat() const noexcept { return format; }
    /// A block-compressed 2D array the CPU decodes on upload (see DecodedBcFormat): its resource
    /// holds plain texels, so it does not copy to or from block-compressed images.
    [[nodiscard]] bool IsBcDecoded() const noexcept {
        return format.converted && !VideoCore::Surface::IsPixelFormatASTC(info.format);
    }
    /// An ASTC image the compute shader decodes into RGBA8 (see AccelerateImageUpload).
    [[nodiscard]] bool IsGpuDecoded() const noexcept {
        return gpu_decoded;
    }
    /// Whether a texture copy between the two resources moves meaningful data: not when one
    /// holds data the host converted (decoded BCn arrays, ASTC) and the other a different block
    /// layout.
    [[nodiscard]] static bool AreCopyCompatible(const Image& a, const Image& b) noexcept;
    /// A depth format with a stencil plane (D24S8, D32S8): D3D12 keeps depth and stencil in two
    /// planes, which the guest packs into one texel.
    [[nodiscard]] bool IsDepthStencilPlanar() const noexcept {
        return format.stencil_srv != DXGI_FORMAT_UNKNOWN;
    }
    /// Copies both planes from src, which has the same resource format.
    void CopyDepthStencilFrom(Image& src, std::span<const VideoCommon::ImageCopy> copies);
    [[nodiscard]] u32 Subresource(s32 level, s32 layer, u32 plane = 0) const noexcept;
    /// Records the barriers into next, or adds them to batch (flushed first when the transition
    /// must record other commands).
    void Transition(D3D12_RESOURCE_STATES next, BarrierBatch* batch = nullptr);
    [[nodiscard]] D3D12_RESOURCE_STATES State() const noexcept { return state; }
    bool IsRescaled() const noexcept { return false; }
    bool ScaleUp(bool = false) { return false; }
    bool ScaleDown(bool = false) { return false; }

    /// 2D array holding the depth slices of this 3D image, for shaders that sample it as a 2D
    /// array (Vulkan's 2D_ARRAY_COMPATIBLE views; D3D12 has no such view, and a 3D SRV where the
    /// shader declares an array reads garbage on the Series). Created on first use.
    [[nodiscard]] ID3D12Resource* SliceArray();
    /// Copies the slices again if the image was written since the last copy; call before a
    /// draw or dispatch that reads SliceArray().
    void RefreshSliceArray();

    /// Snapshot before a draw that samples and writes this depth/stencil attachment. Reuses the
    /// allocation; copies on the GPU without waiting. Null keeps the read-only fallback on failure.
    [[nodiscard]] Image* PrepareDepthFeedback();
    [[nodiscard]] Image* DepthFeedback() const noexcept { return depth_feedback.get(); }

    /// This image's texels in another typeless family of the same texel size, for views D3D12
    /// cannot cast to (the guest reads and renders an R32 image as R16G16, Mario Wonder's world
    /// map). A copy through a buffer, created on first use; null when it cannot be made
    /// (multisampled, or the image already has one in another family).
    [[nodiscard]] ID3D12Resource* Reinterpreted(DXGI_FORMAT family);
    /// Brings the copy up to date and makes it readable; call before a draw or dispatch that
    /// samples Reinterpreted().
    void ReadReinterpreted();
    /// Brings the copy up to date and makes it a render target. The copy then holds the newest
    /// texels until the image itself is used again (any Transition copies them back).
    void RenderToReinterpreted();

    u64 allocation_tick{};

private:
    struct GcCopy {
        GcReadbackRegion rows;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
        u32 subresource;
    };
    struct GcReadback {
        TextureCacheRuntime* runtime{};
        StagingBufferRef map{};
        u64 tick{}, modification_tick{}, write_version{};
        std::vector<GcCopy> copies;
        u64 footprint_bytes{};
        bool compacted{};
        ~GcReadback();
    };
    std::unique_ptr<GcReadback> gc_readback;
    /// Shared upload implementation. mapped_at_base points at base_offset in a persistently
    /// mapped staging allocation; null asks the implementation to inspect and map the resource.
    void UploadMemoryImpl(ID3D12Resource* buffer, size_t base_offset, u8* mapped_at_base,
                          std::span<const VideoCommon::BufferImageCopy> copies);
    /// Staging layout of one BufferImageCopy (see d3d12_texture_cache.cpp).
    struct CopyLayout;
    [[nodiscard]] CopyLayout Layout(const VideoCommon::BufferImageCopy& copy) const;
    /// Guest bytes the copies move (for the performance counters).
    [[nodiscard]] u64 TransferBytes(std::span<const VideoCommon::BufferImageCopy> copies) const;
    /// False (logged once) when this image's data cannot be transferred yet.
    [[nodiscard]] bool CanTransfer() const;
    [[nodiscard]] u64 PlanGcDownload(std::span<const VideoCommon::BufferImageCopy> copies,
                                     std::vector<GcCopy>& plan) const;
    void RecordGcDownload(const StagingBufferRef& map, std::span<const GcCopy> plan);
    /// Copies the texels between the image and its reinterpreted copy through a buffer.
    void RefreshReinterpreted();
    void WriteBackReinterpreted();
    /// Aliasing barrier, transition to the writable state and discard (see needs_placed_init).
    void InitializePlacedResource();
    void CopyThroughBuffer(ID3D12Resource* src, ID3D12Resource* dst);
    void TransitionReinterpreted(D3D12_RESOURCE_STATES next);
    /// Hash and transparency of a CPU-decoded upload (first ones only), to compare machines.
    void LogConvertedUpload(const u8* data, const CopyLayout& layout,
                            const VideoCommon::BufferImageCopy& copy) const;

    /// Copy footprints of both planes of one depth-stencil subresource, in one buffer.
    struct PlaneFootprints {
        std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, 2> planes;
        u64 size;
    };
    [[nodiscard]] PlaneFootprints Footprints(s32 level) const;
    /// Copies both planes of a whole subresource into the footprints of buffer (to_buffer) or
    /// back; the caller puts the buffer in COPY_DEST or COPY_SOURCE.
    void CopyPlanes(s32 level, s32 layer, ID3D12Resource* buffer,
                    const PlaneFootprints& footprints, bool to_buffer);
    /// The guest's packed depth-stencil texels into both planes (the split shader).
    void UploadDepthStencil(ID3D12Resource* buffer, size_t offset,
                            std::span<const VideoCommon::BufferImageCopy> copies);
    /// Both planes into packed guest texels (the merge shader).
    void DownloadDepthStencil(std::span<ID3D12Resource*> buffers, std::span<size_t> offsets,
                              std::span<const VideoCommon::BufferImageCopy> copies);

    TextureCacheRuntime* runtime{};
    ComPtr<ID3D12Resource> resource;
    std::shared_ptr<TextureResourceAllocator::Allocation> resource_allocation;
    FormatInfo format{};
    bool gpu_decoded{};
    DXGI_FORMAT footprint_format{}; ///< format GetCopyableFootprints uses for plane 0
    D3D12_RESOURCE_STATES state{D3D12_RESOURCE_STATE_COMMON};
    /// A placed render target or depth stencil whose heap memory another resource may have used:
    /// D3D12 requires an aliasing barrier and a discard before its first use.
    bool needs_placed_init{};
    /// Bumped by every transition into a writable state: every GPU write to the image follows one.
    u64 write_version{1};
    std::unique_ptr<Image> depth_feedback;
    bool depth_feedback_failed{};
    ComPtr<ID3D12Resource> slice_array;
    D3D12_RESOURCE_STATES slice_array_state{D3D12_RESOURCE_STATE_COMMON};
    u64 slice_array_version{};
    ComPtr<ID3D12Resource> reinterpreted;
    D3D12_RESOURCE_STATES reinterpreted_state{D3D12_RESOURCE_STATE_COMMON};
    u64 reinterpreted_version{};
    /// The copy was rendered to and the image does not have those texels yet.
    bool reinterpreted_ahead{};
};

class ImageView : public VideoCommon::ImageViewBase {
    friend Framebuffer;

public:
    ImageView(TextureCacheRuntime& runtime, const VideoCommon::ImageViewInfo& info, ImageId image_id,
              Image& image);
    ImageView(TextureCacheRuntime& runtime, const VideoCommon::ImageViewInfo& info, ImageId image_id,
              Image& image, SlotVector<Image>& images);
    ImageView(TextureCacheRuntime&, const VideoCommon::ImageInfo&,
              const VideoCommon::ImageViewInfo&, GPUVAddr);
    ImageView(TextureCacheRuntime&, const VideoCommon::NullImageViewParams&);
    ~ImageView();

    ImageView(const ImageView&) = delete;
    ImageView& operator=(const ImageView&) = delete;
    ImageView(ImageView&& other) noexcept;
    ImageView& operator=(ImageView&& other) noexcept;

    /// SRV declared as the shader's texture type: D3D12 requires the view dimension to match
    /// the HLSL declaration, so each type gets its own SRV, created on first use.
    [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE Handle(Shader::TextureType texture_type) const noexcept;
    [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE StorageView(Shader::TextureType,
                                                           Shader::ImageFormat) const noexcept {
        return uav;
    }
    [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE RenderTarget() const noexcept { return rtv; }
    /// The RTV is on the image's reinterpreted copy (Image::RenderToReinterpreted before drawing).
    [[nodiscard]] bool RenderTargetOnCopy() const noexcept { return rtv_on_copy; }
    /// Readies the image for RenderTarget(): the copy or the image itself as a render target.
    void PrepareRender() const;
    [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE DepthStencil() const noexcept { return dsv; }
    /// DSV with depth (and stencil) read-only, for draws that also sample the image.
    [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE DepthStencilReadOnly() const noexcept {
        return dsv_read_only;
    }
    [[nodiscard]] ID3D12Resource* ImageHandle() const noexcept { return image; }
    [[nodiscard]] GPUVAddr GpuAddr() const noexcept { return gpu_addr; }
    [[nodiscard]] u32 BufferSize() const noexcept { return buffer_size; }
    [[nodiscard]] bool IsRescaled() const noexcept;

    /// The texture cache image of this view; null for null and buffer views.
    [[nodiscard]] Image* SourceImage() const noexcept;

    /// Transitions the whole image this view belongs to (no-op for null and buffer views).
    void TransitionImage(D3D12_RESOURCE_STATES state, BarrierBatch* batch = nullptr) const;

    /// Records what reading Handle(texture_type) needs first (a 3D image read as a 2D array
    /// refreshes its slice copy); call before the draw or dispatch, outside any other copy.
    void PrepareRead(Shader::TextureType texture_type) const;

    /// Same format, swizzle and subresources as Handle(), on the prepared depth snapshot.
    [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE DepthFeedbackHandle(
        Shader::TextureType texture_type) const;

private:
    /// What the per-type SRVs are created from.
    struct SrvParams {
        DXGI_FORMAT format{};
        UINT mapping{D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING};
        u32 base_level{};
        u32 levels{1};
        u32 base_layer{};
        u32 layers{1};
        bool is_msaa{};
        D3D12_RESOURCE_DIMENSION dimension{D3D12_RESOURCE_DIMENSION_TEXTURE2D};
        u32 resource_layers{1};
    };

    [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE CreateSrv(
        Shader::TextureType texture_type, ID3D12Resource* override_resource = nullptr) const;
    void Release();
    TextureCacheRuntime* runtime{};
    SlotVector<Image>* slot_images{};
    ID3D12Resource* image{};
    /// The resource the SRVs read: image, or the image's reinterpreted copy.
    ID3D12Resource* srv_resource{};
    SrvParams srv_params{};
    Shader::TextureType natural_type{Shader::TextureType::Color2D};
    mutable std::array<D3D12_CPU_DESCRIPTOR_HANDLE, Shader::NUM_TEXTURE_TYPES> srvs{};
    mutable std::unique_ptr<std::array<D3D12_CPU_DESCRIPTOR_HANDLE, Shader::NUM_TEXTURE_TYPES>>
        depth_feedback_srvs;
    D3D12_CPU_DESCRIPTOR_HANDLE uav{};
    D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
    bool rtv_on_copy{};
    D3D12_CPU_DESCRIPTOR_HANDLE dsv{};
    D3D12_CPU_DESCRIPTOR_HANDLE dsv_read_only{};
    u32 buffer_size{};
};

class ImageAlloc : public VideoCommon::ImageAllocBase {};

/// Whether block-compressed 2D arrays are decoded on the CPU (the default; boot.cfg
/// "bc_arrays=native" keeps them compressed). Applies to images created from now on.
void SetBcArrayDecode(bool enabled);

/// Whether ASTC images kept as RGBA8 are decoded by the compute shader (the default) rather than
/// the CPU. Applies to images created from now on.
void SetAstcGpuDecode(bool enabled);
/// One-shot diagnostic: compare the first GPU BC3 result with the existing CPU pipeline.
void SetAstcGpuVerify(bool enabled);
/// Diagnostic: wait for the GPU after every GPU ASTC upload (tells races from wrong results).
void SetAstcGpuSync(bool enabled);
/// Diagnostic: new scratch resources for every GPU ASTC upload (no reuse between images).
void SetAstcGpuFresh(bool enabled);

class Sampler {
public:
    Sampler(TextureCacheRuntime& runtime, const Tegra::Texture::TSCEntry& config);
    ~Sampler();
    Sampler(const Sampler&) = delete;
    Sampler& operator=(const Sampler&) = delete;
    Sampler(Sampler&& other) noexcept;
    Sampler& operator=(Sampler&& other) noexcept;

    [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE Handle() const noexcept { return handle; }
    [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE HandleWithDefaultAnisotropy() const noexcept {
        return handle;
    }
    [[nodiscard]] bool HasAddedAnisotropy() const noexcept { return false; }

    /// Never reused by another sampler: SamplerHeap deduplicates tables by these keys.
    [[nodiscard]] u64 Key() const noexcept { return key; }
    /// The D3D12 filter it was created with (draw trace).
    [[nodiscard]] D3D12_FILTER Filter() const noexcept { return filter; }
    /// Everything it was created with, for the draw trace.
    [[nodiscard]] std::string Describe() const;
    /// What spirv_to_dxil's integer texture lowering emulates of this sampler, for a view whose
    /// last mip level (relative to the view) is last_level.
    [[nodiscard]] eden_integer_sampler_state IntegerState(u32 last_level) const;

private:
    TextureCacheRuntime* runtime{};
    D3D12_CPU_DESCRIPTOR_HANDLE handle{};
    u64 key{};
    D3D12_FILTER filter{};
    D3D12_SAMPLER_DESC desc{};
};

class Framebuffer {
public:
    Framebuffer(TextureCacheRuntime&, std::span<ImageView*, NUM_RT> color_buffers,
                ImageView* depth_buffer, const VideoCommon::RenderTargets& key);

    /// RTVs by guest render target index, as the pipeline's RTVFormats: slot i is regs.rt[i]
    /// (like the Vulkan render pass), empty slots hold the null RTV.
    [[nodiscard]] std::span<const D3D12_CPU_DESCRIPTOR_HANDLE> ColorTargets() const noexcept {
        return std::span{colors.data(), num_colors};
    }
    [[nodiscard]] bool HasColor(size_t index) const noexcept {
        return index < NUM_RT && color_images[index] != ImageId{};
    }
    /// Null when there is no depth buffer. read_only: the draw also samples it (see
    /// PrepareAttachments).
    [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE DepthTarget(bool read_only = false) const noexcept {
        return read_only && depth_read_only.ptr ? depth_read_only : depth;
    }
    [[nodiscard]] ImageId DepthImageId() const noexcept { return depth_image; }
    /// The texture cache's image slots (null when the framebuffer has no images).
    [[nodiscard]] SlotVector<Image>* Images() const noexcept { return images; }
    [[nodiscard]] ImageId ColorImageId(size_t index) const noexcept {
        return index < NUM_RT ? color_images[index] : ImageId{};
    }
    [[nodiscard]] bool HasStencil() const noexcept { return has_stencil; }
    /// RTV format of render target index (UNKNOWN when empty) and the DSV format.
    [[nodiscard]] DXGI_FORMAT ColorFormat(size_t index) const noexcept {
        return index < NUM_RT ? color_formats[index] : DXGI_FORMAT_UNKNOWN;
    }
    [[nodiscard]] DXGI_FORMAT DepthFormat() const noexcept { return depth_format; }
    /// Mip level and array layer a render target's view starts at (draw trace).
    [[nodiscard]] VideoCommon::SubresourceBase ColorBase(size_t index) const noexcept {
        return index < NUM_RT ? color_bases[index] : VideoCommon::SubresourceBase{};
    }
    /// Layers a render target's view covers, from ColorBase's layer.
    [[nodiscard]] u32 ColorLayers(size_t index) const noexcept {
        return index < NUM_RT ? color_layers[index] : 0;
    }
    /// Whether a render target's RTV is on the image's reinterpreted copy.
    [[nodiscard]] bool ColorOnCopy(size_t index) const noexcept {
        return index < NUM_RT && (copy_colors >> index & 1U) != 0;
    }
    [[nodiscard]] VideoCommon::SubresourceBase DepthBase() const noexcept { return depth_base; }
    [[nodiscard]] u32 DepthLayers() const noexcept { return depth_layers; }
    /// Texture cache image of a render target / the depth buffer; null when absent (draw trace).
    [[nodiscard]] const Image* ColorImage(size_t index) const noexcept;
    [[nodiscard]] const Image* DepthImage() const noexcept;
    /// Guest render targets bound with a view that has no RTV: draws to them are lost.
    [[nodiscard]] u32 MissingColorMask() const noexcept { return missing_colors; }
    /// Sample count of the attachments.
    [[nodiscard]] u32 Samples() const noexcept { return samples; }
    [[nodiscard]] VideoCommon::Extent2D Extent() const noexcept { return extent; }
    [[nodiscard]] bool IsRescaled() const noexcept { return is_rescaled; }

    /// Transitions the attachments to RENDER_TARGET and DEPTH_WRITE, or the depth buffer to
    /// DEPTH_SAMPLED_STATE when the draw also samples it (bind DepthTarget(true) then).
    void PrepareAttachments(bool depth_sampled = false) const;

private:
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, NUM_RT> colors{};
    std::array<ImageId, NUM_RT> color_images{};
    std::array<DXGI_FORMAT, NUM_RT> color_formats{};
    std::array<VideoCommon::SubresourceBase, NUM_RT> color_bases{};
    std::array<u32, NUM_RT> color_layers{};
    ImageId depth_image{};
    VideoCommon::SubresourceBase depth_base{};
    u32 depth_layers{};
    DXGI_FORMAT depth_format{DXGI_FORMAT_UNKNOWN};
    u32 samples{1};
    SlotVector<Image>* images{};
    D3D12_CPU_DESCRIPTOR_HANDLE depth{};
    D3D12_CPU_DESCRIPTOR_HANDLE depth_read_only{};
    u32 num_colors{};
    u32 missing_colors{};
    u32 copy_colors{}; ///< render targets whose RTV is on the image's reinterpreted copy
    VideoCommon::Extent2D extent{};
    bool has_stencil{};
    bool is_rescaled{};
};

struct TextureCacheParams {
    static constexpr bool ENABLE_VALIDATION = true;
    static constexpr bool FRAMEBUFFER_BLITS = false;
    static constexpr bool HAS_EMULATED_COPIES = false;
    static constexpr bool HAS_DEVICE_MEMORY_INFO = true;
    /// MSAA downloads need the resolve passes of phase 5.
    static constexpr bool HAS_MSAA_DOWNLOADS = false;
    /// Downloads are recorded on the GPU thread (CommitAsyncFlushes) and only read back on the fence
    /// thread; the synchronous mode would record and flush the command list from the fence thread.
    static constexpr bool IMPLEMENTS_ASYNC_DOWNLOADS = true;

    using Runtime = TextureCacheRuntime;
    using Image = D3D12::Image;
    using ImageAlloc = D3D12::ImageAlloc;
    using ImageView = D3D12::ImageView;
    using Sampler = D3D12::Sampler;
    using Framebuffer = D3D12::Framebuffer;
    using AsyncBuffer = StagingBufferRef;
    using BufferType = ID3D12Resource*;
};

using TextureCache = VideoCommon::TextureCache<TextureCacheParams>;

} // namespace D3D12
