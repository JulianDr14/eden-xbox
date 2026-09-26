// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <unordered_map>
#include <vector>

#include "common/common_types.h"
#include "video_core/renderer_d3d12/d3d12_device.h"
#include "video_core/texture_cache/types.h"

namespace D3D12 {

class CpuDescriptorAllocator;
class DescriptorRing;
class SamplerHeap;
class Scheduler;
class ShaderCompiler;

/// Draws Eden's host blit and clear shaders into guest images: the D3D12 counterpart of Vulkan's
/// BlitImageHelper. Serves scaled texture blits (Fermi2D), DrawTexture and the clears D3D12 cannot
/// express with ClearRenderTargetView/ClearDepthStencilView (partial color or stencil masks).
///
/// The caller transitions the images (sources to PIXEL_SHADER_RESOURCE, targets to RENDER_TARGET
/// or DEPTH_WRITE) and passes descriptors of them; the helper records a full-screen triangle with
/// its own root signature and PSO. Guest draws record their whole state, so nothing is restored.
class BlitImageHelper {
public:
    /// An offline sampler and the unique key SamplerHeap deduplicates tables by.
    struct Sampling {
        D3D12_CPU_DESCRIPTOR_HANDLE handle;
        u64 key;
    };

    /// A render target or depth-stencil view and what its PSO needs to know about it.
    struct Target {
        D3D12_CPU_DESCRIPTOR_HANDLE view;
        DXGI_FORMAT format; ///< the RTV or DSV format
        u32 samples;
    };

    BlitImageHelper(const Device& device, Scheduler& scheduler, const ShaderCompiler& compiler,
                    DescriptorRing& descriptor_ring, SamplerHeap& sampler_heap,
                    CpuDescriptorAllocator& sampler_descriptors);
    ~BlitImageHelper();

    BlitImageHelper(const BlitImageHelper&) = delete;
    BlitImageHelper& operator=(const BlitImageHelper&) = delete;

    /// False when the shader path is missing or a helper shader failed to translate (logged).
    [[nodiscard]] bool IsAvailable() const noexcept {
        return available;
    }

    [[nodiscard]] Sampling NearestSampler() const noexcept {
        return nearest_sampler;
    }
    [[nodiscard]] Sampling LinearSampler() const noexcept {
        return linear_sampler;
    }

    /// Draws src_region of a 2D color SRV (src_size texels) into dst_region of a color target.
    void BlitColor(const Target& dst, D3D12_CPU_DESCRIPTOR_HANDLE src_srv, const Sampling& sampler,
                   const VideoCommon::Region2D& dst_region,
                   const VideoCommon::Region2D& src_region, const VideoCommon::Extent2D& src_size);

    /// Same for depth: the source's depth (its SRV's red channel) becomes the target's depth.
    void BlitDepth(const Target& dst, D3D12_CPU_DESCRIPTOR_HANDLE src_srv,
                   const VideoCommon::Region2D& dst_region,
                   const VideoCommon::Region2D& src_region, const VideoCommon::Extent2D& src_size);

    /// Writes color into the channels of color_mask (bit 0 red ... bit 3 alpha) inside rect.
    void ClearColor(const Target& dst, u8 color_mask, const std::array<f32, 4>& color,
                    const D3D12_RECT& rect);

    /// Writes depth (when clear_depth) and the stencil bits of stencil_mask inside rect.
    void ClearDepthStencil(const Target& dst, bool clear_depth, f32 depth, u8 stencil_mask,
                           u8 stencil_value, const D3D12_RECT& rect);

private:
    enum class Kind : u8 {
        BlitColor,
        BlitDepth,
        ClearColor,
        ClearDepthStencil,
    };

    struct Program {
        std::vector<u8> vs;
        std::vector<u8> ps;
    };

    /// The PSO of one kind for one target format; mask is the color write mask or the stencil
    /// write mask, depth whether a depth-stencil clear writes depth.
    ID3D12PipelineState* Pipeline(Kind kind, const Target& target, u8 mask, bool depth);
    void CreateRootSignature();
    void CreateSamplers(CpuDescriptorAllocator& sampler_descriptors);
    /// Binds the root signature, the PSO, the target and the viewport and scissor of rect.
    void Begin(ID3D12PipelineState* pipeline, const Target& target, bool is_depth,
               const D3D12_RECT& rect);
    void Blit(Kind kind, const Target& dst, D3D12_CPU_DESCRIPTOR_HANDLE src_srv,
              const Sampling& sampler, const VideoCommon::Region2D& dst_region,
              const VideoCommon::Region2D& src_region, const VideoCommon::Extent2D& src_size);

    const Device& device;
    Scheduler& scheduler;
    DescriptorRing& descriptor_ring;
    SamplerHeap& sampler_heap;

    ComPtr<ID3D12RootSignature> root_signature;
    std::array<Program, 4> programs;
    std::unordered_map<u64, ComPtr<ID3D12PipelineState>> pipelines;
    Sampling nearest_sampler{};
    Sampling linear_sampler{};
    bool available{};
};

} // namespace D3D12
