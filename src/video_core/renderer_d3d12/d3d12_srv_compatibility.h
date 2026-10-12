// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <d3d12.h>
#include "shader_recompiler/shader_info.h"

namespace D3D12 {

// Physical view compatibility only. A 3D-to-2D-array adapter is handled separately;
// changing the requested shader type cannot make an incompatible resource valid.
[[nodiscard]] inline bool IsSrvDimensionCompatible(Shader::TextureType type,
                                                   const D3D12_RESOURCE_DESC& resource,
                                                   u32 base_layer, u32 layers) {
    using Shader::TextureType;
    if (resource.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE3D &&
        (layers == 0 || base_layer >= resource.DepthOrArraySize ||
         layers > resource.DepthOrArraySize - base_layer)) {
        return false;
    }
    switch (type) {
    case TextureType::Color1D:
    case TextureType::ColorArray1D:
        return resource.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D &&
               resource.SampleDesc.Count == 1;
    case TextureType::Color2D:
    case TextureType::Color2DRect:
    case TextureType::ColorArray2D:
        return resource.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    case TextureType::Color3D:
        return resource.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D &&
               resource.SampleDesc.Count == 1;
    case TextureType::ColorCube:
    case TextureType::ColorArrayCube:
        return resource.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
               resource.SampleDesc.Count == 1 && resource.Width == resource.Height &&
               layers >= 6 && (type == TextureType::ColorCube || layers % 6 == 0);
    case TextureType::Buffer:
        return false;
    }
    return false;
}

} // namespace D3D12
