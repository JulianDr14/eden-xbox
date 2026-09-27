// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <mutex>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

#include "shader_recompiler/shader_info.h"
#include "video_core/renderer_d3d12/d3d12_descriptor_heap.h"
#include "video_core/renderer_d3d12/d3d12_staging_buffer_pool.h"
#include "video_core/texture_cache/image_view_base.h"
#include "video_core/texture_cache/texture_cache_base.h"

namespace D3D12 {

using Common::SlotVector;
using VideoCommon::ImageId;
using VideoCommon::NUM_RT;
using VideoCommon::Region2D;

class BlitImageHelper;
class Device;
class Scheduler;
class Image;
class ImageView;
class Framebuffer;
class Sampler;

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

    void RunSelfTest();
    void Finish();

    /// Where shader blits go (set by the renderer once the helper exists; null means skip).
    void SetBlitHelper(BlitImageHelper* helper) noexcept { blit_helper = helper; }
    StagingBufferRef UploadStagingBuffer(size_t size, bool deferred = false);
    StagingBufferRef DownloadStagingBuffer(size_t size, bool deferred = false);
    void FreeDeferredStagingBuffer(StagingBufferRef& ref);
    void TickFrame();
    u64 GetDeviceLocalMemory() const;
    u64 GetDeviceMemoryUsage() const;
    bool CanReportMemoryUsage() const { return true; }
    std::optional<size_t> GetSamplerHeapBudget() const { return SamplerHeap::CAPACITY; }

    void CopyImage(Image& dst, Image& src, std::span<const VideoCommon::ImageCopy> copies);
    void CopyImageMSAA(Image& dst, Image& src, std::span<const VideoCommon::ImageCopy> copies);
    bool ShouldReinterpret(Image&, Image&) const noexcept { return false; }
    void ReinterpretImage(Image& dst, Image& src,
                          std::span<const VideoCommon::ImageCopy> copies);
    void ConvertImage(Framebuffer*, ImageView&, ImageView&);
    void BlitImage(Framebuffer*, ImageView&, ImageView&, const Region2D&, const Region2D&,
                   Tegra::Engines::Fermi2D::Filter, Tegra::Engines::Fermi2D::Operation);

    bool CanAccelerateImageUpload(Image&) const noexcept { return false; }
    bool CanUploadMSAA() const noexcept { return false; }
    void AccelerateImageUpload(Image&, const StagingBufferRef&,
                               std::span<const VideoCommon::SwizzleParameters>, u32, u32) {}
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

    /// RTV of no resource, bound in the render target slots a framebuffer leaves empty.
    [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE NullRenderTarget() const noexcept {
        return null_rtv;
    }

private:
    const Device& device;
    Scheduler& scheduler;
    StagingBufferPool& staging;
    CpuDescriptorAllocator& view_descriptors;
    CpuDescriptorAllocator& sampler_descriptors;
    CpuDescriptorAllocator& rtv_descriptors;
    CpuDescriptorAllocator& dsv_descriptors;
    D3D12_CPU_DESCRIPTOR_HANDLE null_rtv{};
    BlitImageHelper* blit_helper{};
    /// MIN/MAX sampler reductions need tiled resources tier 2; the Xbox Series reports tier 1 and
    /// creating such a sampler there removes the device (DXGI_ERROR_INVALID_CALL).
    bool supports_min_max_filter{};
    /// FORMAT_SUPPORT answers by format, each logged once (see SupportsView).
    mutable std::mutex format_support_mutex;
    mutable std::unordered_map<DXGI_FORMAT, D3D12_FEATURE_DATA_FORMAT_SUPPORT> format_support;
};

class Image : public VideoCommon::ImageBase {
public:
    Image(TextureCacheRuntime& runtime, const VideoCommon::ImageInfo& info, GPUVAddr gpu_addr,
          VAddr cpu_addr);
    explicit Image(const VideoCommon::NullImageParams& params);
    /// The resource outlives the Image until the GPU is done with it.
    ~Image();

    Image(const Image&) = delete;
    Image& operator=(const Image&) = delete;
    Image(Image&&) noexcept = default;
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
    [[nodiscard]] u32 Subresource(s32 level, s32 layer, u32 plane = 0) const noexcept;
    void Transition(D3D12_RESOURCE_STATES next);
    bool IsRescaled() const noexcept { return false; }
    bool ScaleUp(bool = false) { return false; }
    bool ScaleDown(bool = false) { return false; }

    u64 allocation_tick{};

private:
    /// Staging layout of one BufferImageCopy (see d3d12_texture_cache.cpp).
    struct CopyLayout;
    [[nodiscard]] CopyLayout Layout(const VideoCommon::BufferImageCopy& copy) const;
    /// False (logged once) when this image's data cannot be transferred yet.
    [[nodiscard]] bool CanTransfer() const;
    /// Hash and transparency of a CPU-decoded upload (first ones only), to compare machines.
    void LogConvertedUpload(const u8* data, const CopyLayout& layout,
                            const VideoCommon::BufferImageCopy& copy) const;

    TextureCacheRuntime* runtime{};
    ComPtr<ID3D12Resource> resource;
    FormatInfo format{};
    DXGI_FORMAT footprint_format{}; ///< format GetCopyableFootprints uses for plane 0
    D3D12_RESOURCE_STATES state{D3D12_RESOURCE_STATE_COMMON};
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
    [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE DepthStencil() const noexcept { return dsv; }
    [[nodiscard]] ID3D12Resource* ImageHandle() const noexcept { return image; }
    [[nodiscard]] GPUVAddr GpuAddr() const noexcept { return gpu_addr; }
    [[nodiscard]] u32 BufferSize() const noexcept { return buffer_size; }
    [[nodiscard]] bool IsRescaled() const noexcept;

    /// The texture cache image of this view; null for null and buffer views.
    [[nodiscard]] Image* SourceImage() const noexcept;

    /// Transitions the whole image this view belongs to (no-op for null and buffer views).
    void TransitionImage(D3D12_RESOURCE_STATES state) const;

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

    [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE CreateSrv(Shader::TextureType texture_type) const;
    void Release();
    TextureCacheRuntime* runtime{};
    SlotVector<Image>* slot_images{};
    ID3D12Resource* image{};
    SrvParams srv_params{};
    Shader::TextureType natural_type{Shader::TextureType::Color2D};
    mutable std::array<D3D12_CPU_DESCRIPTOR_HANDLE, Shader::NUM_TEXTURE_TYPES> srvs{};
    D3D12_CPU_DESCRIPTOR_HANDLE uav{};
    D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
    D3D12_CPU_DESCRIPTOR_HANDLE dsv{};
    u32 buffer_size{};
};

class ImageAlloc : public VideoCommon::ImageAllocBase {};

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

private:
    TextureCacheRuntime* runtime{};
    D3D12_CPU_DESCRIPTOR_HANDLE handle{};
    u64 key{};
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
    /// Null when there is no depth buffer.
    [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE DepthTarget() const noexcept { return depth; }
    [[nodiscard]] bool HasStencil() const noexcept { return has_stencil; }
    /// RTV format of render target index (UNKNOWN when empty) and the DSV format.
    [[nodiscard]] DXGI_FORMAT ColorFormat(size_t index) const noexcept {
        return index < NUM_RT ? color_formats[index] : DXGI_FORMAT_UNKNOWN;
    }
    [[nodiscard]] DXGI_FORMAT DepthFormat() const noexcept { return depth_format; }
    /// Texture cache image of a render target / the depth buffer; null when absent (draw trace).
    [[nodiscard]] const Image* ColorImage(size_t index) const noexcept;
    [[nodiscard]] const Image* DepthImage() const noexcept;
    /// Guest render targets bound with a view that has no RTV: draws to them are lost.
    [[nodiscard]] u32 MissingColorMask() const noexcept { return missing_colors; }
    /// Sample count of the attachments.
    [[nodiscard]] u32 Samples() const noexcept { return samples; }
    [[nodiscard]] VideoCommon::Extent2D Extent() const noexcept { return extent; }
    [[nodiscard]] bool IsRescaled() const noexcept { return is_rescaled; }

    /// Transitions the attachments to RENDER_TARGET and DEPTH_WRITE.
    void PrepareAttachments() const;

private:
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, NUM_RT> colors{};
    std::array<ImageId, NUM_RT> color_images{};
    std::array<DXGI_FORMAT, NUM_RT> color_formats{};
    ImageId depth_image{};
    DXGI_FORMAT depth_format{DXGI_FORMAT_UNKNOWN};
    u32 samples{1};
    SlotVector<Image>* images{};
    D3D12_CPU_DESCRIPTOR_HANDLE depth{};
    u32 num_colors{};
    u32 missing_colors{};
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
