// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video_core/renderer_d3d12/d3d12_texture_cache.h"
#include "video_core/frame_trace.h"

#include <algorithm>
#include <bit>
#include <optional>
#include <array>
#include <atomic>
#include <cstring>
#include <numeric>
#include <stdexcept>
#include <utility>

#include <fmt/format.h>

#include "common/alignment.h"
#include "common/div_ceil.h"
#include "common/logging.h"
#include "common/settings.h"
#include "video_core/perf_counters.h"
#include "video_core/renderer_d3d12/d3d12_blit_image.h"
#include "video_core/renderer_d3d12/d3d12_device.h"
#include "video_core/renderer_d3d12/d3d12_scheduler.h"
#include "video_core/surface.h"
#include "video_core/texture_cache/accelerated_swizzle.h"
#include "video_core/texture_cache/decode_bc.h"
#include "video_core/texture_cache/render_targets.h"
#include "video_core/texture_cache/samples_helper.h"
#include "video_core/texture_cache/texture_cache.h"
#include "video_core/texture_cache/util.h"
#include "video_core/textures/astc.h"
#include "video_core/textures/bcn.h"

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

/// No ASTC in D3D12: ASTC images hold what the guest blocks decode to (RGBA8, by the CPU or the
/// ASTC compute shader) or, with recompression, what the CPU re-encodes them to (BC1/BC3), as the
/// generic ConvertImage path produces them.
FormatInfo AstcFormat(PixelFormat format, Settings::AstcRecompression recompression) {
    FormatInfo info{};
    const bool srgb = VideoCore::Surface::IsPixelFormatSRGB(format);
    info.converted = true;
    switch (recompression) {
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

FormatInfo NativeFormat(PixelFormat format) {
    FormatInfo info = BaseFormat(format);
    info.copy_format = format;
    if (info.resource != DXGI_FORMAT_UNKNOWN) {
        return info;
    }
    if (VideoCore::Surface::IsPixelFormatASTC(format)) {
        return AstcFormat(format, Settings::values.astc_recompression.GetValue());
    }
    // Formats without a DXGI mapping yet: keep an image so the cache works, never transfer data
    // into it (its bytes would not match the resource layout).
    info.resource = DXGI_FORMAT_R8G8B8A8_TYPELESS;
    info.view = DXGI_FORMAT_R8G8B8A8_UNORM;
    info.srv = DXGI_FORMAT_R8G8B8A8_UNORM;
    info.supported = false;
    return info;
}

/// What a block-compressed format decodes to on the CPU (VideoCommon::DecompressBCn), or nothing
/// for other formats.
///
/// The Series' texture unit and its copy engine disagree on where some layers of block-compressed
/// 2D arrays live: in Mario Wonder's 128x128x105 BC4 tile arrays, layers 84 and 100 read 32 rows
/// off and others read other texels, while CopyTextureRegion reads back the guest data (see
/// docs/xbox_d3d12_phase4.md, 0.2.44). Plain texels are copied without reinterpreting blocks.
std::optional<FormatInfo> DecodedBcFormat(PixelFormat format) {
    const auto make = [](DXGI_FORMAT resource, DXGI_FORMAT view, PixelFormat copy_format) {
        return FormatInfo{.resource = resource, .view = view, .srv = view, .converted = true,
                          .copy_format = copy_format};
    };
    switch (format) {
    case PixelFormat::BC1_RGBA_UNORM:
    case PixelFormat::BC2_UNORM:
    case PixelFormat::BC3_UNORM:
    case PixelFormat::BC7_UNORM:
        return make(DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM,
                    PixelFormat::A8B8G8R8_UNORM);
    case PixelFormat::BC1_RGBA_SRGB:
    case PixelFormat::BC2_SRGB:
    case PixelFormat::BC3_SRGB:
    case PixelFormat::BC7_SRGB:
        return make(DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
                    PixelFormat::A8B8G8R8_SRGB);
    case PixelFormat::BC4_UNORM:
        return make(DXGI_FORMAT_R8_TYPELESS, DXGI_FORMAT_R8_UNORM, PixelFormat::R8_UNORM);
    case PixelFormat::BC4_SNORM:
        return make(DXGI_FORMAT_R8_TYPELESS, DXGI_FORMAT_R8_SNORM, PixelFormat::R8_SNORM);
    case PixelFormat::BC5_UNORM:
        return make(DXGI_FORMAT_R8G8_TYPELESS, DXGI_FORMAT_R8G8_UNORM, PixelFormat::R8G8_UNORM);
    case PixelFormat::BC5_SNORM:
        return make(DXGI_FORMAT_R8G8_TYPELESS, DXGI_FORMAT_R8G8_SNORM, PixelFormat::R8G8_SNORM);
    case PixelFormat::BC6H_UFLOAT:
    case PixelFormat::BC6H_SFLOAT:
        return make(DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_FLOAT,
                    PixelFormat::R16G16B16A16_FLOAT);
    default:
        return std::nullopt;
    }
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

ComPtr<ID3D12Resource> CreateTransferBuffer(ID3D12Device* device, u64 size,
                                            bool unordered_access = false) {
    const D3D12_HEAP_PROPERTIES heap{.Type = D3D12_HEAP_TYPE_DEFAULT};
    const D3D12_RESOURCE_DESC desc{.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER, .Alignment = 0,
        .Width = size, .Height = 1, .DepthOrArraySize = 1, .MipLevels = 1,
        .Format = DXGI_FORMAT_UNKNOWN, .SampleDesc = {.Count = 1, .Quality = 0},
        .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
        .Flags = unordered_access ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
                                  : D3D12_RESOURCE_FLAG_NONE};
    // Buffers start in COMMON and are promoted on first use.
    ComPtr<ID3D12Resource> buffer;
    VideoCore::Perf::ScopedTimer timer{VideoCore::Perf::Counter::ResourceCreateUs,
                                       VideoCore::Perf::Counter::ResourcesCreated};
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

/// How the guest packs the texels of a two-plane depth-stencil format.
std::optional<DepthStencilLayout> GuestDepthStencilLayout(PixelFormat format) {
    switch (format) {
    case PixelFormat::S8_UINT_D24_UNORM: return DepthStencilLayout::Z24S8;
    case PixelFormat::D24_UNORM_S8_UINT: return DepthStencilLayout::S8Z24;
    case PixelFormat::X8_D24_UNORM: return DepthStencilLayout::X8Z24;
    case PixelFormat::D32_FLOAT_S8_UINT: return DepthStencilLayout::ZF32_X24S8;
    default: return std::nullopt;
    }
}

/// Bytes per texel of a depth-stencil plane in a copy footprint: the depth plane of D24S8 and
/// D32S8 copies as R32 (D24 in the low 24 bits), the stencil plane as R8 ("Planar Depth
/// Stencil", DirectX-Specs).
constexpr u32 PlaneTexelBytes(u32 plane) {
    return plane == 0 ? 4 : 1;
}

bool logged_unsupported_transfer = false;
bool logged_depth_stencil_transfer = false;
bool logged_depth_stencil_unaligned = false;
bool logged_depth_stencil_copy = false;
bool logged_depth_stencil_upload = false;
bool logged_depth_stencil_download = false;
bool logged_self_copy = false;
bool logged_decoded_copy = false;
bool logged_gpu_astc = false;
bool logged_reinterpret_copy = false;
bool logged_depth_size = false;
bool logged_view_format = false;
bool logged_self_blit = false;
bool logged_msaa_blit = false;
bool logged_mixed_blit = false;
bool logged_integer_blit = false;
bool logged_blit_target = false;
bool logged_stencil_blit = false;
bool logged_no_blit_helper = false;
bool logged_missing_rtv = false;
bool logged_min_max_filter = false;
bool logged_point_reduction = false;
bool logged_srv_fallback = false;
bool logged_view_family = false;

/// The typeless format a DXGI format belongs to (views may only use formats of their resource's
/// family). Formats without a family are their own.
DXGI_FORMAT TypelessFamily(DXGI_FORMAT format) {
    struct Family {
        u32 first; ///< the TYPELESS format
        u32 last;
    };
    // DXGI lists each family's typed formats right after its typeless one.
    static constexpr Family FAMILIES[] = {
        {DXGI_FORMAT_R32G32B32A32_TYPELESS, DXGI_FORMAT_R32G32B32A32_SINT},
        {DXGI_FORMAT_R32G32B32_TYPELESS, DXGI_FORMAT_R32G32B32_SINT},
        {DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_SINT},
        {DXGI_FORMAT_R32G32_TYPELESS, DXGI_FORMAT_R32G32_SINT},
        {DXGI_FORMAT_R32G8X24_TYPELESS, DXGI_FORMAT_X32_TYPELESS_G8X24_UINT},
        {DXGI_FORMAT_R10G10B10A2_TYPELESS, DXGI_FORMAT_R10G10B10A2_UINT},
        {DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_SINT},
        {DXGI_FORMAT_R16G16_TYPELESS, DXGI_FORMAT_R16G16_SINT},
        {DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_R32_SINT},
        {DXGI_FORMAT_R24G8_TYPELESS, DXGI_FORMAT_X24_TYPELESS_G8_UINT},
        {DXGI_FORMAT_R8G8_TYPELESS, DXGI_FORMAT_R8G8_SINT},
        {DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_R16_SINT},
        {DXGI_FORMAT_R8_TYPELESS, DXGI_FORMAT_R8_SINT},
        {DXGI_FORMAT_BC1_TYPELESS, DXGI_FORMAT_BC1_UNORM_SRGB},
        {DXGI_FORMAT_BC2_TYPELESS, DXGI_FORMAT_BC2_UNORM_SRGB},
        {DXGI_FORMAT_BC3_TYPELESS, DXGI_FORMAT_BC3_UNORM_SRGB},
        {DXGI_FORMAT_BC4_TYPELESS, DXGI_FORMAT_BC4_SNORM},
        {DXGI_FORMAT_BC5_TYPELESS, DXGI_FORMAT_BC5_SNORM},
        {DXGI_FORMAT_B8G8R8A8_TYPELESS, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB},
        {DXGI_FORMAT_B8G8R8X8_TYPELESS, DXGI_FORMAT_B8G8R8X8_UNORM_SRGB},
        {DXGI_FORMAT_BC6H_TYPELESS, DXGI_FORMAT_BC6H_SF16},
        {DXGI_FORMAT_BC7_TYPELESS, DXGI_FORMAT_BC7_UNORM_SRGB},
    };
    // The plain BGRA formats sit apart from their typeless ones.
    if (format == DXGI_FORMAT_B8G8R8A8_UNORM) {
        return DXGI_FORMAT_B8G8R8A8_TYPELESS;
    }
    if (format == DXGI_FORMAT_B8G8R8X8_UNORM) {
        return DXGI_FORMAT_B8G8R8X8_TYPELESS;
    }
    const auto value = static_cast<u32>(format);
    for (const Family& family : FAMILIES) {
        if (value >= family.first && value <= family.last) {
            return static_cast<DXGI_FORMAT>(family.first);
        }
    }
    return format;
}

/// Typed UAV stores every feature level 11.0 device supports ("Format support for Direct3D
/// feature level 11.0 hardware").
bool RequiredTypedUavStore(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R32G32B32A32_FLOAT: case DXGI_FORMAT_R32G32B32A32_UINT:
    case DXGI_FORMAT_R32G32B32A32_SINT:
    case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R16G16B16A16_UINT: case DXGI_FORMAT_R16G16B16A16_SNORM:
    case DXGI_FORMAT_R16G16B16A16_SINT:
    case DXGI_FORMAT_R32G32_FLOAT: case DXGI_FORMAT_R32G32_UINT: case DXGI_FORMAT_R32G32_SINT:
    case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R10G10B10A2_UINT:
    case DXGI_FORMAT_R11G11B10_FLOAT:
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UINT:
    case DXGI_FORMAT_R8G8B8A8_SNORM: case DXGI_FORMAT_R8G8B8A8_SINT:
    case DXGI_FORMAT_R16G16_FLOAT: case DXGI_FORMAT_R16G16_UNORM: case DXGI_FORMAT_R16G16_UINT:
    case DXGI_FORMAT_R16G16_SNORM: case DXGI_FORMAT_R16G16_SINT:
    case DXGI_FORMAT_R32_FLOAT: case DXGI_FORMAT_R32_UINT: case DXGI_FORMAT_R32_SINT:
    case DXGI_FORMAT_R8G8_UNORM: case DXGI_FORMAT_R8G8_UINT: case DXGI_FORMAT_R8G8_SNORM:
    case DXGI_FORMAT_R8G8_SINT:
    case DXGI_FORMAT_R16_FLOAT: case DXGI_FORMAT_R16_UNORM: case DXGI_FORMAT_R16_UINT:
    case DXGI_FORMAT_R16_SNORM: case DXGI_FORMAT_R16_SINT:
    case DXGI_FORMAT_R8_UNORM: case DXGI_FORMAT_R8_UINT: case DXGI_FORMAT_R8_SNORM:
    case DXGI_FORMAT_R8_SINT:
        return true;
    default:
        return false;
    }
}

/// Render targets every feature level 11.0 device supports (same table): the typed UAV formats
/// plus sRGB and BGR ones.
bool RequiredRenderTarget(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B5G6R5_UNORM: case DXGI_FORMAT_B5G5R5A1_UNORM:
    case DXGI_FORMAT_A8_UNORM:
        return true;
    default:
        return RequiredTypedUavStore(format);
    }
}

} // namespace

FormatInfo SurfaceFormat(PixelFormat format) {
    return NativeFormat(format);
}

namespace {
/// Sampler keys for SamplerHeap; 0 is the presenter's linear sampler.
std::atomic<u64> next_sampler_key{1};
std::atomic<bool> decode_bc_arrays{true};
std::atomic<bool> gpu_astc_decode{true};
std::atomic<int> gpu_astc_verify{0}; ///< GPU ASTC uploads left to check against the CPU
} // Anonymous namespace

void SetBcArrayDecode(bool enabled) {
    decode_bc_arrays.store(enabled, std::memory_order_relaxed);
}

void SetAstcGpuDecode(bool enabled) {
    gpu_astc_decode.store(enabled, std::memory_order_relaxed);
}

std::atomic<bool> gpu_astc_sync{false};
std::atomic<bool> gpu_astc_fresh{false};

void SetAstcGpuFresh(bool enabled) {
    gpu_astc_fresh.store(enabled, std::memory_order_relaxed);
}

void SetAstcGpuSync(bool enabled) {
    gpu_astc_sync.store(enabled, std::memory_order_relaxed);
}

void SetAstcGpuVerify(bool enabled) {
    gpu_astc_verify.store(enabled ? 64 : 0, std::memory_order_relaxed);
}

constexpr u64 ASTC_RGBA_SCRATCH_BUDGET = 32ULL * 1024 * 1024;
constexpr u64 ASTC_BC3_SCRATCH_BUDGET = 8ULL * 1024 * 1024;

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
      sampler_descriptors{samplers}, rtv_descriptors{rtvs}, dsv_descriptors{dsvs},
      texture_allocator{device_, scheduler_} {
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
    scheduler.Finish();
    scheduler.CollectGarbage();
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

void TextureCacheRuntime::EnsureAstcRgbaScratch(u32 width, u32 height) {
    if (astc_rgba_scratch && astc_rgba_width >= width && astc_rgba_height >= height) {
        return;
    }
    if (astc_rgba_scratch) {
        scheduler.DeferRelease(std::move(astc_rgba_scratch));
    }
    astc_rgba_width = std::max(astc_rgba_width, width);
    astc_rgba_height = std::max(astc_rgba_height, height);
    // Growing both axes independently could retain a needlessly huge rectangle after unrelated
    // portrait and landscape uploads. Stay inside the promised workspace and fit this request.
    if (static_cast<u64>(astc_rgba_width) * astc_rgba_height * 4 >
        ASTC_RGBA_SCRATCH_BUDGET) {
        astc_rgba_width = width;
        astc_rgba_height = height;
    }
    const D3D12_RESOURCE_DESC desc{
        .Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
        .Alignment = 0,
        .Width = astc_rgba_width,
        .Height = astc_rgba_height,
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .Format = DXGI_FORMAT_R8G8B8A8_TYPELESS,
        .SampleDesc = {.Count = 1, .Quality = 0},
        .Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
        .Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
    };
    const D3D12_HEAP_PROPERTIES heap{.Type = D3D12_HEAP_TYPE_DEFAULT};
    ThrowIfFailed(device.Get()->CreateCommittedResource(
                      &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                      IID_PPV_ARGS(&astc_rgba_scratch)),
                  "Create ASTC RGBA scratch texture");
    astc_rgba_state = D3D12_RESOURCE_STATE_COMMON;
    LOG_INFO(Render, "D3D12: ASTC RGBA scratch {}x{} ({} KiB)", astc_rgba_width,
             astc_rgba_height,
             static_cast<u64>(astc_rgba_width) * astc_rgba_height * 4 / 1024);
}

void TextureCacheRuntime::EnsureAstcBc3Scratch(u64 size) {
    size = Common::AlignUp(size, static_cast<u64>(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT));
    if (astc_bc3_scratch && astc_bc3_size >= size) {
        return;
    }
    if (astc_bc3_scratch) {
        scheduler.DeferRelease(std::move(astc_bc3_scratch));
    }
    astc_bc3_size = std::min(ASTC_BC3_SCRATCH_BUDGET,
                             std::bit_ceil(std::max<u64>(size, 64ULL * 1024)));
    astc_bc3_scratch = CreateTransferBuffer(device.Get(), astc_bc3_size, true);
    astc_bc3_state = D3D12_RESOURCE_STATE_COMMON;
    LOG_INFO(Render, "D3D12: ASTC BC3 scratch {} KiB", astc_bc3_size / 1024);
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
    gc_pending_bytes -= 1ULL << map.log2_level;
    staging.FreeDeferred(map); // Remains fence-protected even if an obsolete copy is still in flight.
    map.buffer = nullptr;
}

bool TextureCacheRuntime::PrepareGcDownload(Image& image,
                                           std::span<const BufferImageCopy> copies,
                                           StagingBufferRef& map) {
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
            scheduler.Wait(pending->tick);
            ++gc_sync;
            VideoCore::FrameTrace::Mark(VideoCore::FrameTrace::Event::TextureGcReadback, 3, image.gpu_addr);
        }
        map = pending->map;
        ++gc_ready;
        VideoCore::FrameTrace::Mark(VideoCore::FrameTrace::Event::TextureGcReadback, 1, image.gpu_addr);
        return true;
    }
    const u64 size = image.unswizzled_size_bytes;
    // Large/unsupported transfers and real allocation emergencies preserve the original
    // recovery path. Bound pinned readback memory by the dedicated pool's actual power-of-two size.
    if (recover_now || size > PendingBudget || !image.CanTransfer()) {
        map = DownloadStagingBuffer(size);
        image.DownloadMemory(map, copies);
        Finish();
        ++gc_sync;
        VideoCore::FrameTrace::Mark(VideoCore::FrameTrace::Event::TextureGcReadback, 3, image.gpu_addr);
        return true;
    }
    const u64 reserved = std::bit_ceil(std::max<u64>(size, 1));
    if (reserved > PendingBudget - gc_pending_bytes) {
        VideoCore::FrameTrace::Mark(VideoCore::FrameTrace::Event::TextureGcReadback, 4, image.gpu_addr);
        return false;
    }
    auto readback = std::make_unique<Image::GcReadback>();
    readback->runtime = this;
    readback->map = DownloadStagingBuffer(size, true);
    gc_pending_bytes += 1ULL << readback->map.log2_level;
    gc_peak_pending_bytes = std::max(gc_peak_pending_bytes, gc_pending_bytes);
    image.DownloadMemory(readback->map, copies);
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
    return CachePressureController::Policy(pressure_level, second_pass);
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
        // GPU mode decodes arrays straight into their RGBA8 image. A single-layer BC3 image uses
        // the same decoder through the bounded scratch texture followed by the GPU BC3 encoder.
        const BlitImageHelper* const helper = runtime->blit_helper;
        gpu_decoded = gpu_astc_decode.load(std::memory_order_relaxed) && helper &&
                      helper->CanDecodeAstc() &&
                      info_.type == ImageType::e2D && info_.size.depth == 1 &&
                      info_.num_samples == 1 &&
                      (format.copy_format == PixelFormat::A8B8G8R8_UNORM ||
                       (format.copy_format == PixelFormat::BC3_UNORM &&
                        info_.resources.layers == 1 && helper->CanEncodeBc3()));
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

void Image::Transition(D3D12_RESOURCE_STATES next) {
    if (!resource) return;
    if (reinterpreted_ahead) {
        WriteBackReinterpreted();
    }
    if (state == next) {
        // Writes in one state are unordered without a barrier (see Buffer::Transition).
        if (next == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
            ++write_version;
            const D3D12_RESOURCE_BARRIER barrier{.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV,
                                                 .UAV = {.pResource = resource.Get()}};
            runtime->scheduler.CommandList()->ResourceBarrier(1, &barrier);
        } else if (next == D3D12_RESOURCE_STATE_COPY_DEST) {
            ++write_version;
            const D3D12_RESOURCE_BARRIER barriers[2]{
                {.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
                 .Transition = {.pResource = resource.Get(),
                                .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                .StateBefore = D3D12_RESOURCE_STATE_COPY_DEST,
                                .StateAfter = D3D12_RESOURCE_STATE_COMMON}},
                {.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
                 .Transition = {.pResource = resource.Get(),
                                .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                .StateBefore = D3D12_RESOURCE_STATE_COMMON,
                                .StateAfter = D3D12_RESOURCE_STATE_COPY_DEST}},
            };
            runtime->scheduler.CommandList()->ResourceBarrier(2, barriers);
        }
        return;
    }
    constexpr D3D12_RESOURCE_STATES WRITE_STATES =
        D3D12_RESOURCE_STATE_RENDER_TARGET | D3D12_RESOURCE_STATE_UNORDERED_ACCESS |
        D3D12_RESOURCE_STATE_DEPTH_WRITE | D3D12_RESOURCE_STATE_COPY_DEST |
        D3D12_RESOURCE_STATE_RESOLVE_DEST;
    if ((next & WRITE_STATES) != 0) {
        ++write_version;
    }
    const D3D12_RESOURCE_BARRIER barrier{.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
        .Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE,
        .Transition = {.pResource = resource.Get(), .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                       .StateBefore = state, .StateAfter = next}};
    runtime->scheduler.CommandList()->ResourceBarrier(1, &barrier);
    state = next;
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
    ComPtr<ID3D12Resource> transfer = CreateTransferBuffer(runtime->device.Get(), total_bytes);
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
    runtime->scheduler.DeferRelease(std::move(transfer));
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
    ComPtr<ID3D12Resource> transfer = CreateTransferBuffer(runtime->device.Get(), total_bytes);
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
    runtime->scheduler.DeferRelease(std::move(transfer));
    slice_array_version = write_version;
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
        WarnOnce(logged_unsupported_transfer, "texture format {} has no DXGI mapping yet; its "
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

Image::PlaneFootprints Image::Footprints(s32 level) const {
    // Both planes of one subresource in one buffer: plane 1 starts at the next placement
    // boundary after plane 0. The footprint of a level does not depend on its layer.
    const D3D12_RESOURCE_DESC desc = resource->GetDesc();
    PlaneFootprints result{};
    u64 depth_bytes = 0;
    runtime->device.Get()->GetCopyableFootprints(&desc, Subresource(level, 0, 0), 1, 0,
                                                 &result.planes[0], nullptr, nullptr,
                                                 &depth_bytes);
    const u64 stencil_offset = Common::AlignUp(depth_bytes, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
    u64 stencil_bytes = 0;
    runtime->device.Get()->GetCopyableFootprints(&desc, Subresource(level, 0, 1), 1,
                                                 stencil_offset, &result.planes[1], nullptr,
                                                 nullptr, &stencil_bytes);
    result.size = stencil_offset + stencil_bytes;
    return result;
}

void Image::CopyPlanes(s32 level, s32 layer, ID3D12Resource* buffer,
                       const PlaneFootprints& footprints, bool to_buffer) {
    Transition(to_buffer ? D3D12_RESOURCE_STATE_COPY_SOURCE : D3D12_RESOURCE_STATE_COPY_DEST);
    auto* const commands = runtime->scheduler.CommandList();
    for (u32 plane = 0; plane < 2; ++plane) {
        const D3D12_TEXTURE_COPY_LOCATION texture{
            .pResource = resource.Get(), .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
            .SubresourceIndex = Subresource(level, layer, plane)};
        const D3D12_TEXTURE_COPY_LOCATION placed{.pResource = buffer,
                                                 .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
                                                 .PlacedFootprint = footprints.planes[plane]};
        // Whole subresources only: CopyTextureRegion does not copy part of a depth-stencil one
        // (offsets 0 and no source box).
        if (to_buffer) {
            commands->CopyTextureRegion(&placed, 0, 0, 0, &texture, nullptr);
        } else {
            commands->CopyTextureRegion(&texture, 0, 0, 0, &placed, nullptr);
        }
    }
}

void Image::UploadDepthStencil(ID3D12Resource* buffer, size_t base_offset,
                               std::span<const BufferImageCopy> copies) {
    BlitImageHelper* const helper = runtime->blit_helper;
    const DepthStencilLayout guest_layout = *GuestDepthStencilLayout(info.format);
    ID3D12Device* const device = runtime->device.Get();
    // The pack shader reads the staging memory directly: UPLOAD buffers already are readable,
    // the buffer cache's DEFAULT ones rest in COMMON.
    const bool source_default = HeapType(buffer) == D3D12_HEAP_TYPE_DEFAULT;
    if (source_default) {
        TransitionBuffer(runtime->scheduler.CommandList(), buffer, D3D12_RESOURCE_STATE_COMMON,
                         D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    const D3D12_GPU_VIRTUAL_ADDRESS source_base = buffer->GetGPUVirtualAddress();
    for (const auto& copy : copies) {
        const CopyLayout layout = Layout(copy);
        const s32 level = copy.image_subresource.base_level;
        const PlaneFootprints footprints = Footprints(level);
        const u32 level_width = footprints.planes[0].Footprint.Width;
        const u32 level_height = footprints.planes[0].Footprint.Height;
        const u32 x = std::min(static_cast<u32>(copy.image_offset.x), level_width);
        const u32 y = std::min(static_cast<u32>(copy.image_offset.y), level_height);
        const u32 width = std::min(copy.image_extent.width, level_width - x);
        const u32 height = std::min(copy.image_extent.height, level_height - y);
        const bool whole = x == 0 && y == 0 && width == level_width && height == level_height;
        const u32 layers = static_cast<u32>(std::max(1, copy.image_subresource.num_layers));
        for (u32 layer = 0; layer < layers; ++layer) {
            const s32 image_layer = copy.image_subresource.base_layer + static_cast<s32>(layer);
            const u64 source_offset = base_offset + copy.buffer_offset +
                                      static_cast<u64>(layer) * layout.tight_slice;
            if (source_offset % sizeof(u32) != 0 || layout.row_bytes % sizeof(u32) != 0) {
                WarnOnce(logged_depth_stencil_unaligned, "depth-stencil ({}) upload at offset {} "
                         "is not word aligned; skipped", info.format, source_offset);
                continue;
            }
            auto* const commands = runtime->scheduler.CommandList();
            ComPtr<ID3D12Resource> planes = CreateTransferBuffer(device, footprints.size, true);
            if (whole) {
                TransitionBuffer(commands, planes.Get(), D3D12_RESOURCE_STATE_COMMON,
                                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            } else {
                // The planes go back as whole subresources: start from what the image holds so
                // the texels outside the region stay.
                CopyPlanes(level, image_layer, planes.Get(), footprints, true);
                TransitionBuffer(commands, planes.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            }
            const D3D12_GPU_VIRTUAL_ADDRESS address = source_base + source_offset;
            const D3D12_GPU_VIRTUAL_ADDRESS aligned = address & ~D3D12_GPU_VIRTUAL_ADDRESS{15};
            helper->SplitDepthStencil({
                .source = aligned,
                .destination = planes->GetGPUVirtualAddress(),
                .packed_offset = static_cast<u32>((address - aligned) / sizeof(u32)),
                .packed_row = layout.row_bytes / static_cast<u32>(sizeof(u32)),
                .depth_pitch = footprints.planes[0].Footprint.RowPitch / 4,
                .stencil_offset = static_cast<u32>(footprints.planes[1].Offset / 4),
                .stencil_pitch = footprints.planes[1].Footprint.RowPitch / 4,
                .x = x,
                .y = y,
                .width = width,
                .height = height,
                .row_texels = 0,
                .layout = guest_layout,
            });
            TransitionBuffer(commands, planes.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                             D3D12_RESOURCE_STATE_COPY_SOURCE);
            CopyPlanes(level, image_layer, planes.Get(), footprints, false);
            runtime->scheduler.DeferRelease(std::move(planes));
            if (!logged_depth_stencil_upload) {
                logged_depth_stencil_upload = true;
                LOG_INFO(Render, "D3D12: first depth-stencil upload ({} {}x{} level {} region "
                         "{},{} {}x{}, {} source) split into its planes",
                         info.format, level_width, level_height, level, x, y, width, height,
                         source_default ? "GPU" : "staging");
            }
        }
    }
    if (source_default) {
        TransitionBuffer(runtime->scheduler.CommandList(), buffer,
                         D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                         D3D12_RESOURCE_STATE_COMMON);
    }
}

void Image::DownloadDepthStencil(std::span<ID3D12Resource*> buffers, std::span<size_t> offsets,
                                 std::span<const BufferImageCopy> copies) {
    BlitImageHelper* const helper = runtime->blit_helper;
    const DepthStencilLayout guest_layout = *GuestDepthStencilLayout(info.format);
    const u32 texel_bytes = VideoCore::Surface::BytesPerBlock(format.copy_format);
    ID3D12Device* const device = runtime->device.Get();
    bool copied = false;
    for (const auto& copy : copies) {
        const CopyLayout layout = Layout(copy);
        const s32 level = copy.image_subresource.base_level;
        const PlaneFootprints footprints = Footprints(level);
        const u32 level_width = footprints.planes[0].Footprint.Width;
        const u32 level_height = footprints.planes[0].Footprint.Height;
        const u32 x = std::min(static_cast<u32>(copy.image_offset.x), level_width);
        const u32 y = std::min(static_cast<u32>(copy.image_offset.y), level_height);
        const u32 width = std::min(copy.image_extent.width, level_width - x);
        const u32 height = std::min(copy.image_extent.height, level_height - y);
        const u64 packed_size = static_cast<u64>(layout.row_bytes) * height;
        if (packed_size == 0 || layout.row_bytes % sizeof(u32) != 0) {
            continue;
        }
        const u32 layers = static_cast<u32>(std::max(1, copy.image_subresource.num_layers));
        for (u32 layer = 0; layer < layers; ++layer) {
            const s32 image_layer = copy.image_subresource.base_layer + static_cast<s32>(layer);
            auto* const commands = runtime->scheduler.CommandList();
            ComPtr<ID3D12Resource> planes = CreateTransferBuffer(device, footprints.size);
            CopyPlanes(level, image_layer, planes.Get(), footprints, true);
            TransitionBuffer(commands, planes.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            ComPtr<ID3D12Resource> packed = CreateTransferBuffer(device, packed_size, true);
            TransitionBuffer(commands, packed.Get(), D3D12_RESOURCE_STATE_COMMON,
                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            helper->MergeDepthStencil({
                .source = planes->GetGPUVirtualAddress(),
                .destination = packed->GetGPUVirtualAddress(),
                .packed_offset = 0,
                .packed_row = layout.row_bytes / static_cast<u32>(sizeof(u32)),
                .depth_pitch = footprints.planes[0].Footprint.RowPitch / 4,
                .stencil_offset = static_cast<u32>(footprints.planes[1].Offset / 4),
                .stencil_pitch = footprints.planes[1].Footprint.RowPitch / 4,
                .x = x,
                .y = y,
                .width = width,
                .height = height,
                .row_texels = layout.row_bytes / texel_bytes,
                .layout = guest_layout,
            });
            TransitionBuffer(commands, packed.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                             D3D12_RESOURCE_STATE_COPY_SOURCE);
            // As in the color path: every buffer receives every copy, each at its own offset.
            for (size_t target = 0; target < buffers.size(); ++target) {
                const u64 dest_offset = offsets[target] + copy.buffer_offset +
                                        static_cast<u64>(layer) * layout.tight_slice;
                commands->CopyBufferRegion(buffers[target], dest_offset, packed.Get(), 0,
                                           packed_size);
            }
            copied = true;
            runtime->scheduler.DeferRelease(std::move(planes));
            runtime->scheduler.DeferRelease(std::move(packed));
            if (!logged_depth_stencil_download) {
                logged_depth_stencil_download = true;
                LOG_INFO(Render, "D3D12: first depth-stencil download ({} {}x{} level {} region "
                         "{},{} {}x{}) merged from its planes",
                         info.format, level_width, level_height, level, x, y, width, height);
            }
        }
    }
    if (copied) {
        for (ID3D12Resource* const buffer : buffers) {
            DecayIfDefault(runtime->scheduler.CommandList(), buffer,
                           D3D12_RESOURCE_STATE_COPY_DEST);
        }
    }
}

void Image::CopyDepthStencilFrom(Image& src, std::span<const ImageCopy> copies) {
    ID3D12Device* const device = runtime->device.Get();
    for (const auto& copy : copies) {
        const PlaneFootprints src_footprints = src.Footprints(copy.src_subresource.base_level);
        const PlaneFootprints dst_footprints = Footprints(copy.dst_subresource.base_level);
        const auto& src_size = src_footprints.planes[0].Footprint;
        const auto& dst_size = dst_footprints.planes[0].Footprint;
        const u32 src_x = std::min(static_cast<u32>(copy.src_offset.x), src_size.Width);
        const u32 src_y = std::min(static_cast<u32>(copy.src_offset.y), src_size.Height);
        const u32 dst_x = std::min(static_cast<u32>(copy.dst_offset.x), dst_size.Width);
        const u32 dst_y = std::min(static_cast<u32>(copy.dst_offset.y), dst_size.Height);
        const u32 width = std::min({copy.extent.width, src_size.Width - src_x,
                                    dst_size.Width - dst_x});
        const u32 height = std::min({copy.extent.height, src_size.Height - src_y,
                                     dst_size.Height - dst_y});
        if (width == 0 || height == 0) {
            continue;
        }
        const bool whole = src_x == 0 && src_y == 0 && dst_x == 0 && dst_y == 0 &&
                           width == src_size.Width && height == src_size.Height &&
                           width == dst_size.Width && height == dst_size.Height;
        for (s32 layer = 0; layer < copy.src_subresource.num_layers; ++layer) {
            const s32 src_layer = copy.src_subresource.base_layer + layer;
            const s32 dst_layer = copy.dst_subresource.base_layer + layer;
            auto* const commands = runtime->scheduler.CommandList();
            if (whole) {
                src.Transition(D3D12_RESOURCE_STATE_COPY_SOURCE);
                Transition(D3D12_RESOURCE_STATE_COPY_DEST);
                for (u32 plane = 0; plane < 2; ++plane) {
                    const D3D12_TEXTURE_COPY_LOCATION source{
                        .pResource = src.Handle(),
                        .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
                        .SubresourceIndex =
                            src.Subresource(copy.src_subresource.base_level, src_layer, plane)};
                    const D3D12_TEXTURE_COPY_LOCATION target{
                        .pResource = resource.Get(),
                        .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
                        .SubresourceIndex =
                            Subresource(copy.dst_subresource.base_level, dst_layer, plane)};
                    commands->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
                }
                continue;
            }
            // Part of a subresource: both go through footprints, the region is copied row by
            // row between them, and the destination returns whole.
            ComPtr<ID3D12Resource> src_planes = CreateTransferBuffer(device, src_footprints.size);
            ComPtr<ID3D12Resource> dst_planes = CreateTransferBuffer(device, dst_footprints.size);
            src.CopyPlanes(copy.src_subresource.base_level, src_layer, src_planes.Get(),
                           src_footprints, true);
            CopyPlanes(copy.dst_subresource.base_level, dst_layer, dst_planes.Get(),
                       dst_footprints, true);
            TransitionBuffer(commands, src_planes.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                             D3D12_RESOURCE_STATE_COPY_SOURCE);
            for (u32 plane = 0; plane < 2; ++plane) {
                const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& from = src_footprints.planes[plane];
                const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& to = dst_footprints.planes[plane];
                const u32 bytes = PlaneTexelBytes(plane);
                for (u32 row = 0; row < height; ++row) {
                    commands->CopyBufferRegion(
                        dst_planes.Get(),
                        to.Offset + static_cast<u64>(dst_y + row) * to.Footprint.RowPitch +
                            static_cast<u64>(dst_x) * bytes,
                        src_planes.Get(),
                        from.Offset + static_cast<u64>(src_y + row) * from.Footprint.RowPitch +
                            static_cast<u64>(src_x) * bytes,
                        static_cast<u64>(width) * bytes);
                }
            }
            TransitionBuffer(commands, dst_planes.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                             D3D12_RESOURCE_STATE_COPY_SOURCE);
            CopyPlanes(copy.dst_subresource.base_level, dst_layer, dst_planes.Get(),
                       dst_footprints, false);
            runtime->scheduler.DeferRelease(std::move(src_planes));
            runtime->scheduler.DeferRelease(std::move(dst_planes));
        }
    }
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
            {
                VideoCore::Perf::ScopedNsTimer timer{
                    VideoCore::Perf::Counter::TextureUploadRecordNs};
                commands->CopyTextureRegion(&dst, copy.image_offset.x, copy.image_offset.y,
                                            copy.image_offset.z, &src, nullptr);
            }
            VideoCore::Perf::AddDetailed(VideoCore::Perf::Counter::TextureUploadCopies, 1);
            if (transfer) {
                runtime->scheduler.DeferRelease(std::move(transfer));
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
    if (!Image::AreCopyCompatible(src, dst)) {
        // Plain texels on one side, compressed blocks on the other: not copyable.
        WarnOnce(logged_decoded_copy, "copy between a host-decoded and a compressed image ({} -> "
                 "{}) skipped", src.info.format, dst.info.format);
        return;
    }
    if (src.IsDepthStencilPlanar() || dst.IsDepthStencilPlanar()) {
        // Plane by plane, and only between the same host layout (D24S8 or D32S8): anything else
        // reinterprets depth bits and needs a conversion shader.
        if (src.ResourceFormat() != dst.ResourceFormat() || src.info.num_samples > 1 ||
            dst.info.num_samples > 1) {
            WarnOnce(logged_depth_stencil_copy, "depth-stencil copy {} -> {} needs a conversion; "
                     "skipped", src.info.format, dst.info.format);
            return;
        }
        dst.CopyDepthStencilFrom(src, copies);
        return;
    }
    src.Transition(D3D12_RESOURCE_STATE_COPY_SOURCE);
    dst.Transition(D3D12_RESOURCE_STATE_COPY_DEST);
    // In the resource's own blocks: a decoded array copies texel by texel.
    const u32 block_w = VideoCore::Surface::DefaultBlockWidth(src.TransferFormat().copy_format);
    const u32 block_h = VideoCore::Surface::DefaultBlockHeight(src.TransferFormat().copy_format);
    const u32 dst_block_w = VideoCore::Surface::DefaultBlockWidth(dst.TransferFormat().copy_format);
    if (src.ResourceFormat() != dst.ResourceFormat() && block_w == 1 && dst_block_w == 1) {
        // Guest reinterpretations (RGBA8 texels read as R11G11B10, R32 as RGBA8...) cross DXGI
        // format families, which CopyTextureRegion rejects (the debug layer invalidates the list;
        // without it the result is undefined). The bytes go through a buffer instead.
        CopyThroughBuffer(dst, src, copies);
        return;
    }
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
void TextureCacheRuntime::CopyThroughBuffer(Image& dst, Image& src,
                                            std::span<const ImageCopy> copies) {
    const u32 texel_bytes = VideoCore::Surface::BytesPerBlock(src.TransferFormat().copy_format);
    if (texel_bytes != VideoCore::Surface::BytesPerBlock(dst.TransferFormat().copy_format)) {
        WarnOnce(logged_reinterpret_copy, "copy between formats of different texel sizes ({} -> "
                 "{}) skipped", src.info.format, dst.info.format);
        return;
    }
    auto* const commands = scheduler.CommandList();
    for (const auto& copy : copies) {
        const u32 depth = src.info.type == ImageType::e3D ? std::max(1U, copy.extent.depth) : 1U;
        const u32 row_pitch = AlignPitch(copy.extent.width * texel_bytes);
        const u64 size = static_cast<u64>(row_pitch) * copy.extent.height * depth;
        if (size == 0) {
            continue;
        }
        for (s32 layer = 0; layer < copy.src_subresource.num_layers; ++layer) {
            ComPtr<ID3D12Resource> transfer = CreateTransferBuffer(device.Get(), size);
            D3D12_TEXTURE_COPY_LOCATION footprint{.pResource = transfer.Get(),
                                                  .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
            footprint.PlacedFootprint.Footprint = {.Format = src.FootprintFormat(),
                                                   .Width = copy.extent.width,
                                                   .Height = copy.extent.height,
                                                   .Depth = depth,
                                                   .RowPitch = row_pitch};
            const D3D12_TEXTURE_COPY_LOCATION source{
                .pResource = src.Handle(), .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
                .SubresourceIndex = src.Subresource(copy.src_subresource.base_level,
                                                    copy.src_subresource.base_layer + layer)};
            const D3D12_BOX box{static_cast<u32>(copy.src_offset.x),
                                static_cast<u32>(copy.src_offset.y),
                                static_cast<u32>(copy.src_offset.z),
                                static_cast<u32>(copy.src_offset.x) + copy.extent.width,
                                static_cast<u32>(copy.src_offset.y) + copy.extent.height,
                                static_cast<u32>(copy.src_offset.z) + depth};
            commands->CopyTextureRegion(&footprint, 0, 0, 0, &source, &box);
            TransitionBuffer(commands, transfer.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                             D3D12_RESOURCE_STATE_COPY_SOURCE);
            // Same bytes, described in the destination's format.
            footprint.PlacedFootprint.Footprint.Format = dst.FootprintFormat();
            const D3D12_TEXTURE_COPY_LOCATION target{
                .pResource = dst.Handle(), .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
                .SubresourceIndex = dst.Subresource(copy.dst_subresource.base_level,
                                                    copy.dst_subresource.base_layer + layer)};
            commands->CopyTextureRegion(&target, copy.dst_offset.x, copy.dst_offset.y,
                                        copy.dst_offset.z, &footprint, nullptr);
            scheduler.DeferRelease(std::move(transfer));
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
void TextureCacheRuntime::BlitImage(Framebuffer*, ImageView& dst, ImageView& src,
                                    const Region2D& dst_region, const Region2D& src_region,
                                    Tegra::Engines::Fermi2D::Filter filter,
                                    Tegra::Engines::Fermi2D::Operation) {
    // Blend operations are drawn as copies, as in the Vulkan backend.
    Image* const dst_image = dst.SourceImage();
    Image* const src_image = src.SourceImage();
    if (!dst_image || !src_image || !dst_image->Handle() || !src_image->Handle()) {
        return;
    }
    if (dst_image == src_image) {
        // Whole-resource state tracking cannot hold a shader resource and a target at once.
        WarnOnce(logged_self_blit, "blits within one image are not supported yet; skipped");
        return;
    }
    if (dst_image->info.num_samples > 1 || src_image->info.num_samples > 1) {
        WarnOnce(logged_msaa_blit, "MSAA blits and resolves need phase 5; skipped");
        return;
    }
    const SurfaceType src_type = VideoCore::Surface::GetFormatType(src.format);
    const SurfaceType dst_type = VideoCore::Surface::GetFormatType(dst.format);
    const bool src_color = src_type == SurfaceType::ColorTexture;
    if (src_color != (dst_type == SurfaceType::ColorTexture)) {
        WarnOnce(logged_mixed_blit, "blit between color and depth ({} -> {}) skipped", src.format,
                 dst.format);
        return;
    }
    const s32 dst_width = dst_region.end.x - dst_region.start.x;
    const s32 dst_height = dst_region.end.y - dst_region.start.y;
    const bool same_extent = dst_width > 0 && dst_height > 0 &&
                             dst_width == src_region.end.x - src_region.start.x &&
                             dst_height == src_region.end.y - src_region.start.y;
    if (src_color && same_extent && src.format == dst.format) {
        if (!Image::AreCopyCompatible(*src_image, *dst_image)) {
            WarnOnce(logged_decoded_copy, "copy between a host-decoded and a compressed image ({} "
                     "-> {}) skipped", src.format, dst.format);
            return;
        }
        // An unscaled copy: exact for every format, integers included.
        src_image->Transition(D3D12_RESOURCE_STATE_COPY_SOURCE);
        dst_image->Transition(D3D12_RESOURCE_STATE_COPY_DEST);
        const D3D12_TEXTURE_COPY_LOCATION source{
            .pResource = src_image->Handle(), .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
            .SubresourceIndex = src_image->Subresource(src.range.base.level, src.range.base.layer)};
        const D3D12_TEXTURE_COPY_LOCATION target{
            .pResource = dst_image->Handle(), .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
            .SubresourceIndex = dst_image->Subresource(dst.range.base.level, dst.range.base.layer)};
        const D3D12_BOX box{static_cast<u32>(src_region.start.x),
                            static_cast<u32>(src_region.start.y), 0,
                            static_cast<u32>(src_region.end.x),
                            static_cast<u32>(src_region.end.y), 1};
        scheduler.CommandList()->CopyTextureRegion(&target, static_cast<u32>(dst_region.start.x),
                                                   static_cast<u32>(dst_region.start.y), 0,
                                                   &source, &box);
        return;
    }
    if (!blit_helper || !blit_helper->IsAvailable()) {
        WarnOnce(logged_no_blit_helper, "scaled blits need the shader path; skipped");
        return;
    }
    const VideoCommon::Extent2D src_size{src.size.width, src.size.height};
    if (src_color) {
        if (VideoCore::Surface::IsPixelFormatInteger(src.format) ||
            VideoCore::Surface::IsPixelFormatInteger(dst.format)) {
            // The blit shader samples and writes floats.
            WarnOnce(logged_integer_blit, "scaled blits of integer formats ({} -> {}) skipped",
                     src.format, dst.format);
            return;
        }
        if (!dst.RenderTarget().ptr) {
            WarnOnce(logged_blit_target, "blit target format {} cannot be rendered to; skipped",
                     dst.format);
            return;
        }
        src_image->Transition(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        src.PrepareRead(Shader::TextureType::Color2D);
        dst.PrepareRender();
        const bool linear = filter == Tegra::Engines::Fermi2D::Filter::Bilinear;
        blit_helper->BlitColor({dst.RenderTarget(), Format(dst.format).view, 1},
                               src.Handle(Shader::TextureType::Color2D),
                               linear ? blit_helper->LinearSampler()
                                      : blit_helper->NearestSampler(),
                               dst_region, src_region, src_size);
        return;
    }
    if (!dst.DepthStencil().ptr) {
        WarnOnce(logged_blit_target, "blit target format {} cannot be rendered to; skipped",
                 dst.format);
        return;
    }
    if (dst_type == SurfaceType::DepthStencil) {
        // Writing stencil from a shader needs SV_StencilRef, which the Series lacks.
        WarnOnce(logged_stencil_blit, "depth-stencil blits copy depth only ({})", dst.format);
    }
    src_image->Transition(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    dst_image->Transition(D3D12_RESOURCE_STATE_DEPTH_WRITE);
    blit_helper->BlitDepth({dst.DepthStencil(), Format(dst.format).dsv, 1},
                           src.Handle(Shader::TextureType::Color2D), dst_region, src_region,
                           src_size);
}
void TextureCacheRuntime::AccelerateImageUpload(
    Image& image, const StagingBufferRef& map,
    std::span<const VideoCommon::SwizzleParameters> swizzles, u32, u32) {
    if (!CanAccelerateImageUpload(image) || !image.Handle()) {
        return;
    }
    VideoCore::Perf::Add(VideoCore::Perf::Counter::TextureUploads, 1);
    VideoCore::Perf::Add(VideoCore::Perf::Counter::TextureUploadBytes, image.guest_size_bytes);
    VideoCore::Perf::Add(VideoCore::Perf::Counter::TextureGpuDecodes, 1);
    if (!logged_gpu_astc) {
        LOG_INFO(Render, "D3D12: first ASTC image decoded on the GPU ({} {}x{}, {} layers, {} "
                 "levels)", image.info.format, image.info.size.width, image.info.size.height,
                 image.info.resources.layers, image.info.resources.levels);
        logged_gpu_astc = true;
    }
    const bool encode_bc3 = image.TransferFormat().copy_format == PixelFormat::BC3_UNORM;
    if (!encode_bc3) {
        image.Transition(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    const u64 base = map.offset;
    static u32 logged_ranges = 0;
    if (logged_ranges < 16) {
        ++logged_ranges;
        const D3D12_RESOURCE_DESC buffer_desc = map.buffer->GetDesc();
        LOG_INFO(Render, "D3D12: ASTC GPU decode {} {}x{}x{} levels {}: blocks at 0x{:X}+0x{:X} "
                 "(buffer 0x{:X}, {} bytes), {} swizzles, layer stride {}",
                 image.info.format, image.info.size.width, image.info.size.height,
                 image.info.resources.layers, image.info.resources.levels,
                 map.buffer->GetGPUVirtualAddress() + base,
                 image.guest_size_bytes, map.buffer->GetGPUVirtualAddress(), buffer_desc.Width,
                 swizzles.size(), image.info.layer_stride);
    }
    const u32 layers = static_cast<u32>(image.info.resources.layers);
    for (const VideoCommon::SwizzleParameters& swizzle : swizzles) {
        const auto params = VideoCommon::Accelerated::MakeBlockLinearSwizzle2DParams(swizzle,
                                                                                     image.info);
        const u64 source_offset = base + swizzle.buffer_offset;
        const u64 buffer_size = map.buffer->GetDesc().Width;
        const u64 wanted_bytes = static_cast<u64>(params.layer_stride) * layers;
        const u64 source_bytes = source_offset < buffer_size
                                     ? std::min(wanted_bytes, buffer_size - source_offset)
                                     : 0;
        const u32 input_words = static_cast<u32>(std::min<u64>(source_bytes / 4,
                                                               std::numeric_limits<u32>::max()));
        const D3D12_CPU_DESCRIPTOR_HANDLE source_srv = view_descriptors.Allocate();
        const D3D12_SHADER_RESOURCE_VIEW_DESC source_desc{
            .Format = DXGI_FORMAT_R32_TYPELESS,
            .ViewDimension = D3D12_SRV_DIMENSION_BUFFER,
            .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
            .Buffer = {.FirstElement = input_words ? source_offset / 4 : 0,
                       .NumElements = input_words,
                       .StructureByteStride = 0,
                       .Flags = D3D12_BUFFER_SRV_FLAG_RAW},
        };
        device.Get()->CreateShaderResourceView(input_words ? map.buffer : nullptr, &source_desc,
                                               source_srv);

        const u32 block_width = VideoCore::Surface::DefaultBlockWidth(image.info.format);
        const u32 block_height = VideoCore::Surface::DefaultBlockHeight(image.info.format);
        if (!encode_bc3) {
            // Each level has a distinct subresource, so consecutive level dispatches require no
            // UAV barrier. The source is a bounded raw view rather than an unbounded root SRV.
            const D3D12_CPU_DESCRIPTOR_HANDLE uav = view_descriptors.Allocate();
            const D3D12_UNORDERED_ACCESS_VIEW_DESC desc{
                .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
                .ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY,
                .Texture2DArray = {.MipSlice = static_cast<UINT>(swizzle.level),
                                   .FirstArraySlice = 0,
                                   .ArraySize = layers,
                                   .PlaneSlice = 0},
            };
            device.Get()->CreateUnorderedAccessView(image.Handle(), nullptr, &desc, uav);
            blit_helper->DecodeAstc({
                .source = source_srv,
                .destination = uav,
                .block_width = block_width,
                .block_height = block_height,
                .layer_stride = params.layer_stride,
                .block_size = params.block_size,
                .x_shift = params.x_shift,
                .gob_block_height = params.block_height,
                .gob_block_height_mask = params.block_height_mask,
                .blocks_x = swizzle.num_tiles.width,
                .blocks_y = swizzle.num_tiles.height,
                .layers = layers,
                .input_words = input_words,
                .first_block_row = 0,
            });
            view_descriptors.Free(uav);
            view_descriptors.Free(source_srv);
            continue;
        }

        const u32 mip_width = std::max(1U, image.info.size.width >> swizzle.level);
        const u32 mip_height = std::max(1U, image.info.size.height >> swizzle.level);
        const u32 bc3_row_pitch = AlignPitch(Common::DivCeil(mip_width, 4U) * 16U);
        const u32 band_alignment = std::lcm(block_height, 4U);
        const u32 rgba_rows = static_cast<u32>(std::max<u64>(
            band_alignment, ASTC_RGBA_SCRATCH_BUDGET / (static_cast<u64>(mip_width) * 4)));
        const u32 bc3_rows = static_cast<u32>(std::max<u64>(
            band_alignment, (ASTC_BC3_SCRATCH_BUDGET / bc3_row_pitch) * 4));
        u32 band_rows = std::min({mip_height, rgba_rows, bc3_rows});
        band_rows = std::max(band_alignment, band_rows / band_alignment * band_alignment);
        band_rows = std::min(band_rows, Common::AlignUp(mip_height, band_alignment));
        if (gpu_astc_fresh.load(std::memory_order_relaxed)) {
            if (astc_rgba_scratch) {
                scheduler.DeferRelease(std::move(astc_rgba_scratch));
            }
            if (astc_bc3_scratch) {
                scheduler.DeferRelease(std::move(astc_bc3_scratch));
            }
            astc_rgba_width = astc_rgba_height = 0;
            astc_bc3_size = 0;
        }
        EnsureAstcRgbaScratch(mip_width, band_rows);
        const u64 bc3_bytes = static_cast<u64>(bc3_row_pitch) * Common::DivCeil(band_rows, 4U);
        EnsureAstcBc3Scratch(bc3_bytes);

        for (u32 first_y = 0; first_y < mip_height; first_y += band_rows) {
            const u32 rows = std::min(band_rows, mip_height - first_y);
            auto* const commands = scheduler.CommandList();
            if (astc_rgba_state != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
                TransitionBuffer(commands, astc_rgba_scratch.Get(), astc_rgba_state,
                                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                astc_rgba_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            } else {
                const D3D12_RESOURCE_BARRIER barrier{.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV,
                                                     .UAV = {.pResource = astc_rgba_scratch.Get()}};
                commands->ResourceBarrier(1, &barrier);
            }
            const D3D12_CPU_DESCRIPTOR_HANDLE rgba_uav = view_descriptors.Allocate();
            const D3D12_UNORDERED_ACCESS_VIEW_DESC rgba_uav_desc{
                .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
                .ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY,
                .Texture2DArray = {.MipSlice = 0, .FirstArraySlice = 0, .ArraySize = 1,
                                   .PlaneSlice = 0},
            };
            device.Get()->CreateUnorderedAccessView(astc_rgba_scratch.Get(), nullptr,
                                                    &rgba_uav_desc, rgba_uav);
            blit_helper->DecodeAstc({
                .source = source_srv,
                .destination = rgba_uav,
                .block_width = block_width,
                .block_height = block_height,
                .layer_stride = params.layer_stride,
                .block_size = params.block_size,
                .x_shift = params.x_shift,
                .gob_block_height = params.block_height,
                .gob_block_height_mask = params.block_height_mask,
                .blocks_x = swizzle.num_tiles.width,
                .blocks_y = Common::DivCeil(rows, block_height),
                .layers = 1,
                .input_words = input_words,
                .first_block_row = first_y / block_height,
            });
            view_descriptors.Free(rgba_uav);

            TransitionBuffer(commands, astc_rgba_scratch.Get(), astc_rgba_state,
                             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            astc_rgba_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            if (astc_bc3_state != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
                TransitionBuffer(commands, astc_bc3_scratch.Get(), astc_bc3_state,
                                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                astc_bc3_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            }
            const D3D12_CPU_DESCRIPTOR_HANDLE rgba_srv = view_descriptors.Allocate();
            const D3D12_SHADER_RESOURCE_VIEW_DESC rgba_srv_desc{
                .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
                .ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D,
                .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
                .Texture2D = {.MostDetailedMip = 0, .MipLevels = 1,
                              .PlaneSlice = 0, .ResourceMinLODClamp = 0.0f},
            };
            device.Get()->CreateShaderResourceView(astc_rgba_scratch.Get(), &rgba_srv_desc,
                                                   rgba_srv);
            blit_helper->EncodeBc3({rgba_srv, astc_bc3_scratch->GetGPUVirtualAddress(),
                                    mip_width, rows, rows,
                                    bc3_row_pitch / 4});
            view_descriptors.Free(rgba_srv);

            TransitionBuffer(commands, astc_bc3_scratch.Get(), astc_bc3_state,
                             D3D12_RESOURCE_STATE_COPY_SOURCE);
            astc_bc3_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
            if (first_y == 0 && gpu_astc_verify.fetch_sub(1, std::memory_order_relaxed) > 0) {
                // Deliberately synchronous and opt-in: compare the actual GPU bytes with the
                // established CPU ASTC->BC3 path before the destination texture can hide whether
                // a mismatch came from encoding or CopyTextureRegion.
                StagingBufferRef readback = DownloadStagingBuffer(bc3_bytes, true);
                commands->CopyBufferRegion(readback.buffer, readback.offset,
                                           astc_bc3_scratch.Get(), 0, bc3_bytes);
                D3D12_PLACED_SUBRESOURCE_FOOTPRINT rgba_footprint{};
                u64 rgba_readback_size = 0;
                const D3D12_RESOURCE_DESC rgba_desc = astc_rgba_scratch->GetDesc();
                device.Get()->GetCopyableFootprints(&rgba_desc, 0, 1, 0, &rgba_footprint, nullptr,
                                                     nullptr, &rgba_readback_size);
                StagingBufferRef rgba_readback = DownloadStagingBuffer(rgba_readback_size, true);
                TransitionBuffer(commands, astc_rgba_scratch.Get(), astc_rgba_state,
                                 D3D12_RESOURCE_STATE_COPY_SOURCE);
                astc_rgba_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
                rgba_footprint.Offset = rgba_readback.offset;
                const D3D12_TEXTURE_COPY_LOCATION rgba_source{
                    .pResource = astc_rgba_scratch.Get(),
                    .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
                    .SubresourceIndex = 0};
                const D3D12_TEXTURE_COPY_LOCATION rgba_destination{
                    .pResource = rgba_readback.buffer,
                    .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
                    .PlacedFootprint = rgba_footprint};
                commands->CopyTextureRegion(&rgba_destination, 0, 0, 0, &rgba_source, nullptr);
                scheduler.Finish();

                const u32 blocks_x = Common::DivCeil(mip_width, block_width);
                const u32 blocks_y = Common::DivCeil(rows, block_height);
                std::vector<u8> linear_astc(static_cast<size_t>(blocks_x) * blocks_y * 16);
                bool source_valid = true;
                u64 largest_source = 0;
                u64 first_invalid_source = 0;
                for (u32 block_y = 0; block_y < blocks_y; ++block_y) {
                    for (u32 block_x = 0; block_x < blocks_x; ++block_x) {
                        const u32 byte_x = block_x << 4U;
                        const u32 gob_y = block_y >> 3U;
                        const u32 swizzled = ((byte_x & 32U) << 3U) |
                                              ((block_y & 6U) << 5U) |
                                              ((byte_x & 16U) << 1U) |
                                              ((block_y & 1U) << 4U) | (byte_x & 15U);
                        u64 offset = (gob_y >> params.block_height) * params.block_size;
                        offset += (gob_y & params.block_height_mask) << 9U;
                        offset += (byte_x >> 6U) << params.x_shift;
                        offset += swizzled;
                        const u64 source = swizzle.buffer_offset + offset;
                        largest_source = std::max(largest_source, source + 16);
                        const size_t destination =
                            (static_cast<size_t>(block_y) * blocks_x + block_x) * 16;
                        if (source + 16 > map.mapped_span.size()) {
                            if (source_valid) {
                                first_invalid_source = source;
                            }
                            source_valid = false;
                            continue;
                        }
                        std::memcpy(linear_astc.data() + destination,
                                    map.mapped_span.data() + source, 16);
                    }
                }
                std::vector<u8> rgba(static_cast<size_t>(mip_width) * rows * 4);
                std::vector<u8> expected(static_cast<size_t>(Common::DivCeil(mip_width, 4U)) *
                                         Common::DivCeil(rows, 4U) * 16);
                Tegra::Texture::ASTC::Decompress(linear_astc, mip_width, rows, 1, block_width,
                                                 block_height, rgba);
                Tegra::Texture::BCN::CompressBC3(rgba, mip_width, rows, 1, expected);
                const u32 tight_row = Common::DivCeil(mip_width, 4U) * 16;
                std::vector<u8> gpu_bc3(expected.size());
                for (u32 row = 0; row < Common::DivCeil(rows, 4U); ++row) {
                    std::memcpy(gpu_bc3.data() + static_cast<size_t>(row) * tight_row,
                                readback.mapped_span.data() + static_cast<size_t>(row) *
                                                                  bc3_row_pitch,
                                tight_row);
                }
                std::vector<u8> gpu_rgba(rgba.size());
                VideoCommon::BufferImageCopy verify_copy{
                    .buffer_offset = 0,
                    .buffer_size = gpu_bc3.size(),
                    .buffer_row_length = Common::AlignUp(mip_width, 4U),
                    .buffer_image_height = Common::AlignUp(rows, 4U),
                    .image_subresource = {.base_level = 0, .base_layer = 0, .num_layers = 1},
                    .image_offset = {},
                    .image_extent = {mip_width, rows, 1},
                };
                VideoCommon::DecompressBCn(gpu_bc3, gpu_rgba, verify_copy,
                                           PixelFormat::BC3_UNORM);
                std::array<u64, 4> absolute_error{};
                std::array<u64, 4> large_error{};
                u64 lost_alpha = 0;
                for (size_t pixel = 0; pixel < rgba.size() / 4; ++pixel) {
                    for (size_t channel = 0; channel < 4; ++channel) {
                        const u32 error = static_cast<u32>(std::abs(
                            static_cast<int>(gpu_rgba[pixel * 4 + channel]) -
                            static_cast<int>(rgba[pixel * 4 + channel])));
                        absolute_error[channel] += error;
                        large_error[channel] += error > 32;
                    }
                    lost_alpha += rgba[pixel * 4 + 3] >= 128 && gpu_rgba[pixel * 4 + 3] < 32;
                }
                u64 mismatches = 0;
                u64 rgba_mismatches = 0;
                size_t first_mismatch = std::numeric_limits<size_t>::max();
                size_t first_rgba_mismatch = std::numeric_limits<size_t>::max();
                for (u32 row = 0; row < rows; ++row) {
                    for (u32 byte = 0; byte < mip_width * 4; ++byte) {
                        const size_t tight = static_cast<size_t>(row) * mip_width * 4 + byte;
                        const u8 gpu = rgba_readback.mapped_span[
                            rgba_footprint.Offset - rgba_readback.offset +
                            static_cast<size_t>(row) * rgba_footprint.Footprint.RowPitch + byte];
                        if (gpu != rgba[tight]) {
                            ++rgba_mismatches;
                            first_rgba_mismatch = std::min(first_rgba_mismatch, tight);
                        }
                    }
                }
                for (u32 row = 0; row < Common::DivCeil(rows, 4U); ++row) {
                    for (u32 byte = 0; byte < tight_row; ++byte) {
                        const u8 gpu = readback.mapped_span[static_cast<size_t>(row) *
                                                                bc3_row_pitch + byte];
                        const u8 cpu = expected[static_cast<size_t>(row) * tight_row + byte];
                        if (gpu != cpu) {
                            ++mismatches;
                            first_mismatch = std::min(first_mismatch,
                                                      static_cast<size_t>(row) * tight_row + byte);
                        }
                    }
                }
                if (first_rgba_mismatch != std::numeric_limits<size_t>::max()) {
                    const size_t pixel = first_rgba_mismatch / 4;
                    const u32 px = static_cast<u32>(pixel % mip_width);
                    const u32 py = static_cast<u32>(pixel / mip_width);
                    const u8* const gpu_px = rgba_readback.mapped_span.data() +
                                             (rgba_footprint.Offset - rgba_readback.offset) +
                                             static_cast<size_t>(py) *
                                                 rgba_footprint.Footprint.RowPitch +
                                             static_cast<size_t>(px) * 4;
                    const u8* const cpu_px = rgba.data() + pixel * 4;
                    u64 bad_blocks = 0;
                    const u32 blocks_x_check = Common::DivCeil(mip_width, block_width);
                    for (u32 by = 0; by < Common::DivCeil(rows, block_height); ++by) {
                        for (u32 bx = 0; bx < blocks_x_check; ++bx) {
                            bool bad = false;
                            for (u32 y = by * block_height;
                                 !bad && y < std::min(rows, (by + 1) * block_height); ++y) {
                                for (u32 x = bx * block_width;
                                     !bad && x < std::min(mip_width, (bx + 1) * block_width);
                                     ++x) {
                                    const u8* g = rgba_readback.mapped_span.data() +
                                                  (rgba_footprint.Offset - rgba_readback.offset) +
                                                  static_cast<size_t>(y) *
                                                      rgba_footprint.Footprint.RowPitch +
                                                  static_cast<size_t>(x) * 4;
                                    const u8* c = rgba.data() +
                                                  (static_cast<size_t>(y) * mip_width + x) * 4;
                                    bad = std::memcmp(g, c, 4) != 0;
                                }
                            }
                            bad_blocks += bad;
                        }
                    }
                    const size_t block_offset =
                        (static_cast<size_t>(py / block_height) * blocks_x_check +
                         px / block_width) * 16;
                    const u8* const blk = linear_astc.data() + block_offset;
                    LOG_INFO(Render,
                             "D3D12: ASTC verify {} {}x{}: {} of {} blocks differ; first at "
                             "({}, {}) GPU {:02X}{:02X}{:02X}{:02X} CPU {:02X}{:02X}{:02X}{:02X}; "
                             "block {:02X}{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}"
                             "{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}",
                             image.info.format, mip_width, rows, bad_blocks,
                             blocks_x_check * Common::DivCeil(rows, block_height), px, py,
                             gpu_px[0], gpu_px[1], gpu_px[2], gpu_px[3], cpu_px[0], cpu_px[1],
                             cpu_px[2], cpu_px[3], blk[0], blk[1], blk[2], blk[3], blk[4],
                             blk[5], blk[6], blk[7], blk[8], blk[9], blk[10], blk[11], blk[12],
                             blk[13], blk[14], blk[15]);
                }
                LOG_INFO(Render,
                         "D3D12: ASTC verify {}x{} source {}: RGBA differs in {} / {} bytes first "
                         "{}; GPU BC3 differs in {} / {} bytes first {}; source span {} max {} "
                         "invalid {}", mip_width, rows,
                         source_valid ? "valid" : "INVALID",
                         rgba_mismatches, rgba.size(),
                         first_rgba_mismatch == std::numeric_limits<size_t>::max()
                             ? 0
                             : first_rgba_mismatch,
                         mismatches, expected.size(),
                         first_mismatch == std::numeric_limits<size_t>::max() ? 0
                                                                             : first_mismatch,
                         map.mapped_span.size(), largest_source, first_invalid_source);
                LOG_INFO(Render,
                         "D3D12: ASTC verify decoded BC3 abs error RGBA [{}, {}, {}, {}], >32 "
                         "[{}, {}, {}, {}], lost alpha {} / {} pixels",
                         absolute_error[0], absolute_error[1], absolute_error[2],
                         absolute_error[3], large_error[0], large_error[1], large_error[2],
                         large_error[3], lost_alpha, rgba.size() / 4);
                LOG_INFO(Render,
                         "D3D12: ASTC verify block0 GPU {:02X} {:02X} {:02X} {:02X} {:02X} "
                         "{:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} "
                         "{:02X} {:02X}; CPU {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} "
                         "{:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X}",
                         gpu_bc3[0], gpu_bc3[1], gpu_bc3[2], gpu_bc3[3], gpu_bc3[4], gpu_bc3[5],
                         gpu_bc3[6], gpu_bc3[7], gpu_bc3[8], gpu_bc3[9], gpu_bc3[10], gpu_bc3[11],
                         gpu_bc3[12], gpu_bc3[13], gpu_bc3[14], gpu_bc3[15], expected[0],
                         expected[1], expected[2], expected[3], expected[4], expected[5],
                         expected[6], expected[7], expected[8], expected[9], expected[10],
                         expected[11], expected[12], expected[13], expected[14], expected[15]);
                FreeDeferredStagingBuffer(readback);
                FreeDeferredStagingBuffer(rgba_readback);
            }
            image.Transition(D3D12_RESOURCE_STATE_COPY_DEST);
            const D3D12_TEXTURE_COPY_LOCATION src{
                .pResource = astc_bc3_scratch.Get(),
                .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
                .PlacedFootprint = {
                    .Offset = 0,
                    .Footprint = {.Format = image.FootprintFormat(),
                                  .Width = Common::AlignUp(mip_width, 4U),
                                  .Height = Common::AlignUp(rows, 4U),
                                  .Depth = 1,
                                  .RowPitch = bc3_row_pitch},
                },
            };
            const D3D12_TEXTURE_COPY_LOCATION dst{
                .pResource = image.Handle(),
                .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
                .SubresourceIndex = image.Subresource(swizzle.level, 0),
            };
            commands->CopyTextureRegion(&dst, 0, first_y, 0, &src, nullptr);
        }
        view_descriptors.Free(source_srv);
    }
    if (gpu_astc_sync.load(std::memory_order_relaxed)) {
        scheduler.Finish();
    }
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
    VideoCore::Perf::LapTimer view_timer;
    // Views of a decoded block-compressed array read its plain texels (a view in another format
    // reinterprets the same resource format).
    FormatInfo format_info = runtime->Format(info.format);
    if (source.IsBcDecoded()) {
        format_info = DecodedBcFormat(info.format).value_or(source.TransferFormat());
    } else if (VideoCore::Surface::IsPixelFormatASTC(info.format)) {
        // ASTC arrays may be RGBA8 where the rest is recompressed: as the image decided.
        format_info = AstcFormat(info.format, VideoCommon::AstcRecompressionFor(source.info));
    }
    auto swizzle = info.Swizzle();
    if (const SurfaceType surface_type = VideoCore::Surface::GetFormatType(info.format);
        surface_type == SurfaceType::Depth || surface_type == SurfaceType::DepthStencil) {
        // As the Vulkan backend (ConvertGreenRed): guest depth views read depth from G, the
        // host SRV has it in R and G reads 0 (linear-depth passes then output 0 and lighting
        // divides by it: NaN/Inf, the black silhouettes of Mario Wonder).
        std::ranges::transform(swizzle, swizzle.begin(), [](Tegra::Texture::SwizzleSource s) {
            return s == Tegra::Texture::SwizzleSource::G ? Tegra::Texture::SwizzleSource::R : s;
        });
    }
    const D3D12_RESOURCE_DESC resource_desc = image->GetDesc();
    const u32 resource_levels = resource_desc.MipLevels;
    const u32 resource_array = resource_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
                                   ? 1U
                                   : resource_desc.DepthOrArraySize;
    // A view past the end of its resource is undefined in D3D12: the PC's driver tolerates it,
    // AMD's may build a descriptor that addresses the wrong memory. Clamp it (and say so).
    const u32 base_level = std::min(static_cast<u32>(info.range.base.level), resource_levels - 1);
    const u32 base_layer = std::min(static_cast<u32>(info.range.base.layer), resource_array - 1);
    const u32 levels = std::clamp(static_cast<u32>(info.range.extent.levels), 1U,
                                  resource_levels - base_level);
    const u32 layers = std::clamp(static_cast<u32>(info.range.extent.layers), 1U,
                                  resource_array - base_layer);
    if (base_level != static_cast<u32>(info.range.base.level) ||
        base_layer != static_cast<u32>(info.range.base.layer) ||
        levels != static_cast<u32>(info.range.extent.levels) ||
        layers != static_cast<u32>(info.range.extent.layers)) {
        static std::atomic<u32> logged_clamps{};
        if (logged_clamps.fetch_add(1, std::memory_order_relaxed) < 16) {
            LOG_WARNING(Render, "D3D12: view of {} @{:x} (levels {}+{} layers {}+{}) exceeds its "
                        "image ({} levels, {} layers); clamped",
                        info.format, source.gpu_addr, info.range.base.level,
                        info.range.extent.levels, info.range.base.layer,
                        info.range.extent.layers, resource_levels, resource_array);
        }
    }
    view_timer.Lap(VideoCore::Perf::Counter::TextureCacheViewSetupNs);
    const bool is_msaa = source.info.num_samples > 1;
    // The guest views an image in any format of the same size, and Vulkan allows it (mutable
    // format). D3D12 needs the view in the resource's typeless family: a view outside it is an
    // invalid call, which the PC's driver tolerates and the Series answers by removing the
    // device (an R16G16_FLOAT SRV in Mario Wonder, 0.2.56-0.2.57). Such views read the image in
    // its own format instead: maybe wrong colors on one texture, but the device lives.
    const DXGI_FORMAT resource_family = TypelessFamily(resource_desc.Format);
    const FormatInfo& own_format = source.TransferFormat();
    srv_resource = image;
    if (const DXGI_FORMAT view_family = TypelessFamily(format_info.srv);
        view_family != resource_family) {
        // Same texel size and plain texels: read a copy in the view's family, refreshed before
        // each read (PrepareRead). Otherwise read the image in its own format.
        using namespace VideoCore::Surface;
        const bool plain = DefaultBlockWidth(info.format) == 1 &&
                           DefaultBlockHeight(info.format) == 1 &&
                           DefaultBlockWidth(source.info.format) == 1 &&
                           DefaultBlockHeight(source.info.format) == 1 &&
                           GetFormatType(source.info.format) != SurfaceType::DepthStencil;
        ID3D12Resource* const copy =
            plain && BytesPerBlock(info.format) == BytesPerBlock(source.info.format)
                ? source.Reinterpreted(view_family)
                : nullptr;
        if (copy) {
            srv_resource = copy;
        } else {
            WarnOnce(logged_view_family, "view of {} (DXGI {}) on an image of {} (DXGI {}) is "
                     "not castable in D3D12; sampling it as DXGI {}",
                     info.format, static_cast<u32>(format_info.srv), source.info.format,
                     static_cast<u32>(resource_desc.Format), static_cast<u32>(own_format.srv));
            format_info.srv = own_format.srv;
        }
    }
    // Render targets follow the SRVs: on the copy when there is one in the view's family.
    const bool view_on_copy = srv_resource != image &&
                              TypelessFamily(format_info.view) == TypelessFamily(format_info.srv);
    if (!view_on_copy && TypelessFamily(format_info.view) != resource_family) {
        format_info.view = own_format.view;
    }
    view_timer.Lap(VideoCore::Perf::Counter::TextureCacheViewReinterpretNs);

    srv_params = {
        .format = format_info.srv,
        // Render-target views (blits, the display) carry no swizzle: sample them as stored, as
        // the Vulkan backend does.
        .mapping = info.IsRenderTarget()
                       ? D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING
                       : D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
                             Component(swizzle[0]), Component(swizzle[1]),
                             Component(swizzle[2]), Component(swizzle[3])),
        .base_level = base_level,
        .levels = levels,
        .base_layer = base_layer,
        .layers = layers,
        .is_msaa = is_msaa,
        .dimension = resource_desc.Dimension,
        .resource_layers = resource_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
                               ? 1U
                               : resource_desc.DepthOrArraySize,
    };
    switch (info.type) {
    case ImageViewType::e1D: natural_type = Shader::TextureType::Color1D; break;
    case ImageViewType::e1DArray: natural_type = Shader::TextureType::ColorArray1D; break;
    case ImageViewType::e2DArray: natural_type = Shader::TextureType::ColorArray2D; break;
    case ImageViewType::e3D: natural_type = Shader::TextureType::Color3D; break;
    case ImageViewType::Cube: natural_type = Shader::TextureType::ColorCube; break;
    case ImageViewType::CubeArray: natural_type = Shader::TextureType::ColorArrayCube; break;
    default: natural_type = Shader::TextureType::Color2D; break;
    }
    // A render-target view often exists only to be written. Creating its SRV eagerly made loading
    // frames issue hundreds of unnecessary device calls. Handle() creates this same natural SRV
    // on first shader read, as it already does for every alternate texture type.
    if (!info.IsRenderTarget()) {
        srvs[static_cast<size_t>(natural_type)] = CreateSrv(natural_type);
    }
    view_timer.Lap(VideoCore::Perf::Counter::TextureCacheViewSrvNs);

    const SurfaceType surface = VideoCore::Surface::GetFormatType(info.format);
    const bool is_3d = resource_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    // The underlying image may allow rendering, but most views of it are shader-only. Creating an
    // RTV/DSV for every such view made texture-heavy loading frames issue hundreds of needless
    // device calls. The generic cache marks attachment views with the render-target swizzle (the
    // same distinction used by Vulkan), so only those need CPU render descriptors.
    if (info.IsRenderTarget() && surface == SurfaceType::ColorTexture &&
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
            rtv_on_copy = view_on_copy;
            runtime->device.Get()->CreateRenderTargetView(view_on_copy ? srv_resource : image,
                                                          &rtv_desc, rtv);
            CheckRemovedAfterDescriptor(runtime->device.Get(), [&] {
                return fmt::format("RTV of {} (format {} dim {} level {} layers {}+{})",
                                   info.format, static_cast<u32>(rtv_desc.Format),
                                   static_cast<u32>(rtv_desc.ViewDimension), base_level,
                                   base_layer, layers);
            });
        } else {
            WarnOnce(logged_view_format, "view format {} cannot be a render target", info.format);
        }
    }
    if (info.IsRenderTarget() && surface != SurfaceType::ColorTexture &&
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
        // For draws that also sample this image: D3D12 cannot read a resource in DEPTH_WRITE.
        dsv_read_only = runtime->dsv_descriptors.Allocate();
        dsv_desc.Flags = D3D12_DSV_FLAG_READ_ONLY_DEPTH;
        if (format_info.stencil_srv != DXGI_FORMAT_UNKNOWN) {
            dsv_desc.Flags |= D3D12_DSV_FLAG_READ_ONLY_STENCIL;
        }
        runtime->device.Get()->CreateDepthStencilView(image, &dsv_desc, dsv_read_only);
        CheckRemovedAfterDescriptor(runtime->device.Get(), [&] {
            return fmt::format("DSV of {} (format {} dim {} level {} layers {}+{})", info.format,
                               static_cast<u32>(dsv_desc.Format),
                               static_cast<u32>(dsv_desc.ViewDimension), base_level, base_layer,
                               layers);
        });
    }
    // The UAV stays on the image (the copy has no UAV flag), so it needs the image's family.
    const DXGI_FORMAT uav_format = view_on_copy ? own_format.view : format_info.view;
    if ((resource_desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) &&
        runtime->SupportsView(uav_format, D3D12_FORMAT_SUPPORT1_NONE,
                              D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE)) {
        uav = runtime->view_descriptors.Allocate();
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc{.Format = uav_format};
        if (is_3d) {
            uav_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
            uav_desc.Texture3D = {base_level, 0, static_cast<u32>(-1)};
        } else {
            uav_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
            uav_desc.Texture2DArray = {base_level, base_layer, layers, 0};
        }
        runtime->device.Get()->CreateUnorderedAccessView(image, nullptr, &uav_desc, uav);
        CheckRemovedAfterDescriptor(runtime->device.Get(), [&] {
            return fmt::format("UAV of {} (format {} dim {} level {} layers {}+{})", info.format,
                               static_cast<u32>(uav_desc.Format),
                               static_cast<u32>(uav_desc.ViewDimension), base_level, base_layer,
                               layers);
        });
    }
    view_timer.Lap(VideoCore::Perf::Counter::TextureCacheViewAttachmentNs);
}
ImageView::ImageView(TextureCacheRuntime& runtime, const VideoCommon::ImageViewInfo& info,
                     ImageId id, Image& image_, SlotVector<Image>& images)
    : ImageView{runtime, info, id, image_} {
    slot_images = &images;
    if (natural_type == Shader::TextureType::ColorArray2D &&
        srv_params.dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D) {
        // Before slot_images was available, CreateSrv fell back to the 3D SRV and put the same
        // handle in both slots. Keep Color3D as its owner, but leave ColorArray2D empty so Handle()
        // creates the slice-array SRV only if a shader actually reads this view as a 2D array.
        srvs[static_cast<size_t>(natural_type)] = {};
    }
}
ImageView::ImageView(TextureCacheRuntime&, const VideoCommon::ImageInfo& info,
                     const VideoCommon::ImageViewInfo& view, GPUVAddr addr)
    : VideoCommon::ImageViewBase{info, view, addr}, buffer_size{VideoCommon::CalculateGuestSizeInBytes(info)} {}
ImageView::ImageView(TextureCacheRuntime& runtime_, const VideoCommon::NullImageViewParams& params)
    : VideoCommon::ImageViewBase{params}, runtime{&runtime_} {
    // Null SRVs of every type (dimension must still match the shader), created on demand.
    srv_params.format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvs[static_cast<size_t>(natural_type)] = CreateSrv(natural_type);
    uav = runtime->view_descriptors.Allocate();
    const D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc{
        .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
        .ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D,
        .Texture2D = {.MipSlice = 0, .PlaneSlice = 0},
    };
    runtime->device.Get()->CreateUnorderedAccessView(nullptr, nullptr, &uav_desc, uav);
}
ImageView::~ImageView() { Release(); }

D3D12_CPU_DESCRIPTOR_HANDLE ImageView::Handle(Shader::TextureType texture_type) const noexcept {
    const size_t index = static_cast<size_t>(texture_type);
    if (index >= srvs.size() || !runtime) {
        return {};
    }
    if (srvs[index].ptr == 0) {
        srvs[index] = CreateSrv(texture_type);
    }
    return srvs[index];
}

D3D12_CPU_DESCRIPTOR_HANDLE ImageView::DepthFeedbackHandle(Shader::TextureType texture_type) const {
    Image* const source = SourceImage();
    if (!source || !source->DepthFeedback()) {
        return Handle(texture_type);
    }
    if (!depth_feedback_srvs) {
        depth_feedback_srvs = std::make_unique<
            std::array<D3D12_CPU_DESCRIPTOR_HANDLE, Shader::NUM_TEXTURE_TYPES>>();
    }
    auto& handle = depth_feedback_srvs->at(static_cast<size_t>(texture_type));
    if (!handle.ptr) {
        handle = CreateSrv(texture_type, source->DepthFeedback()->Handle());
    }
    return handle;
}

D3D12_CPU_DESCRIPTOR_HANDLE ImageView::CreateSrv(Shader::TextureType texture_type,
                                               ID3D12Resource* override_resource) const {
    using Shader::TextureType;
    const SrvParams& p = srv_params;
    // A type the resource cannot be viewed as (a 2D texture read as 3D, a cube from fewer than six
    // layers) falls back to the view's own dimension, or to the resource's when that one does not
    // fit either (a 2D view of a 3D texture, a cube view of a 2D texture with fewer than six
    // layers): the shader reads garbage, not a bad view. Creating an SRV whose dimension the
    // resource cannot have removes the device.
    const bool is_1d = p.dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D;
    const bool is_3d = p.dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    const auto is_compatible = [&](TextureType candidate) {
        switch (candidate) {
        case TextureType::Color1D:
        case TextureType::ColorArray1D:
            return !image || is_1d;
        case TextureType::Color3D:
            return !image || is_3d;
        case TextureType::ColorCube:
        case TextureType::ColorArrayCube:
            return !image || (!is_1d && !is_3d && !p.is_msaa && p.resource_layers >= 6);
        case TextureType::Buffer:
            return false;
        default:
            return !image || (!is_1d && !is_3d);
        }
    };
    if (texture_type == TextureType::ColorArray2D && is_3d && image) {
        // Mario Wonder samples a 3D texture as a 2D array (Vulkan's 2D_ARRAY_COMPATIBLE); D3D12
        // has no such view, and the 3D SRV this fell back to made the Series write NaN (the black
        // silhouettes; the PC's driver happened to read it right). View a 2D array copy instead.
        Image* const source = SourceImage();
        if (ID3D12Resource* const slices = source ? source->SliceArray() : nullptr) {
            const u32 depth = slices->GetDesc().DepthOrArraySize;
            D3D12_SHADER_RESOURCE_VIEW_DESC desc{.Format = p.format,
                                                 .ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY,
                                                 .Shader4ComponentMapping = p.mapping};
            desc.Texture2DArray = {p.base_level, p.levels, 0, depth, 0, 0.0f};
            const D3D12_CPU_DESCRIPTOR_HANDLE handle = runtime->view_descriptors.Allocate();
            runtime->device.Get()->CreateShaderResourceView(slices, &desc, handle);
            CheckRemovedAfterDescriptor(runtime->device.Get(), [&] {
                return fmt::format("slice array SRV of {} (format {} levels {}+{} of {}, "
                                   "slices {}, resource format {})",
                                   format, static_cast<u32>(desc.Format), p.base_level, p.levels,
                                   slices->GetDesc().MipLevels, depth,
                                   static_cast<u32>(slices->GetDesc().Format));
            });
            return handle;
        }
    }
    if (!is_compatible(texture_type)) {
        const bool slices_pending = texture_type == TextureType::ColorArray2D && is_3d && image &&
                                    !slot_images; // replaced once the view knows its image
        if (!slices_pending) WarnOnce(logged_srv_fallback, "SRV of {} as texture type {} is not possible on a resource of "
                 "dimension {}; reading it with its own dimension", format,
                 static_cast<u32>(texture_type), static_cast<u32>(p.dimension));
        const TextureType resource_type = is_3d   ? TextureType::Color3D
                                          : is_1d ? TextureType::ColorArray1D
                                                  : TextureType::ColorArray2D;
        const TextureType fallback = is_compatible(natural_type) ? natural_type : resource_type;
        return override_resource ? CreateSrv(fallback, override_resource) : Handle(fallback);
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC desc{.Format = p.format,
                                         .Shader4ComponentMapping = p.mapping};
    const u32 cubes = std::max(1U, p.layers / 6);
    switch (texture_type) {
    case TextureType::Color1D:
        if (p.base_layer == 0) {
            desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1D;
            desc.Texture1D = {p.base_level, p.levels, 0.0f};
        } else {
            desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1DARRAY;
            desc.Texture1DArray = {p.base_level, p.levels, p.base_layer, 1, 0.0f};
        }
        break;
    case TextureType::ColorArray1D:
        desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1DARRAY;
        desc.Texture1DArray = {p.base_level, p.levels, p.base_layer, p.layers, 0.0f};
        break;
    case TextureType::Color3D:
        desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
        desc.Texture3D = {p.base_level, p.levels, 0.0f};
        break;
    case TextureType::ColorCube:
        if (p.base_layer == 0) {
            desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
            desc.TextureCube = {p.base_level, p.levels, 0.0f};
        } else {
            desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
            desc.TextureCubeArray = {p.base_level, p.levels, p.base_layer, 1, 0.0f};
        }
        break;
    case TextureType::ColorArrayCube:
        desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
        desc.TextureCubeArray = {p.base_level, p.levels, p.base_layer, cubes, 0.0f};
        break;
    case TextureType::ColorArray2D:
        if (p.is_msaa) {
            desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY;
            desc.Texture2DMSArray = {p.base_layer, p.layers};
        } else {
            desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            desc.Texture2DArray = {p.base_level, p.levels, p.base_layer, p.layers, 0, 0.0f};
        }
        break;
    default: // Color2D, Color2DRect (and a buffer read from an image view)
        if (p.base_layer != 0) {
            // A plain 2D SRV can only see layer 0: view the layer as a one-layer array.
            if (p.is_msaa) {
                desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY;
                desc.Texture2DMSArray = {p.base_layer, 1};
            } else {
                desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
                desc.Texture2DArray = {p.base_level, p.levels, p.base_layer, 1, 0, 0.0f};
            }
        } else if (p.is_msaa) {
            desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
        } else {
            desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            desc.Texture2D = {p.base_level, p.levels, 0, 0.0f};
        }
        break;
    }
    // The tripwire only names the first checked call after a removal: this one tells whether the
    // device was already gone before the SRV (the Series named the same R16G16_FLOAT SRV three
    // times, 0.2.56-0.2.57).
    CheckRemovedAfterDescriptor(runtime->device.Get(), [&] {
        return fmt::format("something unchecked before an SRV of {} as type {}", format,
                           static_cast<u32>(texture_type));
    });
    const D3D12_CPU_DESCRIPTOR_HANDLE handle = runtime->view_descriptors.Allocate();
    runtime->device.Get()->CreateShaderResourceView(override_resource ? override_resource :
                                                    srv_resource ? srv_resource : image, &desc,
                                                    handle);
    CheckRemovedAfterDescriptor(runtime->device.Get(), [&] {
        return fmt::format("SRV of {} as type {} (format {} dim {} levels {}+{} layers {}+{} of "
                           "{}, resource dim {} msaa {})",
                           format, static_cast<u32>(texture_type), static_cast<u32>(desc.Format),
                           static_cast<u32>(desc.ViewDimension), p.base_level, p.levels,
                           p.base_layer, p.layers, p.resource_layers,
                           static_cast<u32>(p.dimension), p.is_msaa);
    });
    return handle;
}

Image* ImageView::SourceImage() const noexcept {
    return slot_images && image ? &(*slot_images)[image_id] : nullptr;
}

void ImageView::TransitionImage(D3D12_RESOURCE_STATES state) const {
    if (slot_images && image) {
        // Reads through the reinterpreted copy leave the image alone: touching it would copy the
        // texels back and take the copy out of its readable state.
        if (srv_resource && srv_resource != image &&
            (state & D3D12_RESOURCE_STATE_UNORDERED_ACCESS) == 0) {
            (*slot_images)[image_id].ReadReinterpreted();
            return;
        }
        (*slot_images)[image_id].Transition(state);
    }
}

void ImageView::PrepareRender() const {
    if (Image* const source = SourceImage()) {
        if (rtv_on_copy) {
            source->RenderToReinterpreted();
        } else {
            source->Transition(D3D12_RESOURCE_STATE_RENDER_TARGET);
        }
    }
}

void ImageView::PrepareRead(Shader::TextureType texture_type) const {
    if (srv_resource && srv_resource != image) {
        if (Image* const source = SourceImage()) {
            source->ReadReinterpreted();
        }
    }
    if (texture_type == Shader::TextureType::ColorArray2D &&
        srv_params.dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D) {
        if (Image* const source = SourceImage()) {
            source->RefreshSliceArray();
        }
    }
}

void ImageView::Release() {
    if (!runtime) return;
    if (depth_feedback_srvs) {
        for (const auto handle : *depth_feedback_srvs) {
            if (handle.ptr) runtime->view_descriptors.Free(handle);
        }
        depth_feedback_srvs.reset();
    }
    for (D3D12_CPU_DESCRIPTOR_HANDLE& handle : srvs) {
        if (handle.ptr) runtime->view_descriptors.Free(handle);
        handle = {};
    }
    if (uav.ptr) runtime->view_descriptors.Free(uav);
    if (rtv.ptr) runtime->rtv_descriptors.Free(rtv);
    if (dsv.ptr) runtime->dsv_descriptors.Free(dsv);
    if (dsv_read_only.ptr) runtime->dsv_descriptors.Free(dsv_read_only);
    runtime = nullptr;
}
ImageView::ImageView(ImageView&& other) noexcept : VideoCommon::ImageViewBase{std::move(other)}, runtime{std::exchange(other.runtime, nullptr)},
    slot_images{other.slot_images}, image{other.image}, srv_resource{other.srv_resource}, srv_params{other.srv_params}, natural_type{other.natural_type},
    srvs{std::exchange(other.srvs, {})}, depth_feedback_srvs{std::move(other.depth_feedback_srvs)}, uav{other.uav}, rtv{other.rtv}, rtv_on_copy{other.rtv_on_copy}, dsv{other.dsv}, dsv_read_only{other.dsv_read_only}, buffer_size{other.buffer_size} {}
ImageView& ImageView::operator=(ImageView&& other) noexcept {
    if (this != &other) { Release(); static_cast<VideoCommon::ImageViewBase&>(*this) = std::move(other);
        runtime = std::exchange(other.runtime, nullptr); slot_images = other.slot_images; image = other.image; srv_resource = other.srv_resource;
        srv_params = other.srv_params; natural_type = other.natural_type; srvs = std::exchange(other.srvs, {});
        depth_feedback_srvs = std::move(other.depth_feedback_srvs);
        uav = other.uav; rtv = other.rtv; rtv_on_copy = other.rtv_on_copy; dsv = other.dsv; dsv_read_only = other.dsv_read_only; buffer_size = other.buffer_size; }
    return *this;
}
bool ImageView::IsRescaled() const noexcept { return slot_images && (*slot_images)[image_id].IsRescaled(); }

Sampler::Sampler(TextureCacheRuntime& runtime_, const Tegra::Texture::TSCEntry& config)
    : runtime{&runtime_}, key{next_sampler_key.fetch_add(1, std::memory_order_relaxed)} {
    handle = runtime->sampler_descriptors.Allocate();
    const bool linear_min = config.min_filter == Tegra::Texture::TextureFilter::Linear;
    const bool linear_mag = config.mag_filter == Tegra::Texture::TextureFilter::Linear;
    const bool linear_mip = config.mipmap_filter == Tegra::Texture::TextureMipmapFilter::Linear;
    const f32 anisotropy = std::clamp(config.MaxAnisotropy(), 1.0f, 16.0f);
    D3D12_FILTER_REDUCTION_TYPE reduction = D3D12_FILTER_REDUCTION_TYPE_STANDARD;
    if (config.depth_compare_enabled) {
        reduction = D3D12_FILTER_REDUCTION_TYPE_COMPARISON;
    } else if (config.reduction_filter == Tegra::Texture::SamplerReduction::Min) {
        reduction = D3D12_FILTER_REDUCTION_TYPE_MINIMUM;
    } else if (config.reduction_filter == Tegra::Texture::SamplerReduction::Max) {
        reduction = D3D12_FILTER_REDUCTION_TYPE_MAXIMUM;
    }
    const bool min_max = reduction == D3D12_FILTER_REDUCTION_TYPE_MINIMUM ||
                         reduction == D3D12_FILTER_REDUCTION_TYPE_MAXIMUM;
    if (min_max) {
        // D3D's reduction footprint contains only texels with non-zero weights. With point
        // min/mag/mip and no anisotropy it contains exactly one texel: min(x) == max(x) == x.
        // Canonicalize on every host, so Xbox needs neither tier-2 samplers nor shader work.
        if (!linear_min && !linear_mag && !linear_mip && anisotropy == 1.0f) {
            reduction = D3D12_FILTER_REDUCTION_TYPE_STANDARD;
            if (!logged_point_reduction) {
                logged_point_reduction = true;
                LOG_INFO(Render, "D3D12: point MIN/MAX sampler uses its exact single-texel "
                                 "equivalent (no reduction hardware required)");
            }
        } else if (!runtime->supports_min_max_filter) {
            WarnOnce(logged_min_max_filter, "filtered min/max sampler reduction is not supported "
                                            "by this device; filtering normally instead");
            reduction = D3D12_FILTER_REDUCTION_TYPE_STANDARD;
        }
    }
    filter = anisotropy > 1.0f ? D3D12_ENCODE_ANISOTROPIC_FILTER(reduction) :
        D3D12_ENCODE_BASIC_FILTER(linear_min ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT,
        linear_mag ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT,
        linear_mip ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT,
        reduction);
    const auto border = config.BorderColor();
    // D3D12 leaves MinLOD > MaxLOD and biases outside [-16, 15.99] undefined; drivers differ.
    const bool no_mips = config.mipmap_filter == Tegra::Texture::TextureMipmapFilter::None;
    const f32 min_lod = no_mips ? 0.0f : config.MinLod();
    const f32 max_lod = no_mips ? 0.25f : std::max(config.MaxLod(), min_lod);
    desc = D3D12_SAMPLER_DESC{.Filter = filter, .AddressU = AddressMode(config.wrap_u),
        .AddressV = AddressMode(config.wrap_v), .AddressW = AddressMode(config.wrap_p),
        .MipLODBias = std::clamp(config.LodBias(), D3D12_MIP_LOD_BIAS_MIN, D3D12_MIP_LOD_BIAS_MAX),
        .MaxAnisotropy = static_cast<u32>(anisotropy),
        .ComparisonFunc = Compare(config.depth_compare_func), .BorderColor = {border[0], border[1], border[2], border[3]},
        .MinLOD = min_lod, .MaxLOD = max_lod};
    runtime->device.Get()->CreateSampler(&desc, handle);
    CheckRemovedAfter(runtime->device.Get(), [&] {
        return fmt::format("sampler (filter 0x{:x} address {}/{}/{} aniso {} compare {} lod {}..{} "
                           "bias {})",
                           static_cast<u32>(desc.Filter), static_cast<u32>(desc.AddressU),
                           static_cast<u32>(desc.AddressV), static_cast<u32>(desc.AddressW),
                           desc.MaxAnisotropy, static_cast<u32>(desc.ComparisonFunc), desc.MinLOD,
                           desc.MaxLOD, desc.MipLODBias);
    });
}
std::string Sampler::Describe() const {
    return fmt::format("filter 0x{:x} address {}/{}/{} aniso {} lod {}..{} bias {}",
                       static_cast<u32>(desc.Filter), static_cast<u32>(desc.AddressU),
                       static_cast<u32>(desc.AddressV), static_cast<u32>(desc.AddressW),
                       desc.MaxAnisotropy, desc.MinLOD, desc.MaxLOD, desc.MipLODBias);
}
Sampler::~Sampler() { if (runtime && handle.ptr) runtime->sampler_descriptors.Free(handle); }
Sampler::Sampler(Sampler&& other) noexcept : runtime{std::exchange(other.runtime, nullptr)}, handle{other.handle}, key{other.key}, filter{other.filter}, desc{other.desc} {}
Sampler& Sampler::operator=(Sampler&& other) noexcept {
    if (this != &other) { if (runtime && handle.ptr) runtime->sampler_descriptors.Free(handle);
        runtime = std::exchange(other.runtime, nullptr); handle = other.handle; key = other.key; filter = other.filter; desc = other.desc; } return *this;
}

Framebuffer::Framebuffer(TextureCacheRuntime& runtime, std::span<ImageView*, NUM_RT> color_buffers,
                         ImageView* depth_buffer, const VideoCommon::RenderTargets& key)
    : extent{key.size}, is_rescaled{key.is_rescaled} {
    // Slot i is guest render target i, as in the Vulkan render pass and the pipeline's RTVFormats
    // (fragment output i writes render target i; rt_control's mapping is not applied).
    colors.fill(runtime.NullRenderTarget());
    for (size_t index = 0; index < color_buffers.size(); ++index) {
        ImageView* const view = color_buffers[index];
        if (!view) continue;
        if (!view->RenderTarget().ptr) {
            missing_colors |= 1U << index;
            WarnOnce(logged_missing_rtv, "render target {} (format {}) has no RTV; draws to it "
                     "are lost", index, view->format);
            continue;
        }
        colors[index] = view->RenderTarget();
        color_images[index] = view->image_id;
        if (view->RenderTargetOnCopy()) {
            copy_colors |= 1U << index;
        }
        color_formats[index] = runtime.Format(view->format).view;
        images = view->slot_images ? view->slot_images : images;
        num_colors = static_cast<u32>(index) + 1;
        if (const Image* const image = view->SourceImage()) {
            samples = std::max(1U, image->info.num_samples);
        }
    }
    // D3D12 only binds a depth view at least as large as every render target; drivers accept a
    // smaller one (Vulkan draws in the intersection), but the debug layer invalidates the command
    // list. With the debug layer (PC), such a pass is drawn without depth so the run goes on.
    bool depth_too_small = false;
    if (depth_buffer && Settings::values.renderer_debug.GetValue()) {
        for (size_t index = 0; index < num_colors; ++index) {
            const ImageView* const view = color_buffers[index];
            if (view && (view->size.width > depth_buffer->size.width ||
                         view->size.height > depth_buffer->size.height)) {
                depth_too_small = true;
            }
        }
        if (depth_too_small) {
            WarnOnce(logged_depth_size, "depth buffer smaller than a render target ({}x{}); "
                     "drawn without depth under the debug layer", depth_buffer->size.width,
                     depth_buffer->size.height);
        }
    }
    if (depth_buffer && depth_buffer->DepthStencil().ptr && !depth_too_small) {
        depth = depth_buffer->DepthStencil();
        depth_read_only = depth_buffer->DepthStencilReadOnly();
        depth_image = depth_buffer->image_id;
        depth_format = runtime.Format(depth_buffer->format).dsv;
        images = depth_buffer->slot_images ? depth_buffer->slot_images : images;
        has_stencil = VideoCore::Surface::GetFormatType(depth_buffer->format) ==
                      SurfaceType::DepthStencil;
        if (const Image* const image = depth_buffer->SourceImage()) {
            samples = std::max(1U, image->info.num_samples);
        }
    }
}

void Framebuffer::PrepareAttachments(bool depth_sampled) const {
    if (!images) return;
    for (size_t index = 0; index < num_colors; ++index) {
        if (color_images[index] == ImageId{}) {
            continue;
        }
        Image& image = (*images)[color_images[index]];
        if ((copy_colors >> index) & 1) {
            image.RenderToReinterpreted();
        } else {
            image.Transition(D3D12_RESOURCE_STATE_RENDER_TARGET);
        }
    }
    if (depth_image != ImageId{}) {
        (*images)[depth_image].Transition(depth_sampled ? DEPTH_SAMPLED_STATE
                                                        : D3D12_RESOURCE_STATE_DEPTH_WRITE);
    }
}

const Image* Framebuffer::ColorImage(size_t index) const noexcept {
    if (!images || index >= NUM_RT || color_images[index] == ImageId{}) return nullptr;
    return &(*images)[color_images[index]];
}

const Image* Framebuffer::DepthImage() const noexcept {
    if (!images || depth_image == ImageId{}) return nullptr;
    return &(*images)[depth_image];
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
        // Exercise the maintenance path with actual GPU copies, including ownership transfer
        // and writes between recording and consumption. Finish is used ONLY by this test gate.
        image.flags &= ~VideoCommon::ImageFlagBits::CpuModified;
        StagingBufferRef gc_map{};
        if (PrepareGcDownload(image, std::span{&copy, 1}, gc_map))
            throw std::runtime_error{"GC readback was not deferred"};
        Image moved{std::move(image)};
        image = std::move(moved);
        Finish();
        if (!PrepareGcDownload(image, std::span{&copy, 1}, gc_map) ||
            !std::equal(upload.mapped_span.begin(), upload.mapped_span.begin() + bytes,
                        gc_map.mapped_span.begin()))
            throw std::runtime_error{"GC deferred readback/move mismatch"};
        CompleteGcDownload(image);
        if (gc_pending_bytes != 0) throw std::runtime_error{"GC move leaked staging"};

        if (PrepareGcDownload(image, std::span{&copy, 1}, gc_map))
            throw std::runtime_error{"GC stale test was not deferred"};
        Finish();
        auto newer_upload = UploadStagingBuffer(bytes);
        for (u32 i = 0; i < bytes; ++i)
            newer_upload.mapped_span[i] = static_cast<u8>((i * 13 + 11) & 0xff);
        image.UploadMemory(newer_upload, std::span{&copy, 1});
        if (PrepareGcDownload(image, std::span{&copy, 1}, gc_map))
            throw std::runtime_error{"GC accepted stale GPU bytes"};
        Finish();
        if (!PrepareGcDownload(image, std::span{&copy, 1}, gc_map) ||
            !std::equal(newer_upload.mapped_span.begin(), newer_upload.mapped_span.begin() + bytes,
                        gc_map.mapped_span.begin()))
            throw std::runtime_error{"GC refreshed GPU bytes mismatch"};
        CompleteGcDownload(image);
        PrepareGcDownload(image, std::span{&copy, 1}, gc_map);
        image.flags |= VideoCommon::ImageFlagBits::CpuModified;
        if (PrepareGcDownload(image, std::span{&copy, 1}, gc_map) || gc_pending_bytes != 0)
            throw std::runtime_error{"GC accepted stale CPU bytes or retained staging"};
        image.flags &= ~VideoCommon::ImageFlagBits::CpuModified;
        Finish();
        // Fill the pending budget, prove another image cannot grow it, then discard in flight.
        {
            Image budget_image{*this, info, 0, 0};
            budget_image.flags &= ~VideoCommon::ImageFlagBits::CpuModified;
            budget_image.UploadMemory(newer_upload, std::span{&copy, 1});
            budget_image.unswizzled_size_bytes = 8U * 1024 * 1024;
            if (PrepareGcDownload(budget_image, std::span{&copy, 1}, gc_map) ||
                gc_pending_bytes != 8ULL * 1024 * 1024 ||
                PrepareGcDownload(image, std::span{&copy, 1}, gc_map) || image.gc_readback)
                throw std::runtime_error{"GC pending budget exceeded"};
        }
        if (gc_pending_bytes != 0) throw std::runtime_error{"GC discarded readback leaked"};
        Finish();
        const auto saved_pressure = pressure_snapshot;
        pressure_snapshot = {.app_used = 100ULL * 1024 * 1024,
                             .app_limit = 128ULL * 1024 * 1024};
        const bool recovered = PrepareGcDownload(image, std::span{&copy, 1}, gc_map);
        pressure_snapshot = saved_pressure;
        if (!recovered || gc_pending_bytes != 0 ||
            !std::equal(newer_upload.mapped_span.begin(), newer_upload.mapped_span.begin() + bytes,
                        gc_map.mapped_span.begin()))
            throw std::runtime_error{"GC emergency recovery mismatch"};
        CompleteGcDownload(image);
        LOG_INFO(Render, "D3D12: GC deferred readback gate passed (GPU data, moves, GPU/CPU stale rejection, 8 MiB cap, discard, emergency)");
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
        // A snapshot must preserve the previous depth even after the original is written again.
        const auto depth_dsv = dsv_descriptors.Allocate();
        const D3D12_DEPTH_STENCIL_VIEW_DESC depth_desc{
            .Format = DXGI_FORMAT_D32_FLOAT, .ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D};
        device.Get()->CreateDepthStencilView(depth_image.Handle(), &depth_desc, depth_dsv);
        depth_image.Transition(D3D12_RESOURCE_STATE_DEPTH_WRITE);
        scheduler.CommandList()->ClearDepthStencilView(depth_dsv, D3D12_CLEAR_FLAG_DEPTH,
                                                       0.25f, 0, 0, nullptr);
        Image* const snapshot = depth_image.PrepareDepthFeedback();
        if (!snapshot) throw std::runtime_error{"depth feedback allocation failed"};
        scheduler.CommandList()->ClearDepthStencilView(depth_dsv, D3D12_CLEAR_FLAG_DEPTH,
                                                       0.75f, 0, 0, nullptr);
        auto old_depth = DownloadStagingBuffer(width * height * sizeof(float), true);
        auto new_depth = DownloadStagingBuffer(width * height * sizeof(float), true);
        snapshot->DownloadMemory(old_depth, std::span{&copy, 1});
        depth_image.DownloadMemory(new_depth, std::span{&copy, 1});
        FreeDeferredStagingBuffer(old_depth);
        FreeDeferredStagingBuffer(new_depth);
        Finish();
        dsv_descriptors.Free(depth_dsv);
        // DownloadMemory repacks the aligned GPU footprint into the requested guest rows.
        const u32 depth_pitch = width * sizeof(float);
        for (u32 y = 0; y < height; ++y) {
            for (u32 x = 0; x < width; ++x) {
                float before{}, after{};
                std::memcpy(&before, old_depth.mapped_span.data() + y * depth_pitch + x * 4, 4);
                std::memcpy(&after, new_depth.mapped_span.data() + y * depth_pitch + x * 4, 4);
                if (before != 0.25f || after != 0.75f)
                    throw std::runtime_error{"depth feedback contents mismatch"};
            }
        }
        LOG_INFO(Render, "D3D12: depth feedback round-trip passed (snapshot 0.25, original 0.75)");
        ImageView null_view{*this, VideoCommon::NullImageViewParams{}};
        Framebuffer framebuffer{*this, colors, &depth_view, targets};
        LOG_INFO(Render, "D3D12: texture cache round-trip passed ({}x{} RGBA8 unaligned rows, SRV/UAV/RTV/DSV, null views, sampler, framebuffer)", width, height);
    } catch (const std::exception& error) {
        LOG_ERROR(Render, "D3D12: texture cache self-test failed: {}", error.what());
    }
}

template class VideoCommon::TextureCache<TextureCacheParams>;

} // namespace D3D12
