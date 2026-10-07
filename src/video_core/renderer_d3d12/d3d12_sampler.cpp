// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video_core/renderer_d3d12/d3d12_texture_cache_internal.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <utility>

#include <fmt/format.h>

#include "common/logging.h"

namespace D3D12 {

using namespace TextureDetail;

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
eden_integer_sampler_state Sampler::IntegerState(u32 last_level) const {
    // Integer textures read the border color's raw bits, as Vulkan's custom border color does.
    eden_integer_sampler_state state{};
    for (size_t i = 0; i < 4; ++i) {
        state.border_color[i] = std::bit_cast<u32>(desc.BorderColor[i]);
    }
    state.lod_bias = desc.MipLODBias;
    state.min_lod = desc.MinLOD;
    state.max_lod = desc.MaxLOD;
    state.last_level = last_level;
    state.wrap[0] = static_cast<u32>(desc.AddressU);
    state.wrap[1] = static_cast<u32>(desc.AddressV);
    state.wrap[2] = static_cast<u32>(desc.AddressW);
    return state;
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

} // namespace D3D12
