// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Shared by the files implementing RendererD3D12 (renderer_d3d12.cpp, d3d12_present_*.cpp,
// d3d12_overlay.cpp, diagnostics/). Not meant for other users.

#include "video_core/renderer_d3d12/d3d12_resource_utils.h"
#include "video_core/renderer_d3d12/d3d12_staging_buffer_pool.h"

namespace D3D12::PresentDetail {

inline D3D12_RESOURCE_DESC Texture2DDesc(u32 width, u32 height, DXGI_FORMAT format) {
    return D3D12_RESOURCE_DESC{
        .Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
        .Alignment = 0,
        .Width = width,
        .Height = height,
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .Format = format,
        .SampleDesc = {.Count = 1, .Quality = 0},
        .Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
        .Flags = D3D12_RESOURCE_FLAG_NONE,
    };
}

/// A staging allocation as the source of a buffer -> texture copy.
inline D3D12_TEXTURE_COPY_LOCATION StagingSource(const StagingBufferRef& ref,
                                                 D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint) {
    footprint.Offset = ref.offset;
    return D3D12_TEXTURE_COPY_LOCATION{
        .pResource = ref.buffer,
        .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
        .PlacedFootprint = footprint,
    };
}

} // namespace D3D12::PresentDetail
