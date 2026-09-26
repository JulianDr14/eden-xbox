// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video_core/renderer_d3d12/d3d12_texture_cache.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>
#include <utility>

#include <fmt/format.h>

#include "common/alignment.h"
#include "common/div_ceil.h"
#include "common/logging.h"
#include "common/settings.h"
#include "video_core/renderer_d3d12/d3d12_device.h"
#include "video_core/renderer_d3d12/d3d12_scheduler.h"
#include "video_core/surface.h"
#include "video_core/texture_cache/render_targets.h"
#include "video_core/texture_cache/samples_helper.h"
#include "video_core/texture_cache/texture_cache.h"
#include "video_core/texture_cache/util.h"

namespace D3D12 {
namespace {

using PixelFormat = VideoCore::Surface::PixelFormat;
using SurfaceType = VideoCore::Surface::SurfaceType;
using VideoCommon::BufferImageCopy;
using VideoCommon::ImageCopy;
using VideoCommon::ImageType;
using VideoCommon::ImageViewType;

constexpr u32 AlignPitch(u32 value) {
    return (value + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) &
           ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
}

FormatInfo BaseFormat(PixelFormat format) {
    using enum PixelFormat;
    switch (format) {
    case A8B8G8R8_UNORM: return {DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM};
    case A8B8G8R8_SRGB: return {DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB};
    case A8B8G8R8_SNORM: return {DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_SNORM, DXGI_FORMAT_R8G8B8A8_SNORM};
    case A8B8G8R8_SINT: return {DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_SINT, DXGI_FORMAT_R8G8B8A8_SINT};
    case A8B8G8R8_UINT: return {DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UINT, DXGI_FORMAT_R8G8B8A8_UINT};
    case B8G8R8A8_UNORM: return {DXGI_FORMAT_B8G8R8A8_TYPELESS, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM};
    case B8G8R8A8_SRGB: return {DXGI_FORMAT_B8G8R8A8_TYPELESS, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB};
    case R5G6B5_UNORM: case B5G6R5_UNORM: return {DXGI_FORMAT_B5G6R5_UNORM, DXGI_FORMAT_B5G6R5_UNORM, DXGI_FORMAT_B5G6R5_UNORM};
    case A1R5G5B5_UNORM: case A1B5G5R5_UNORM: return {DXGI_FORMAT_B5G5R5A1_UNORM, DXGI_FORMAT_B5G5R5A1_UNORM, DXGI_FORMAT_B5G5R5A1_UNORM};
    case A4B4G4R4_UNORM: return {DXGI_FORMAT_B4G4R4A4_UNORM, DXGI_FORMAT_B4G4R4A4_UNORM, DXGI_FORMAT_B4G4R4A4_UNORM};
    case A2B10G10R10_UNORM: case A2R10G10B10_UNORM: return {DXGI_FORMAT_R10G10B10A2_TYPELESS, DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM};
    case A2B10G10R10_UINT: return {DXGI_FORMAT_R10G10B10A2_TYPELESS, DXGI_FORMAT_R10G10B10A2_UINT, DXGI_FORMAT_R10G10B10A2_UINT};
    case R8_UNORM: return {DXGI_FORMAT_R8_TYPELESS, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8_UNORM};
    case R8_SNORM: return {DXGI_FORMAT_R8_TYPELESS, DXGI_FORMAT_R8_SNORM, DXGI_FORMAT_R8_SNORM};
    case R8_SINT: return {DXGI_FORMAT_R8_TYPELESS, DXGI_FORMAT_R8_SINT, DXGI_FORMAT_R8_SINT};
    case R8_UINT: case S8_UINT: return {DXGI_FORMAT_R8_TYPELESS, DXGI_FORMAT_R8_UINT, DXGI_FORMAT_R8_UINT};
    case R8G8_UNORM: return {DXGI_FORMAT_R8G8_TYPELESS, DXGI_FORMAT_R8G8_UNORM, DXGI_FORMAT_R8G8_UNORM};
    case R8G8_SNORM: return {DXGI_FORMAT_R8G8_TYPELESS, DXGI_FORMAT_R8G8_SNORM, DXGI_FORMAT_R8G8_SNORM};
    case R8G8_SINT: return {DXGI_FORMAT_R8G8_TYPELESS, DXGI_FORMAT_R8G8_SINT, DXGI_FORMAT_R8G8_SINT};
    case R8G8_UINT: return {DXGI_FORMAT_R8G8_TYPELESS, DXGI_FORMAT_R8G8_UINT, DXGI_FORMAT_R8G8_UINT};
    case R16_FLOAT: return {DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_R16_FLOAT, DXGI_FORMAT_R16_FLOAT};
    case R16_UNORM: return {DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16_UNORM};
    case R16_SNORM: return {DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_R16_SNORM, DXGI_FORMAT_R16_SNORM};
    case R16_UINT: return {DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_R16_UINT, DXGI_FORMAT_R16_UINT};
    case R16_SINT: return {DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_R16_SINT, DXGI_FORMAT_R16_SINT};
    case R16G16_FLOAT: return {DXGI_FORMAT_R16G16_TYPELESS, DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R16G16_FLOAT};
    case R16G16_UNORM: return {DXGI_FORMAT_R16G16_TYPELESS, DXGI_FORMAT_R16G16_UNORM, DXGI_FORMAT_R16G16_UNORM};
    case R16G16_SNORM: return {DXGI_FORMAT_R16G16_TYPELESS, DXGI_FORMAT_R16G16_SNORM, DXGI_FORMAT_R16G16_SNORM};
    case R16G16_UINT: return {DXGI_FORMAT_R16G16_TYPELESS, DXGI_FORMAT_R16G16_UINT, DXGI_FORMAT_R16G16_UINT};
    case R16G16_SINT: return {DXGI_FORMAT_R16G16_TYPELESS, DXGI_FORMAT_R16G16_SINT, DXGI_FORMAT_R16G16_SINT};
    case R16G16B16A16_FLOAT: case R16G16B16X16_FLOAT: return {DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT};
    case R16G16B16A16_UNORM: return {DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_UNORM, DXGI_FORMAT_R16G16B16A16_UNORM};
    case R16G16B16A16_SNORM: return {DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_SNORM, DXGI_FORMAT_R16G16B16A16_SNORM};
    case R16G16B16A16_SINT: return {DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_SINT, DXGI_FORMAT_R16G16B16A16_SINT};
    case R16G16B16A16_UINT: return {DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_UINT, DXGI_FORMAT_R16G16B16A16_UINT};
    case R32_FLOAT: return {DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT};
    case R32_UINT: return {DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_R32_UINT, DXGI_FORMAT_R32_UINT};
    case R32_SINT: return {DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_R32_SINT, DXGI_FORMAT_R32_SINT};
    case R32G32_FLOAT: return {DXGI_FORMAT_R32G32_TYPELESS, DXGI_FORMAT_R32G32_FLOAT, DXGI_FORMAT_R32G32_FLOAT};
    case R32G32_UINT: return {DXGI_FORMAT_R32G32_TYPELESS, DXGI_FORMAT_R32G32_UINT, DXGI_FORMAT_R32G32_UINT};
    case R32G32_SINT: return {DXGI_FORMAT_R32G32_TYPELESS, DXGI_FORMAT_R32G32_SINT, DXGI_FORMAT_R32G32_SINT};
    case R32G32B32_FLOAT: return {DXGI_FORMAT_R32G32B32_TYPELESS, DXGI_FORMAT_R32G32B32_FLOAT, DXGI_FORMAT_R32G32B32_FLOAT};
    case R32G32B32A32_FLOAT: return {DXGI_FORMAT_R32G32B32A32_TYPELESS, DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT};
    case R32G32B32A32_UINT: return {DXGI_FORMAT_R32G32B32A32_TYPELESS, DXGI_FORMAT_R32G32B32A32_UINT, DXGI_FORMAT_R32G32B32A32_UINT};
    case R32G32B32A32_SINT: return {DXGI_FORMAT_R32G32B32A32_TYPELESS, DXGI_FORMAT_R32G32B32A32_SINT, DXGI_FORMAT_R32G32B32A32_SINT};
    case B10G11R11_FLOAT: return {DXGI_FORMAT_R11G11B10_FLOAT, DXGI_FORMAT_R11G11B10_FLOAT, DXGI_FORMAT_R11G11B10_FLOAT};
    case E5B9G9R9_FLOAT: return {DXGI_FORMAT_R9G9B9E5_SHAREDEXP, DXGI_FORMAT_R9G9B9E5_SHAREDEXP, DXGI_FORMAT_R9G9B9E5_SHAREDEXP};
    case BC1_RGBA_UNORM: return {DXGI_FORMAT_BC1_TYPELESS, DXGI_FORMAT_BC1_UNORM, DXGI_FORMAT_BC1_UNORM};
    case BC1_RGBA_SRGB: return {DXGI_FORMAT_BC1_TYPELESS, DXGI_FORMAT_BC1_UNORM_SRGB, DXGI_FORMAT_BC1_UNORM_SRGB};
    case BC2_UNORM: return {DXGI_FORMAT_BC2_TYPELESS, DXGI_FORMAT_BC2_UNORM, DXGI_FORMAT_BC2_UNORM};
    case BC2_SRGB: return {DXGI_FORMAT_BC2_TYPELESS, DXGI_FORMAT_BC2_UNORM_SRGB, DXGI_FORMAT_BC2_UNORM_SRGB};
    case BC3_UNORM: return {DXGI_FORMAT_BC3_TYPELESS, DXGI_FORMAT_BC3_UNORM, DXGI_FORMAT_BC3_UNORM};
    case BC3_SRGB: return {DXGI_FORMAT_BC3_TYPELESS, DXGI_FORMAT_BC3_UNORM_SRGB, DXGI_FORMAT_BC3_UNORM_SRGB};
    case BC4_UNORM: return {DXGI_FORMAT_BC4_TYPELESS, DXGI_FORMAT_BC4_UNORM, DXGI_FORMAT_BC4_UNORM};
    case BC4_SNORM: return {DXGI_FORMAT_BC4_TYPELESS, DXGI_FORMAT_BC4_SNORM, DXGI_FORMAT_BC4_SNORM};
    case BC5_UNORM: return {DXGI_FORMAT_BC5_TYPELESS, DXGI_FORMAT_BC5_UNORM, DXGI_FORMAT_BC5_UNORM};
    case BC5_SNORM: return {DXGI_FORMAT_BC5_TYPELESS, DXGI_FORMAT_BC5_SNORM, DXGI_FORMAT_BC5_SNORM};
    case BC6H_UFLOAT: return {DXGI_FORMAT_BC6H_TYPELESS, DXGI_FORMAT_BC6H_UF16, DXGI_FORMAT_BC6H_UF16};
    case BC6H_SFLOAT: return {DXGI_FORMAT_BC6H_TYPELESS, DXGI_FORMAT_BC6H_SF16, DXGI_FORMAT_BC6H_SF16};
    case BC7_UNORM: return {DXGI_FORMAT_BC7_TYPELESS, DXGI_FORMAT_BC7_UNORM, DXGI_FORMAT_BC7_UNORM};
    case BC7_SRGB: return {DXGI_FORMAT_BC7_TYPELESS, DXGI_FORMAT_BC7_UNORM_SRGB, DXGI_FORMAT_BC7_UNORM_SRGB};
    case D16_UNORM: return {DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_D16_UNORM, DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_D16_UNORM};
    case D32_FLOAT: return {DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_D32_FLOAT};
    case X8_D24_UNORM: case D24_UNORM_S8_UINT: case S8_UINT_D24_UNORM:
        return {DXGI_FORMAT_R24G8_TYPELESS, DXGI_FORMAT_D24_UNORM_S8_UINT, DXGI_FORMAT_R24_UNORM_X8_TYPELESS, DXGI_FORMAT_D24_UNORM_S8_UINT, DXGI_FORMAT_X24_TYPELESS_G8_UINT};
    case D32_FLOAT_S8_UINT:
        return {DXGI_FORMAT_R32G8X24_TYPELESS, DXGI_FORMAT_D32_FLOAT_S8X24_UINT, DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS, DXGI_FORMAT_D32_FLOAT_S8X24_UINT, DXGI_FORMAT_X32_TYPELESS_G8X24_UINT};
    default:
        return {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN};
    }
}

FormatInfo NativeFormat(PixelFormat format) {
    FormatInfo info = BaseFormat(format);
    info.copy_format = format;
    if (info.resource != DXGI_FORMAT_UNKNOWN) {
        return info;
    }
    if (VideoCore::Surface::IsPixelFormatASTC(format)) {
        // No ASTC in D3D12: the CPU decodes it (and optionally re-encodes to BC1/BC3) on upload,
        // exactly like the generic ConvertImage path expects.
        const bool srgb = VideoCore::Surface::IsPixelFormatSRGB(format);
        info.converted = true;
        switch (Settings::values.astc_recompression.GetValue()) {
        case Settings::AstcRecompression::Bc1:
            info.resource = DXGI_FORMAT_BC1_TYPELESS;
            info.view = srgb ? DXGI_FORMAT_BC1_UNORM_SRGB : DXGI_FORMAT_BC1_UNORM;
            info.copy_format = PixelFormat::BC1_RGBA_UNORM;
            break;
        case Settings::AstcRecompression::Bc3:
            info.resource = DXGI_FORMAT_BC3_TYPELESS;
            info.view = srgb ? DXGI_FORMAT_BC3_UNORM_SRGB : DXGI_FORMAT_BC3_UNORM;
            info.copy_format = PixelFormat::BC3_UNORM;
            break;
        default:
            info.resource = DXGI_FORMAT_R8G8B8A8_TYPELESS;
            info.view = srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
            info.copy_format = PixelFormat::A8B8G8R8_UNORM;
            break;
        }
        info.srv = info.view;
        return info;
    }
    // Formats without a DXGI mapping yet: keep an image so the cache works, never transfer data
    // into it (its bytes would not match the resource layout).
    info.resource = DXGI_FORMAT_R8G8B8A8_TYPELESS;
    info.view = DXGI_FORMAT_R8G8B8A8_UNORM;
    info.srv = DXGI_FORMAT_R8G8B8A8_UNORM;
    info.supported = false;
    return info;
}

D3D12_RESOURCE_DIMENSION Dimension(ImageType type) {
    switch (type) {
    case ImageType::e1D: return D3D12_RESOURCE_DIMENSION_TEXTURE1D;
    case ImageType::e3D: return D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    default: return D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    }
}

D3D12_TEXTURE_ADDRESS_MODE AddressMode(Tegra::Texture::WrapMode mode) {
    switch (mode) {
    case Tegra::Texture::WrapMode::Wrap: return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    case Tegra::Texture::WrapMode::Mirror: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
    case Tegra::Texture::WrapMode::Border:
    case Tegra::Texture::WrapMode::MirrorOnceBorder: return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    case Tegra::Texture::WrapMode::MirrorOnceClampToEdge:
    case Tegra::Texture::WrapMode::MirrorOnceClampOGL: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE;
    default: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    }
}

D3D12_COMPARISON_FUNC Compare(Tegra::Texture::DepthCompareFunc func) {
    return static_cast<D3D12_COMPARISON_FUNC>(static_cast<u32>(func) + 1);
}

u32 Component(Tegra::Texture::SwizzleSource source) {
    using enum Tegra::Texture::SwizzleSource;
    switch (source) {
    case R: return D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0;
    case G: return D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_1;
    case B: return D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_2;
    case A: return D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_3;
    case Zero: return D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0;
    case OneFloat: case OneInt: return D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1;
    }
    return D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0;
}

ComPtr<ID3D12Resource> CreateTransferBuffer(ID3D12Device* device, u64 size) {
    const D3D12_HEAP_PROPERTIES heap{.Type = D3D12_HEAP_TYPE_DEFAULT};
    const D3D12_RESOURCE_DESC desc{.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER, .Alignment = 0,
        .Width = size, .Height = 1, .DepthOrArraySize = 1, .MipLevels = 1,
        .Format = DXGI_FORMAT_UNKNOWN, .SampleDesc = {.Count = 1, .Quality = 0},
        .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR, .Flags = D3D12_RESOURCE_FLAG_NONE};
    // Buffers start in COMMON and are promoted on first use.
    ComPtr<ID3D12Resource> buffer;
    ThrowIfFailed(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                  D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&buffer)),
                  "Create texture transfer buffer");
    return buffer;
}

void TransitionBuffer(ID3D12GraphicsCommandList* commands, ID3D12Resource* buffer,
                      D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    const D3D12_RESOURCE_BARRIER barrier{.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
        .Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE,
        .Transition = {.pResource = buffer, .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                       .StateBefore = before, .StateAfter = after}};
    commands->ResourceBarrier(1, &barrier);
}

D3D12_HEAP_TYPE HeapType(ID3D12Resource* resource) {
    D3D12_HEAP_PROPERTIES properties{};
    D3D12_HEAP_FLAGS flags{};
    if (FAILED(resource->GetHeapProperties(&properties, &flags))) {
        return D3D12_HEAP_TYPE_DEFAULT;
    }
    return properties.Type;
}

/// DEFAULT-heap buffers (the buffer cache's) are implicitly promoted by copies; hand them back in
/// COMMON, the state the buffer cache assumes, so its next promotion is legal. UPLOAD/READBACK
/// buffers can never change state.
void DecayIfDefault(ID3D12GraphicsCommandList* commands, ID3D12Resource* buffer,
                    D3D12_RESOURCE_STATES promoted) {
    if (HeapType(buffer) == D3D12_HEAP_TYPE_DEFAULT) {
        TransitionBuffer(commands, buffer, promoted, D3D12_RESOURCE_STATE_COMMON);
    }
}

template <typename... Args>
void WarnOnce(bool& logged, fmt::format_string<Args...> format, Args&&... args) {
    if (!logged) {
        LOG_WARNING(Render, "D3D12: {}", fmt::format(format, std::forward<Args>(args)...));
        logged = true;
    }
}

bool logged_unsupported_transfer = false;
bool logged_depth_stencil_transfer = false;
bool logged_self_copy = false;
bool logged_view_format = false;

} // namespace

/// How one BufferImageCopy is laid out in staging memory (tightly packed by the generic cache) and
/// in the placed footprint D3D12 copies through (rows padded to 256 bytes).
struct Image::CopyLayout {
    u32 row_bytes;    ///< tight bytes per row of blocks
    u32 row_pitch;    ///< footprint row pitch, aligned to D3D12_TEXTURE_DATA_PITCH_ALIGNMENT
    u32 rows;         ///< rows of blocks copied per slice (the footprint's height in blocks)
    u32 depth;        ///< slices per layer (3D), 1 otherwise
    u32 width;        ///< copy width in texels, aligned up to the block width
    u32 height;       ///< copy height in texels, aligned up to the block height
    u64 tight_slice;  ///< bytes per slice in staging (buffer_image_height rows)
    u64 padded_slice; ///< bytes per slice in the footprint (rows * row_pitch)

    /// True when the staging bytes already form a valid footprint at that offset.
    [[nodiscard]] bool IsFootprint(u64 offset) const {
        return row_bytes == row_pitch && tight_slice == padded_slice &&
               offset % D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT == 0;
    }
};

TextureCacheRuntime::TextureCacheRuntime(const Device& device_, Scheduler& scheduler_,
                                         StagingBufferPool& staging_,
                                         CpuDescriptorAllocator& views,
                                         CpuDescriptorAllocator& samplers,
                                         CpuDescriptorAllocator& rtvs,
                                         CpuDescriptorAllocator& dsvs)
    : device{device_}, scheduler{scheduler_}, staging{staging_}, view_descriptors{views},
      sampler_descriptors{samplers}, rtv_descriptors{rtvs}, dsv_descriptors{dsvs} {
    LOG_INFO(Render, "D3D12: texture cache runtime ready");
}

FormatInfo TextureCacheRuntime::Format(PixelFormat format) const { return NativeFormat(format); }
void TextureCacheRuntime::Finish() { scheduler.Finish(); }
StagingBufferRef TextureCacheRuntime::UploadStagingBuffer(size_t size, bool deferred) {
    return staging.Request(size, MemoryUsage::Upload, deferred);
}
StagingBufferRef TextureCacheRuntime::DownloadStagingBuffer(size_t size, bool deferred) {
    return staging.Request(size, MemoryUsage::Download, deferred);
}
void TextureCacheRuntime::FreeDeferredStagingBuffer(StagingBufferRef& ref) { staging.FreeDeferred(ref); }
void TextureCacheRuntime::TickFrame() { staging.TickFrame(); }
u64 TextureCacheRuntime::GetDeviceLocalMemory() const { return device.QueryVideoMemory().Budget; }
u64 TextureCacheRuntime::GetDeviceMemoryUsage() const {
    return device.QueryVideoMemory().CurrentUsage;
}

bool TextureCacheRuntime::SupportsView(DXGI_FORMAT format, D3D12_FORMAT_SUPPORT1 support1,
                                       D3D12_FORMAT_SUPPORT2 support2) const {
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{.Format = format};
    if (format == DXGI_FORMAT_UNKNOWN ||
        FAILED(device.Get()->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support,
                                                 sizeof(support)))) {
        return false;
    }
    return (support.Support1 & support1) == support1 && (support.Support2 & support2) == support2;
}

Image::Image(TextureCacheRuntime& runtime_, const VideoCommon::ImageInfo& info_, GPUVAddr gpu_addr_,
             VAddr cpu_addr_)
    : VideoCommon::ImageBase{info_, gpu_addr_, cpu_addr_}, runtime{&runtime_},
      format{runtime_.Format(info_.format)} {
    if (format.converted) {
        flags |= VideoCommon::ImageFlagBits::Converted | VideoCommon::ImageFlagBits::CostlyLoad;
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
    if (is_color && !is_msaa &&
        runtime->SupportsView(format.view, D3D12_FORMAT_SUPPORT1_NONE,
                              D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE)) {
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
    const D3D12_HEAP_PROPERTIES heap{.Type = D3D12_HEAP_TYPE_DEFAULT};
    ThrowIfFailed(runtime->device.Get()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                  D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&resource)),
                  "Create texture cache image");
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    runtime->device.Get()->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr,
                                                 nullptr);
    footprint_format = footprint.Footprint.Format;
}

Image::Image(const VideoCommon::NullImageParams& params) : VideoCommon::ImageBase{params} {}

Image::~Image() {
    if (runtime && resource) {
        runtime->scheduler.DeferRelease(std::move(resource));
    }
}

Image& Image::operator=(Image&& other) noexcept {
    if (this != &other) {
        if (runtime && resource) {
            runtime->scheduler.DeferRelease(std::move(resource));
        }
        static_cast<VideoCommon::ImageBase&>(*this) = std::move(other);
        allocation_tick = other.allocation_tick;
        runtime = other.runtime;
        resource = std::move(other.resource);
        format = other.format;
        footprint_format = other.footprint_format;
        state = other.state;
    }
    return *this;
}

u32 Image::Subresource(s32 level, s32 layer, u32 plane) const noexcept {
    const u32 levels = static_cast<u32>(info.resources.levels);
    const u32 layers = info.type == ImageType::e3D ? 1U : static_cast<u32>(info.resources.layers);
    return static_cast<u32>(level) + static_cast<u32>(layer) * levels + plane * levels * layers;
}

void Image::Transition(D3D12_RESOURCE_STATES next) {
    if (!resource || state == next) return;
    const D3D12_RESOURCE_BARRIER barrier{.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
        .Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE,
        .Transition = {.pResource = resource.Get(), .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                       .StateBefore = state, .StateAfter = next}};
    runtime->scheduler.CommandList()->ResourceBarrier(1, &barrier);
    state = next;
}

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

bool Image::CanTransfer() const {
    if (!resource) {
        return false;
    }
    if (!format.supported) {
        WarnOnce(logged_unsupported_transfer, "texture format {} has no DXGI mapping yet; its "
                 "contents are not transferred", info.format);
        return false;
    }
    if (format.stencil_srv != DXGI_FORMAT_UNKNOWN) {
        // D3D12 stores depth and stencil in separate planes; the guest packs them together.
        WarnOnce(logged_depth_stencil_transfer, "depth-stencil ({}) transfers need plane "
                 "splitting (phase 5); contents are not transferred", info.format);
        return false;
    }
    return true;
}

void Image::UploadMemory(ID3D12Resource* buffer, size_t base_offset,
                         std::span<const BufferImageCopy> copies) {
    if (!CanTransfer() || copies.empty()) {
        return;
    }
    Transition(D3D12_RESOURCE_STATE_COPY_DEST);
    auto* const commands = runtime->scheduler.CommandList();
    const bool cpu_visible = HeapType(buffer) == D3D12_HEAP_TYPE_UPLOAD;
    u8* source_base = nullptr;
    if (cpu_visible) {
        // Staging buffers are persistently mapped; Map again just to learn the base address.
        void* mapped{};
        const D3D12_RANGE no_read{0, 0};
        ThrowIfFailed(buffer->Map(0, &no_read, &mapped), "Map (texture upload source)");
        source_base = static_cast<u8*>(mapped);
    }
    bool promoted_source = false;
    for (const auto& copy : copies) {
        const CopyLayout layout = Layout(copy);
        const u32 layers = static_cast<u32>(std::max(1, copy.image_subresource.num_layers));
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
            } else if (cpu_visible) {
                // Repack on the CPU into a pitch-aligned staging allocation.
                const StagingBufferRef packed = runtime->UploadStagingBuffer(
                    static_cast<size_t>(layout.padded_slice * layout.depth));
                for (u32 z = 0; z < layout.depth; ++z) {
                    for (u32 row = 0; row < layout.rows; ++row) {
                        std::memcpy(packed.mapped_span.data() + z * layout.padded_slice +
                                        static_cast<u64>(row) * layout.row_pitch,
                                    source_base + source_offset + z * layout.tight_slice +
                                        static_cast<u64>(row) * layout.row_bytes,
                                    layout.row_bytes);
                    }
                }
                src.pResource = packed.buffer;
                src.PlacedFootprint.Offset = packed.offset;
            } else {
                // GPU-only source (DMA from the buffer cache): repack row by row on the GPU.
                transfer = CreateTransferBuffer(runtime->device.Get(),
                                                layout.padded_slice * layout.depth);
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
            commands->CopyTextureRegion(&dst, copy.image_offset.x, copy.image_offset.y,
                                        copy.image_offset.z, &src, nullptr);
            if (transfer) {
                runtime->scheduler.DeferRelease(std::move(transfer));
            }
        }
    }
    if (cpu_visible) {
        const D3D12_RANGE no_write{0, 0};
        buffer->Unmap(0, &no_write);
    } else if (promoted_source) {
        DecayIfDefault(commands, buffer, D3D12_RESOURCE_STATE_COPY_SOURCE);
    }
}

void Image::UploadMemory(const StagingBufferRef& map, std::span<const BufferImageCopy> copies) {
    UploadMemory(map.buffer, map.offset, copies);
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
                ComPtr<ID3D12Resource> transfer = CreateTransferBuffer(
                    runtime->device.Get(), layout.padded_slice * layout.depth);
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
                runtime->scheduler.DeferRelease(std::move(transfer));
            }
        }
        if (promoted) {
            DecayIfDefault(commands, buffer, D3D12_RESOURCE_STATE_COPY_DEST);
        }
    }
}

void Image::DownloadMemory(const StagingBufferRef& map, std::span<const BufferImageCopy> copies) {
    DownloadMemory(map.buffer, map.offset, copies);
}

void TextureCacheRuntime::CopyImage(Image& dst, Image& src, std::span<const ImageCopy> copies) {
    if (&dst == &src) {
        // Whole-resource state tracking cannot hold COPY_SOURCE and COPY_DEST at once.
        WarnOnce(logged_self_copy, "copies within one image are not supported yet; skipped");
        return;
    }
    if (!src.Handle() || !dst.Handle()) {
        return;
    }
    src.Transition(D3D12_RESOURCE_STATE_COPY_SOURCE);
    dst.Transition(D3D12_RESOURCE_STATE_COPY_DEST);
    const u32 block_w = VideoCore::Surface::DefaultBlockWidth(src.info.format);
    const u32 block_h = VideoCore::Surface::DefaultBlockHeight(src.info.format);
    for (const auto& copy : copies) {
        const u32 depth = src.info.type == ImageType::e3D ? copy.extent.depth : 1U;
        for (s32 layer = 0; layer < copy.src_subresource.num_layers; ++layer) {
            D3D12_TEXTURE_COPY_LOCATION source{.pResource = src.Handle(), .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
                .SubresourceIndex = src.Subresource(copy.src_subresource.base_level, copy.src_subresource.base_layer + layer)};
            D3D12_TEXTURE_COPY_LOCATION target{.pResource = dst.Handle(), .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
                .SubresourceIndex = dst.Subresource(copy.dst_subresource.base_level, copy.dst_subresource.base_layer + layer)};
            // Boxes on block-compressed images must cover whole blocks.
            const D3D12_BOX box{static_cast<u32>(copy.src_offset.x), static_cast<u32>(copy.src_offset.y), static_cast<u32>(copy.src_offset.z),
                static_cast<u32>(copy.src_offset.x) + Common::AlignUp(copy.extent.width, block_w),
                static_cast<u32>(copy.src_offset.y) + Common::AlignUp(copy.extent.height, block_h),
                static_cast<u32>(copy.src_offset.z) + depth};
            scheduler.CommandList()->CopyTextureRegion(&target, copy.dst_offset.x, copy.dst_offset.y,
                                                       copy.dst_offset.z, &source, &box);
        }
    }
}
void TextureCacheRuntime::CopyImageMSAA(Image&, Image&, std::span<const ImageCopy>) {
    LOG_WARNING(Render, "D3D12: MSAA texture copies are deferred to phase 5");
}
void TextureCacheRuntime::ReinterpretImage(Image& dst, Image& src, std::span<const ImageCopy> copies) {
    if (VideoCore::Surface::BytesPerBlock(dst.info.format) == VideoCore::Surface::BytesPerBlock(src.info.format))
        CopyImage(dst, src, copies);
    else LOG_WARNING(Render, "D3D12: incompatible texture reinterpretation skipped ({} -> {})", src.info.format, dst.info.format);
}
void TextureCacheRuntime::ConvertImage(Framebuffer*, ImageView&, ImageView&) {
    LOG_WARNING(Render, "D3D12: shader texture conversion is deferred to phase 5");
}
void TextureCacheRuntime::BlitImage(Framebuffer*, ImageView&, ImageView&, const Region2D&,
                                    const Region2D&, Tegra::Engines::Fermi2D::Filter,
                                    Tegra::Engines::Fermi2D::Operation) {
    LOG_WARNING(Render, "D3D12: filtered texture blit is deferred to phase 4");
}
void TextureCacheRuntime::TransitionImageLayout(Image& image) {
    image.Transition(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}

ImageView::ImageView(TextureCacheRuntime& runtime_, const VideoCommon::ImageViewInfo& info,
                     ImageId image_id_, Image& source)
    : VideoCommon::ImageViewBase{info, source.info, image_id_, source.gpu_addr}, runtime{&runtime_},
      image{source.Handle()} {
    if (!image) {
        return;
    }
    const FormatInfo format_info = runtime->Format(info.format);
    const auto swizzle = info.Swizzle();
    const u32 base_level = static_cast<u32>(info.range.base.level);
    const u32 base_layer = static_cast<u32>(info.range.base.layer);
    const u32 levels = info.range.extent.levels;
    const u32 layers = info.range.extent.layers;
    const bool is_msaa = source.info.num_samples > 1;

    // SRV: the dimension must match the shader's declaration, so it follows the view type.
    // TODO(phase 4): one SRV per compatible Shader::TextureType, as the Vulkan backend does.
    srv = runtime->view_descriptors.Allocate();
    D3D12_SHADER_RESOURCE_VIEW_DESC desc{.Format = format_info.srv,
        .Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(Component(swizzle[0]), Component(swizzle[1]), Component(swizzle[2]), Component(swizzle[3]))};
    switch (info.type) {
    case ImageViewType::e1D: desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1D; desc.Texture1D = {base_level, levels, 0.0f}; break;
    case ImageViewType::e1DArray: desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1DARRAY; desc.Texture1DArray = {base_level, levels, base_layer, layers, 0.0f}; break;
    case ImageViewType::e3D: desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D; desc.Texture3D = {base_level, levels, 0.0f}; break;
    case ImageViewType::Cube: desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE; desc.TextureCube = {base_level, levels, 0.0f}; break;
    case ImageViewType::CubeArray: desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY; desc.TextureCubeArray = {base_level, levels, base_layer, layers / 6, 0.0f}; break;
    case ImageViewType::e2DArray:
        if (is_msaa) { desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY; desc.Texture2DMSArray = {base_layer, layers}; }
        else { desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY; desc.Texture2DArray = {base_level, levels, base_layer, layers, 0, 0.0f}; }
        break;
    default: // e2D, Rect
        if (is_msaa) {
            desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY; desc.Texture2DMSArray = {base_layer, 1};
        } else if (base_layer == 0 && source.info.resources.layers <= 1) {
            desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; desc.Texture2D = {base_level, levels, 0, 0.0f};
        } else {
            desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY; desc.Texture2DArray = {base_level, levels, base_layer, 1, 0, 0.0f};
        }
        break;
    }
    runtime->device.Get()->CreateShaderResourceView(image, &desc, srv);

    const SurfaceType surface = VideoCore::Surface::GetFormatType(info.format);
    const D3D12_RESOURCE_DESC resource_desc = image->GetDesc();
    const bool is_3d = resource_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    if (surface == SurfaceType::ColorTexture &&
        (resource_desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)) {
        if (runtime->SupportsView(format_info.view, D3D12_FORMAT_SUPPORT1_RENDER_TARGET)) {
            rtv = runtime->rtv_descriptors.Allocate();
            D3D12_RENDER_TARGET_VIEW_DESC rtv_desc{.Format = format_info.view};
            if (is_3d) {
                rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE3D;
                rtv_desc.Texture3D = {base_level, 0, static_cast<u32>(-1)};
            } else if (is_msaa) {
                rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY;
                rtv_desc.Texture2DMSArray = {base_layer, layers};
            } else {
                rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
                rtv_desc.Texture2DArray = {base_level, base_layer, layers, 0};
            }
            runtime->device.Get()->CreateRenderTargetView(image, &rtv_desc, rtv);
        } else {
            WarnOnce(logged_view_format, "view format {} cannot be a render target", info.format);
        }
    }
    if (surface != SurfaceType::ColorTexture &&
        (resource_desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) &&
        format_info.dsv != DXGI_FORMAT_UNKNOWN) {
        dsv = runtime->dsv_descriptors.Allocate();
        D3D12_DEPTH_STENCIL_VIEW_DESC dsv_desc{.Format = format_info.dsv, .Flags = D3D12_DSV_FLAG_NONE};
        if (resource_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D) {
            dsv_desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE1DARRAY;
            dsv_desc.Texture1DArray = {base_level, base_layer, layers};
        } else if (is_msaa) {
            dsv_desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY;
            dsv_desc.Texture2DMSArray = {base_layer, layers};
        } else {
            dsv_desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
            dsv_desc.Texture2DArray = {base_level, base_layer, layers};
        }
        runtime->device.Get()->CreateDepthStencilView(image, &dsv_desc, dsv);
    }
    if ((resource_desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) &&
        runtime->SupportsView(format_info.view, D3D12_FORMAT_SUPPORT1_NONE,
                              D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE)) {
        uav = runtime->view_descriptors.Allocate();
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc{.Format = format_info.view};
        if (is_3d) {
            uav_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
            uav_desc.Texture3D = {base_level, 0, static_cast<u32>(-1)};
        } else {
            uav_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
            uav_desc.Texture2DArray = {base_level, base_layer, layers, 0};
        }
        runtime->device.Get()->CreateUnorderedAccessView(image, nullptr, &uav_desc, uav);
    }
}
ImageView::ImageView(TextureCacheRuntime& runtime, const VideoCommon::ImageViewInfo& info,
                     ImageId id, Image& image_, const SlotVector<Image>& images)
    : ImageView{runtime, info, id, image_} { slot_images = &images; }
ImageView::ImageView(TextureCacheRuntime&, const VideoCommon::ImageInfo& info,
                     const VideoCommon::ImageViewInfo& view, GPUVAddr addr)
    : VideoCommon::ImageViewBase{info, view, addr}, buffer_size{VideoCommon::CalculateGuestSizeInBytes(info)} {}
ImageView::ImageView(TextureCacheRuntime& runtime_, const VideoCommon::NullImageViewParams& params)
    : VideoCommon::ImageViewBase{params}, runtime{&runtime_} {
    srv = runtime->view_descriptors.Allocate();
    const D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc{
        .Format = DXGI_FORMAT_R8_UNORM,
        .ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D,
        .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
        .Texture2D = {.MostDetailedMip = 0, .MipLevels = 1, .PlaneSlice = 0,
                      .ResourceMinLODClamp = 0.0f},
    };
    runtime->device.Get()->CreateShaderResourceView(nullptr, &srv_desc, srv);
    uav = runtime->view_descriptors.Allocate();
    const D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc{
        .Format = DXGI_FORMAT_R8_UNORM,
        .ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D,
        .Texture2D = {.MipSlice = 0, .PlaneSlice = 0},
    };
    runtime->device.Get()->CreateUnorderedAccessView(nullptr, nullptr, &uav_desc, uav);
}
ImageView::~ImageView() { Release(); }
void ImageView::Release() {
    if (!runtime) return;
    if (srv.ptr) runtime->view_descriptors.Free(srv);
    if (uav.ptr) runtime->view_descriptors.Free(uav);
    if (rtv.ptr) runtime->rtv_descriptors.Free(rtv);
    if (dsv.ptr) runtime->dsv_descriptors.Free(dsv);
    runtime = nullptr;
}
ImageView::ImageView(ImageView&& other) noexcept : VideoCommon::ImageViewBase{std::move(other)}, runtime{std::exchange(other.runtime, nullptr)},
    slot_images{other.slot_images}, image{other.image}, srv{other.srv}, uav{other.uav}, rtv{other.rtv}, dsv{other.dsv}, buffer_size{other.buffer_size} {}
ImageView& ImageView::operator=(ImageView&& other) noexcept {
    if (this != &other) { Release(); static_cast<VideoCommon::ImageViewBase&>(*this) = std::move(other);
        runtime = std::exchange(other.runtime, nullptr); slot_images = other.slot_images; image = other.image;
        srv = other.srv; uav = other.uav; rtv = other.rtv; dsv = other.dsv; buffer_size = other.buffer_size; }
    return *this;
}
bool ImageView::IsRescaled() const noexcept { return slot_images && (*slot_images)[image_id].IsRescaled(); }

Sampler::Sampler(TextureCacheRuntime& runtime_, const Tegra::Texture::TSCEntry& config) : runtime{&runtime_} {
    handle = runtime->sampler_descriptors.Allocate();
    const bool linear_min = config.min_filter == Tegra::Texture::TextureFilter::Linear;
    const bool linear_mag = config.mag_filter == Tegra::Texture::TextureFilter::Linear;
    const bool linear_mip = config.mipmap_filter == Tegra::Texture::TextureMipmapFilter::Linear;
    D3D12_FILTER_REDUCTION_TYPE reduction = D3D12_FILTER_REDUCTION_TYPE_STANDARD;
    if (config.depth_compare_enabled) {
        reduction = D3D12_FILTER_REDUCTION_TYPE_COMPARISON;
    } else if (config.reduction_filter == Tegra::Texture::SamplerReduction::Min) {
        reduction = D3D12_FILTER_REDUCTION_TYPE_MINIMUM;
    } else if (config.reduction_filter == Tegra::Texture::SamplerReduction::Max) {
        reduction = D3D12_FILTER_REDUCTION_TYPE_MAXIMUM;
    }
    const f32 anisotropy = std::clamp(config.MaxAnisotropy(), 1.0f, 16.0f);
    D3D12_FILTER filter = anisotropy > 1.0f ? D3D12_ENCODE_ANISOTROPIC_FILTER(reduction) :
        D3D12_ENCODE_BASIC_FILTER(linear_min ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT,
        linear_mag ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT,
        linear_mip ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT,
        reduction);
    const auto border = config.BorderColor();
    D3D12_SAMPLER_DESC desc{.Filter = filter, .AddressU = AddressMode(config.wrap_u),
        .AddressV = AddressMode(config.wrap_v), .AddressW = AddressMode(config.wrap_p),
        .MipLODBias = config.LodBias(), .MaxAnisotropy = static_cast<u32>(anisotropy),
        .ComparisonFunc = Compare(config.depth_compare_func), .BorderColor = {border[0], border[1], border[2], border[3]},
        .MinLOD = config.mipmap_filter == Tegra::Texture::TextureMipmapFilter::None ? 0.0f : config.MinLod(),
        .MaxLOD = config.mipmap_filter == Tegra::Texture::TextureMipmapFilter::None ? 0.25f : config.MaxLod()};
    runtime->device.Get()->CreateSampler(&desc, handle);
}
Sampler::~Sampler() { if (runtime && handle.ptr) runtime->sampler_descriptors.Free(handle); }
Sampler::Sampler(Sampler&& other) noexcept : runtime{std::exchange(other.runtime, nullptr)}, handle{other.handle} {}
Sampler& Sampler::operator=(Sampler&& other) noexcept {
    if (this != &other) { if (runtime && handle.ptr) runtime->sampler_descriptors.Free(handle);
        runtime = std::exchange(other.runtime, nullptr); handle = other.handle; } return *this;
}

Framebuffer::Framebuffer(TextureCacheRuntime&, std::span<ImageView*, NUM_RT> color_buffers,
                         ImageView* depth_buffer, const VideoCommon::RenderTargets& key)
    : extent{key.size}, is_rescaled{key.is_rescaled} {
    for (size_t index = 0; index < color_buffers.size(); ++index) {
        if (!color_buffers[index]) continue;
        colors[key.draw_buffers[index]] = color_buffers[index]->RenderTarget();
        num_colors = std::max(num_colors, static_cast<u32>(key.draw_buffers[index]) + 1);
    }
    if (depth_buffer) depth = depth_buffer->DepthStencil();
}

void TextureCacheRuntime::RunSelfTest() {
    try {
        constexpr u32 width = 13, height = 7, bytes = width * height * 4;
        VideoCommon::ImageInfo info{}; info.format = PixelFormat::A8B8G8R8_UNORM;
        info.type = ImageType::e2D; info.resources = {.levels = 1, .layers = 1}; info.size = {width, height, 1};
        Image image{*this, info, 0, 0};
        auto upload = UploadStagingBuffer(bytes); auto readback = DownloadStagingBuffer(bytes, true);
        for (u32 i = 0; i < bytes; ++i) upload.mapped_span[i] = static_cast<u8>((i * 29 + 7) & 0xff);
        const BufferImageCopy copy{.buffer_offset = 0, .buffer_size = bytes, .buffer_row_length = width,
            .buffer_image_height = height, .image_subresource = {}, .image_offset = {}, .image_extent = {width, height, 1}};
        image.UploadMemory(upload, std::span{&copy, 1}); image.DownloadMemory(readback, std::span{&copy, 1});
        FreeDeferredStagingBuffer(readback); Finish();
        if (!std::equal(upload.mapped_span.begin(), upload.mapped_span.begin() + bytes, readback.mapped_span.begin()))
            throw std::runtime_error{"texture round-trip mismatch"};
        VideoCommon::ImageViewInfo view_info{ImageViewType::e2D, info.format};
        ImageView view{*this, view_info, ImageId{1}, image};
        Tegra::Texture::TSCEntry tsc{};
        tsc.raw[0] = 2U | (2U << 3) | (2U << 6);
        tsc.raw[1] = 2U | (2U << 4) | (1U << 6);
        Sampler sampler{*this, tsc};
        std::array<ImageView*, NUM_RT> colors{}; colors[0] = &view;
        VideoCommon::RenderTargets targets{}; targets.color_buffer_ids[0] = ImageId{1}; targets.draw_buffers[0] = 0; targets.size = {width, height};
        VideoCommon::ImageInfo depth_info{info}; depth_info.format = PixelFormat::D32_FLOAT;
        Image depth_image{*this, depth_info, 0, 0};
        VideoCommon::ImageViewInfo depth_view_info{ImageViewType::e2D, depth_info.format};
        ImageView depth_view{*this, depth_view_info, ImageId{2}, depth_image};
        ImageView null_view{*this, VideoCommon::NullImageViewParams{}};
        Framebuffer framebuffer{*this, colors, &depth_view, targets};
        LOG_INFO(Render, "D3D12: texture cache round-trip passed ({}x{} RGBA8 unaligned rows, SRV/UAV/RTV/DSV, null views, sampler, framebuffer)", width, height);
    } catch (const std::exception& error) {
        LOG_ERROR(Render, "D3D12: texture cache self-test failed: {}", error.what());
    }
}

template class VideoCommon::TextureCache<TextureCacheParams>;

} // namespace D3D12
