// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <optional>
#include <span>
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

private:
    const Device& device;
    Scheduler& scheduler;
    StagingBufferPool& staging;
    CpuDescriptorAllocator& view_descriptors;
    CpuDescriptorAllocator& sampler_descriptors;
    CpuDescriptorAllocator& rtv_descriptors;
    CpuDescriptorAllocator& dsv_descriptors;
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

    TextureCacheRuntime* runtime{};
    ComPtr<ID3D12Resource> resource;
    FormatInfo format{};
    DXGI_FORMAT footprint_format{}; ///< format GetCopyableFootprints uses for plane 0
    D3D12_RESOURCE_STATES state{D3D12_RESOURCE_STATE_COMMON};
};

class ImageView : public VideoCommon::ImageViewBase {
public:
    ImageView(TextureCacheRuntime& runtime, const VideoCommon::ImageViewInfo& info, ImageId image_id,
              Image& image);
    ImageView(TextureCacheRuntime& runtime, const VideoCommon::ImageViewInfo& info, ImageId image_id,
              Image& image, const SlotVector<Image>& images);
    ImageView(TextureCacheRuntime&, const VideoCommon::ImageInfo&,
              const VideoCommon::ImageViewInfo&, GPUVAddr);
    ImageView(TextureCacheRuntime&, const VideoCommon::NullImageViewParams&);
    ~ImageView();

    ImageView(const ImageView&) = delete;
    ImageView& operator=(const ImageView&) = delete;
    ImageView(ImageView&& other) noexcept;
    ImageView& operator=(ImageView&& other) noexcept;

    [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE Handle(Shader::TextureType) const noexcept {
        return srv;
    }
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

private:
    void Release();
    TextureCacheRuntime* runtime{};
    const SlotVector<Image>* slot_images{};
    ID3D12Resource* image{};
    D3D12_CPU_DESCRIPTOR_HANDLE srv{};
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

private:
    TextureCacheRuntime* runtime{};
    D3D12_CPU_DESCRIPTOR_HANDLE handle{};
};

class Framebuffer {
public:
    Framebuffer(TextureCacheRuntime&, std::span<ImageView*, NUM_RT> color_buffers,
                ImageView* depth_buffer, const VideoCommon::RenderTargets& key);

    [[nodiscard]] std::span<const D3D12_CPU_DESCRIPTOR_HANDLE> ColorTargets() const noexcept {
        return std::span{colors.data(), num_colors};
    }
    [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE DepthTarget() const noexcept { return depth; }
    [[nodiscard]] VideoCommon::Extent2D Extent() const noexcept { return extent; }
    [[nodiscard]] bool IsRescaled() const noexcept { return is_rescaled; }

private:
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, NUM_RT> colors{};
    D3D12_CPU_DESCRIPTOR_HANDLE depth{};
    u32 num_colors{};
    VideoCommon::Extent2D extent{};
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
