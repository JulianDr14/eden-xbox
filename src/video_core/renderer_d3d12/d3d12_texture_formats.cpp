// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video_core/renderer_d3d12/d3d12_texture_cache_internal.h"

#include <optional>

#include "common/settings.h"
#include "video_core/renderer_d3d12/d3d12_blit_image.h"
#include "video_core/surface.h"
#include "video_core/texture_cache/decode_bc.h"

// DXGI formats of guest pixel formats and the Maxwell -> D3D12 sampler and view enums.

namespace D3D12 {

namespace TextureDetail {

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
/// docs/xbox/xbox_d3d12_phase4.md, 0.2.44). Plain texels are copied without reinterpreting blocks.
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

} // namespace TextureDetail

FormatInfo SurfaceFormat(VideoCore::Surface::PixelFormat format) {
    return TextureDetail::NativeFormat(format);
}

} // namespace D3D12
