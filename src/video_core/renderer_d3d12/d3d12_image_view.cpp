// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video_core/renderer_d3d12/d3d12_texture_cache_internal.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <utility>

#include <fmt/format.h>

#include "common/logging.h"
#include "video_core/perf_counters.h"
#include "video_core/renderer_d3d12/d3d12_barrier_batch.h"
#include "video_core/surface.h"
#include "video_core/texture_cache/util.h"

namespace D3D12 {

using namespace TextureDetail;

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
    switch (info.format) {
    case PixelFormat::B5G6R5_UNORM:
    case PixelFormat::A1B5G5R5_UNORM:
    case PixelFormat::A4B4G4R4_UNORM:
        // Their DXGI format (BaseFormat) has blue in the bits where these hold red: swap them
        // back when sampling, as the Vulkan backend does (TryTransformSwizzleIfNeeded).
        std::ranges::transform(swizzle, swizzle.begin(), [](Tegra::Texture::SwizzleSource s) {
            using enum Tegra::Texture::SwizzleSource;
            return s == R ? B : s == B ? R : s;
        });
        break;
    default:
        break;
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
        } else if (resource_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D) {
            // A 2D-array UAV on a 1D resource is an invalid call: the device is removed (an
            // R32_FLOAT 1D image in Super Mario 3D World).
            uav_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1DARRAY;
            uav_desc.Texture1DArray = {base_level, base_layer, layers};
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

void ImageView::TransitionImage(D3D12_RESOURCE_STATES state, BarrierBatch* batch) const {
    if (slot_images && image) {
        Image& source = (*slot_images)[image_id];
        // Reads through the reinterpreted copy leave the image alone: touching it would copy the
        // texels back and take the copy out of its readable state.
        if (srv_resource && srv_resource != image &&
            (state & D3D12_RESOURCE_STATE_UNORDERED_ACCESS) == 0) {
            if (batch) {
                batch->Flush();
            }
            source.ReadReinterpreted();
            return;
        }
        source.Transition(state, batch);
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

} // namespace D3D12
