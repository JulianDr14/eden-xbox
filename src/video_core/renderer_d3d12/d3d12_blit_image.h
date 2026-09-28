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

/// How guest memory packs one depth-stencil texel (the layout_mode of the pack shaders). The
/// Maxwell names list the bits from the most significant (as Ryujinx and nouveau use them).
enum class DepthStencilLayout : u32 {
    Z24S8,      ///< S8_UINT_D24_UNORM: depth in bits 31:8, stencil in 7:0
    S8Z24,      ///< D24_UNORM_S8_UINT: stencil in bits 31:24, depth in 23:0
    X8Z24,      ///< X8_D24_UNORM: depth in bits 23:0, nothing in 31:24
    ZF32_X24S8, ///< D32_FLOAT_S8_UINT: the float depth, then a word with the stencil in 7:0
};

/// One region repacked between guest depth-stencil texels and the copy footprints of both planes
/// of a D3D12 depth-stencil subresource (depth: 32-bit words, stencil: bytes). Offsets, rows and
/// pitches are in 32-bit words; the region is in texels of the footprints.
struct DepthStencilPack {
    /// Read through a root SRV: the guest texels (split) or the footprints (merge). 4-byte aligned.
    D3D12_GPU_VIRTUAL_ADDRESS source;
    /// Written through a root UAV: the footprints (split) or the guest texels (merge).
    D3D12_GPU_VIRTUAL_ADDRESS destination;
    u32 packed_offset;  ///< first guest texel, from the packed buffer's address
    u32 packed_row;     ///< words per guest row
    u32 depth_pitch;    ///< words per depth footprint row
    u32 stencil_offset; ///< start of the stencil footprint, from the footprints' address
    u32 stencil_pitch;  ///< words per stencil footprint row
    u32 x;
    u32 y;
    u32 width;
    u32 height;
    u32 row_texels; ///< merge: guest texels per row (at least width; the rest is written as zero)
    DepthStencilLayout layout;
};

/// One band of a guest ASTC mip decoded on the GPU. Both descriptors are bounded so malformed
/// guest offsets cannot turn into an unbounded root-descriptor access.
struct AstcDecode {
    /// Raw R32_TYPELESS SRV containing the level's swizzled ASTC blocks.
    D3D12_CPU_DESCRIPTOR_HANDLE source;
    /// Offline UAV (R8G8B8A8_UNORM, Texture2DArray) of the level; copied into the ring.
    D3D12_CPU_DESCRIPTOR_HANDLE destination;
    u32 block_width;  ///< ASTC block size in texels
    u32 block_height;
    /// The block-linear layout (VideoCommon::Accelerated::BlockLinearSwizzle2DParams).
    u32 layer_stride;
    u32 block_size;
    u32 x_shift;
    u32 gob_block_height;
    u32 gob_block_height_mask;
    u32 blocks_x; ///< ASTC blocks per row and column of the level
    u32 blocks_y;
    u32 layers;
    u32 input_words;
    u32 first_block_row;
};

/// One horizontal RGBA8 band encoded as BC3 into a raw buffer footprint.
struct Bc3Encode {
    D3D12_CPU_DESCRIPTOR_HANDLE source;      ///< R8G8B8A8_UNORM Texture2D SRV
    D3D12_GPU_VIRTUAL_ADDRESS destination;   ///< raw buffer UAV
    u32 width;
    u32 height;
    u32 band_height;
    u32 output_row_words;
};

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

    /// False when the depth-stencil pack shaders are missing (logged): guest depth-stencil
    /// texels then cannot move to or from D3D12 depth-stencil resources.
    [[nodiscard]] bool CanPackDepthStencil() const noexcept {
        return pack_available;
    }
    /// Guest texels -> both footprints (d3d12_depth_stencil_split.comp). The caller puts the
    /// source in a shader-readable state and the footprints in UNORDERED_ACCESS.
    void SplitDepthStencil(const DepthStencilPack& pack);
    /// Both footprints -> guest texels (d3d12_depth_stencil_merge.comp), same states.
    void MergeDepthStencil(const DepthStencilPack& pack);

    /// False when the ASTC decoder failed to translate or build (logged): ASTC is then decoded
    /// on the CPU.
    [[nodiscard]] bool CanDecodeAstc() const noexcept {
        return astc_available;
    }
    [[nodiscard]] bool CanEncodeBc3() const noexcept {
        return bc3_available;
    }
    /// Records the decode of one level; the caller puts the image in UNORDERED_ACCESS.
    void DecodeAstc(const AstcDecode& decode);
    void EncodeBc3(const Bc3Encode& encode);

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
    /// Builds the pack root signature and both compute pipelines; throws on failure.
    void CreatePackPipelines(const ShaderCompiler& compiler);
    void DispatchPack(ID3D12PipelineState* pipeline, const DepthStencilPack& pack, u32 groups_x,
                      u32 groups_y);
    /// Builds the ASTC decoder's root signature and pipeline; throws on failure.
    void CreateAstcPipeline(const ShaderCompiler& compiler);
    void CreateBc3Pipeline(const ShaderCompiler& compiler);

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

    ComPtr<ID3D12RootSignature> pack_root_signature;
    ComPtr<ID3D12PipelineState> split_pipeline;
    ComPtr<ID3D12PipelineState> merge_pipeline;
    bool pack_available{};

    ComPtr<ID3D12RootSignature> astc_root_signature;
    ComPtr<ID3D12PipelineState> astc_pipeline;
    bool astc_available{};

    ComPtr<ID3D12RootSignature> bc3_root_signature;
    ComPtr<ID3D12PipelineState> bc3_pipeline;
    bool bc3_available{};
};

} // namespace D3D12
