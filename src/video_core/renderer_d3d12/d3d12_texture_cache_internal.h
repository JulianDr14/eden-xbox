// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Shared by the files implementing the texture cache runtime (d3d12_texture_cache.cpp,
// d3d12_texture_formats.cpp, d3d12_image_*.cpp, ...). Not meant for other users.

#include <atomic>
#include <cstdint>
#include <optional>
#include <utility>

#include <fmt/format.h>

#include "common/bug_tracker.h"
#include "common/settings.h"
#include "video_core/renderer_d3d12/d3d12_blit_image.h"
#include "video_core/renderer_d3d12/d3d12_log.h"
#include "video_core/renderer_d3d12/d3d12_resource_utils.h"
#include "video_core/renderer_d3d12/d3d12_texture_cache.h"
#include "video_core/surface.h"
#include "video_core/textures/texture.h"

namespace D3D12::TextureDetail {

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

// Format tables and Maxwell -> D3D12 enums (d3d12_texture_formats.cpp).
FormatInfo BaseFormat(PixelFormat format);
FormatInfo AstcFormat(PixelFormat format, Settings::AstcRecompression recompression);
FormatInfo NativeFormat(PixelFormat format);
std::optional<FormatInfo> DecodedBcFormat(PixelFormat format);
D3D12_RESOURCE_DIMENSION Dimension(ImageType type);
D3D12_TEXTURE_ADDRESS_MODE AddressMode(Tegra::Texture::WrapMode mode);
D3D12_COMPARISON_FUNC Compare(Tegra::Texture::DepthCompareFunc func);
u32 Component(Tegra::Texture::SwizzleSource source);
std::optional<DepthStencilLayout> GuestDepthStencilLayout(PixelFormat format);
DXGI_FORMAT TypelessFamily(DXGI_FORMAT format);
bool RequiredTypedUavStore(DXGI_FORMAT format);
bool RequiredRenderTarget(DXGI_FORMAT format);

inline ComPtr<ID3D12Resource> CreateTransferBuffer(ID3D12Device* device, u64 size,
                                                   bool unordered_access = false) {
    // Buffers start in COMMON and are promoted on first use.
    return CreateCommittedBuffer(device, size, D3D12_HEAP_TYPE_DEFAULT,
                                 D3D12_RESOURCE_STATE_COMMON,
                                 unordered_access ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
                                                  : D3D12_RESOURCE_FLAG_NONE,
                                 "Create texture transfer buffer");
}

inline void TransitionBuffer(ID3D12GraphicsCommandList* commands, ID3D12Resource* buffer,
                             D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    TransitionResource(commands, buffer, before, after);
}

/// DEFAULT-heap buffers (the buffer cache's) are implicitly promoted by copies; hand them back in
/// COMMON, the state the buffer cache assumes, so its next promotion is legal. UPLOAD/READBACK
/// buffers can never change state.
inline void DecayIfDefault(ID3D12GraphicsCommandList* commands, ID3D12Resource* buffer,
                           D3D12_RESOURCE_STATES promoted) {
    if (HeapType(buffer) == D3D12_HEAP_TYPE_DEFAULT) {
        TransitionBuffer(commands, buffer, promoted, D3D12_RESOURCE_STATE_COMMON);
    }
}

/// Logs once per process; the bug tracker counts every occurrence, keyed by the flag (one per kind
/// of skipped copy, blit, view or transfer). The message is formatted only on the first one.
template <typename... Args>
void WarnOnce(bool& logged, fmt::format_string<Args...> format, Args&&... args) {
    // fmt only reads its arguments, so forwarding them twice is safe.
    BUG_TRACK_KEY(CopySkipped, reinterpret_cast<std::uintptr_t>(&logged), "{}",
                  fmt::format(format, std::forward<Args>(args)...));
    WarnOnceLog(logged, format, std::forward<Args>(args)...);
}

/// Bytes per texel of a depth-stencil plane in a copy footprint: the depth plane of D24S8 and
/// D32S8 copies as R32 (D24 in the low 24 bits), the stencil plane as R8 ("Planar Depth
/// Stencil", DirectX-Specs).
constexpr u32 PlaneTexelBytes(u32 plane) {
    return plane == 0 ? 4 : 1;
}

inline bool logged_unsupported_transfer = false;
inline bool logged_depth_stencil_transfer = false;
inline bool logged_depth_stencil_unaligned = false;
inline bool logged_depth_stencil_copy = false;
inline bool logged_depth_stencil_upload = false;
inline bool logged_depth_stencil_download = false;
inline bool logged_self_copy = false;
inline bool logged_decoded_copy = false;
inline bool logged_gpu_astc = false;
inline bool logged_reinterpret_copy = false;
inline bool logged_depth_size = false;
inline bool logged_view_format = false;
inline bool logged_self_blit = false;
inline bool logged_msaa_blit = false;
inline bool logged_mixed_blit = false;
inline bool logged_integer_blit = false;
inline bool logged_blit_target = false;
inline bool logged_stencil_blit = false;
inline bool logged_no_blit_helper = false;
inline bool logged_missing_rtv = false;
inline bool logged_min_max_filter = false;
inline bool logged_point_reduction = false;
inline bool logged_srv_fallback = false;
inline bool logged_view_family = false;

// Runtime toggles (Set* in d3d12_texture_cache.cpp).
/// Sampler keys for SamplerHeap; 0 is the presenter's linear sampler.
inline std::atomic<u64> next_sampler_key{1};
inline std::atomic<bool> decode_bc_arrays{true};
inline std::atomic<bool> gpu_astc_decode{true};
inline std::atomic<int> gpu_astc_verify{0}; ///< GPU ASTC uploads left to check against the CPU
inline std::atomic<bool> gpu_astc_sync{false};
inline std::atomic<bool> gpu_astc_fresh{false};

constexpr u64 ASTC_RGBA_SCRATCH_BUDGET = 32ULL * 1024 * 1024;
constexpr u64 ASTC_BC_SCRATCH_BUDGET = 8ULL * 1024 * 1024;

} // namespace D3D12::TextureDetail

namespace D3D12 {

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

} // namespace D3D12
