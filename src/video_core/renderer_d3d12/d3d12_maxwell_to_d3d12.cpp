// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>

#include "common/logging.h"
#include "video_core/renderer_d3d12/d3d12_maxwell_to_d3d12.h"

namespace D3D12::MaxwellToD3D12 {

namespace {

using Size = Maxwell::VertexAttribute::Size;
using Type = Maxwell::VertexAttribute::Type;

/// DXGI formats of one component width, indexed by component count - 1, for each kind.
struct FormatRow {
    DXGI_FORMAT unorm[4];
    DXGI_FORMAT snorm[4];
    DXGI_FORMAT uint[4];
    DXGI_FORMAT sint[4];
    DXGI_FORMAT sfloat[4];
};

// No three-component 8- or 16-bit formats exist: those rows repeat the four-component one.
constexpr FormatRow ROW_8{
    {DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8G8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM,
     DXGI_FORMAT_R8G8B8A8_UNORM},
    {DXGI_FORMAT_R8_SNORM, DXGI_FORMAT_R8G8_SNORM, DXGI_FORMAT_R8G8B8A8_SNORM,
     DXGI_FORMAT_R8G8B8A8_SNORM},
    {DXGI_FORMAT_R8_UINT, DXGI_FORMAT_R8G8_UINT, DXGI_FORMAT_R8G8B8A8_UINT,
     DXGI_FORMAT_R8G8B8A8_UINT},
    {DXGI_FORMAT_R8_SINT, DXGI_FORMAT_R8G8_SINT, DXGI_FORMAT_R8G8B8A8_SINT,
     DXGI_FORMAT_R8G8B8A8_SINT},
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN},
};
constexpr FormatRow ROW_16{
    {DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16G16_UNORM, DXGI_FORMAT_R16G16B16A16_UNORM,
     DXGI_FORMAT_R16G16B16A16_UNORM},
    {DXGI_FORMAT_R16_SNORM, DXGI_FORMAT_R16G16_SNORM, DXGI_FORMAT_R16G16B16A16_SNORM,
     DXGI_FORMAT_R16G16B16A16_SNORM},
    {DXGI_FORMAT_R16_UINT, DXGI_FORMAT_R16G16_UINT, DXGI_FORMAT_R16G16B16A16_UINT,
     DXGI_FORMAT_R16G16B16A16_UINT},
    {DXGI_FORMAT_R16_SINT, DXGI_FORMAT_R16G16_SINT, DXGI_FORMAT_R16G16B16A16_SINT,
     DXGI_FORMAT_R16G16B16A16_SINT},
    {DXGI_FORMAT_R16_FLOAT, DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT,
     DXGI_FORMAT_R16G16B16A16_FLOAT},
};
constexpr FormatRow ROW_32{
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN},
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN},
    {DXGI_FORMAT_R32_UINT, DXGI_FORMAT_R32G32_UINT, DXGI_FORMAT_R32G32B32_UINT,
     DXGI_FORMAT_R32G32B32A32_UINT},
    {DXGI_FORMAT_R32_SINT, DXGI_FORMAT_R32G32_SINT, DXGI_FORMAT_R32G32B32_SINT,
     DXGI_FORMAT_R32G32B32A32_SINT},
    {DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32G32_FLOAT, DXGI_FORMAT_R32G32B32_FLOAT,
     DXGI_FORMAT_R32G32B32A32_FLOAT},
};

DXGI_FORMAT Pick(const FormatRow& row, Type type, u32 components) {
    const u32 i = components - 1;
    switch (type) {
    case Type::UNorm:
        return row.unorm[i];
    case Type::SNorm:
        return row.snorm[i];
    case Type::UInt:
    case Type::UScaled:
        return row.uint[i];
    case Type::SInt:
    case Type::SScaled:
        return row.sint[i];
    case Type::Float:
        return row.sfloat[i];
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}

} // Anonymous namespace

D3D12_PRIMITIVE_TOPOLOGY_TYPE PrimitiveTopologyType(Maxwell::PrimitiveTopology topology) {
    switch (topology) {
    case Maxwell::PrimitiveTopology::Points:
        return D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
    case Maxwell::PrimitiveTopology::Lines:
    case Maxwell::PrimitiveTopology::LineLoop:
    case Maxwell::PrimitiveTopology::LineStrip:
    case Maxwell::PrimitiveTopology::LinesAdjacency:
    case Maxwell::PrimitiveTopology::LineStripAdjacency:
        return D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
    case Maxwell::PrimitiveTopology::Patches:
        return D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
    case Maxwell::PrimitiveTopology::Triangles:
    case Maxwell::PrimitiveTopology::TriangleStrip:
    case Maxwell::PrimitiveTopology::TriangleFan:
    case Maxwell::PrimitiveTopology::Quads:
    case Maxwell::PrimitiveTopology::QuadStrip:
    case Maxwell::PrimitiveTopology::Polygon:
    case Maxwell::PrimitiveTopology::TrianglesAdjacency:
    case Maxwell::PrimitiveTopology::TriangleStripAdjacency:
        return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    }
    return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
}

D3D_PRIMITIVE_TOPOLOGY PrimitiveTopology(Maxwell::PrimitiveTopology topology, u32 patch_points) {
    switch (topology) {
    case Maxwell::PrimitiveTopology::Points:
        return D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
    case Maxwell::PrimitiveTopology::Lines:
    case Maxwell::PrimitiveTopology::LineLoop:
        return D3D_PRIMITIVE_TOPOLOGY_LINELIST;
    case Maxwell::PrimitiveTopology::LineStrip:
        return D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
    case Maxwell::PrimitiveTopology::TriangleStrip:
        return D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
    case Maxwell::PrimitiveTopology::LinesAdjacency:
        return D3D_PRIMITIVE_TOPOLOGY_LINELIST_ADJ;
    case Maxwell::PrimitiveTopology::LineStripAdjacency:
        return D3D_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ;
    case Maxwell::PrimitiveTopology::TrianglesAdjacency:
        return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ;
    case Maxwell::PrimitiveTopology::TriangleStripAdjacency:
        return D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ;
    case Maxwell::PrimitiveTopology::Patches: {
        const u32 points = std::clamp<u32>(patch_points, 1, 32);
        return static_cast<D3D_PRIMITIVE_TOPOLOGY>(
            D3D_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST + points - 1);
    }
    case Maxwell::PrimitiveTopology::Triangles:
    case Maxwell::PrimitiveTopology::TriangleFan:
    case Maxwell::PrimitiveTopology::Quads:
    case Maxwell::PrimitiveTopology::QuadStrip:
    case Maxwell::PrimitiveTopology::Polygon:
        return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    }
    return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
}

D3D12_COMPARISON_FUNC ComparisonFunc(Maxwell::ComparisonOp comparison) {
    switch (comparison) {
    case Maxwell::ComparisonOp::Never_D3D:
    case Maxwell::ComparisonOp::Never_GL:
        return D3D12_COMPARISON_FUNC_NEVER;
    case Maxwell::ComparisonOp::Less_D3D:
    case Maxwell::ComparisonOp::Less_GL:
        return D3D12_COMPARISON_FUNC_LESS;
    case Maxwell::ComparisonOp::Equal_D3D:
    case Maxwell::ComparisonOp::Equal_GL:
        return D3D12_COMPARISON_FUNC_EQUAL;
    case Maxwell::ComparisonOp::LessEqual_D3D:
    case Maxwell::ComparisonOp::LessEqual_GL:
        return D3D12_COMPARISON_FUNC_LESS_EQUAL;
    case Maxwell::ComparisonOp::Greater_D3D:
    case Maxwell::ComparisonOp::Greater_GL:
        return D3D12_COMPARISON_FUNC_GREATER;
    case Maxwell::ComparisonOp::NotEqual_D3D:
    case Maxwell::ComparisonOp::NotEqual_GL:
        return D3D12_COMPARISON_FUNC_NOT_EQUAL;
    case Maxwell::ComparisonOp::GreaterEqual_D3D:
    case Maxwell::ComparisonOp::GreaterEqual_GL:
        return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    case Maxwell::ComparisonOp::Always_D3D:
    case Maxwell::ComparisonOp::Always_GL:
        return D3D12_COMPARISON_FUNC_ALWAYS;
    }
    LOG_ERROR(Render, "D3D12: unknown comparison op {}", static_cast<u32>(comparison));
    return D3D12_COMPARISON_FUNC_ALWAYS;
}

D3D12_STENCIL_OP StencilOp(Maxwell::StencilOp::Op op) {
    switch (op) {
    case Maxwell::StencilOp::Op::Keep_D3D:
    case Maxwell::StencilOp::Op::Keep_GL:
        return D3D12_STENCIL_OP_KEEP;
    case Maxwell::StencilOp::Op::Zero_D3D:
    case Maxwell::StencilOp::Op::Zero_GL:
        return D3D12_STENCIL_OP_ZERO;
    case Maxwell::StencilOp::Op::Replace_D3D:
    case Maxwell::StencilOp::Op::Replace_GL:
        return D3D12_STENCIL_OP_REPLACE;
    case Maxwell::StencilOp::Op::IncrSaturate_D3D:
    case Maxwell::StencilOp::Op::IncrSaturate_GL:
        return D3D12_STENCIL_OP_INCR_SAT;
    case Maxwell::StencilOp::Op::DecrSaturate_D3D:
    case Maxwell::StencilOp::Op::DecrSaturate_GL:
        return D3D12_STENCIL_OP_DECR_SAT;
    case Maxwell::StencilOp::Op::Invert_D3D:
    case Maxwell::StencilOp::Op::Invert_GL:
        return D3D12_STENCIL_OP_INVERT;
    case Maxwell::StencilOp::Op::Incr_D3D:
    case Maxwell::StencilOp::Op::Incr_GL:
        return D3D12_STENCIL_OP_INCR;
    case Maxwell::StencilOp::Op::Decr_D3D:
    case Maxwell::StencilOp::Op::Decr_GL:
        return D3D12_STENCIL_OP_DECR;
    }
    LOG_ERROR(Render, "D3D12: unknown stencil op {}", static_cast<u32>(op));
    return D3D12_STENCIL_OP_KEEP;
}

D3D12_BLEND_OP BlendOp(Maxwell::Blend::Equation equation) {
    switch (equation) {
    case Maxwell::Blend::Equation::Add_D3D:
    case Maxwell::Blend::Equation::Add_GL:
        return D3D12_BLEND_OP_ADD;
    case Maxwell::Blend::Equation::Subtract_D3D:
    case Maxwell::Blend::Equation::Subtract_GL:
        return D3D12_BLEND_OP_SUBTRACT;
    case Maxwell::Blend::Equation::ReverseSubtract_D3D:
    case Maxwell::Blend::Equation::ReverseSubtract_GL:
        return D3D12_BLEND_OP_REV_SUBTRACT;
    case Maxwell::Blend::Equation::Min_D3D:
    case Maxwell::Blend::Equation::Min_GL:
        return D3D12_BLEND_OP_MIN;
    case Maxwell::Blend::Equation::Max_D3D:
    case Maxwell::Blend::Equation::Max_GL:
        return D3D12_BLEND_OP_MAX;
    }
    LOG_ERROR(Render, "D3D12: unknown blend equation {}", static_cast<u32>(equation));
    return D3D12_BLEND_OP_ADD;
}

D3D12_BLEND BlendFactor(Maxwell::Blend::Factor factor, bool for_alpha) {
    using F = Maxwell::Blend::Factor;
    switch (factor) {
    case F::Zero_D3D:
    case F::Zero_GL:
        return D3D12_BLEND_ZERO;
    case F::One_D3D:
    case F::One_GL:
        return D3D12_BLEND_ONE;
    case F::SourceColor_D3D:
    case F::SourceColor_GL:
        return for_alpha ? D3D12_BLEND_SRC_ALPHA : D3D12_BLEND_SRC_COLOR;
    case F::OneMinusSourceColor_D3D:
    case F::OneMinusSourceColor_GL:
        return for_alpha ? D3D12_BLEND_INV_SRC_ALPHA : D3D12_BLEND_INV_SRC_COLOR;
    case F::SourceAlpha_D3D:
    case F::SourceAlpha_GL:
        return D3D12_BLEND_SRC_ALPHA;
    case F::OneMinusSourceAlpha_D3D:
    case F::OneMinusSourceAlpha_GL:
        return D3D12_BLEND_INV_SRC_ALPHA;
    case F::DestAlpha_D3D:
    case F::DestAlpha_GL:
        return D3D12_BLEND_DEST_ALPHA;
    case F::OneMinusDestAlpha_D3D:
    case F::OneMinusDestAlpha_GL:
        return D3D12_BLEND_INV_DEST_ALPHA;
    case F::DestColor_D3D:
    case F::DestColor_GL:
        return for_alpha ? D3D12_BLEND_DEST_ALPHA : D3D12_BLEND_DEST_COLOR;
    case F::OneMinusDestColor_D3D:
    case F::OneMinusDestColor_GL:
        return for_alpha ? D3D12_BLEND_INV_DEST_ALPHA : D3D12_BLEND_INV_DEST_COLOR;
    case F::SourceAlphaSaturate_D3D:
    case F::SourceAlphaSaturate_GL:
        return D3D12_BLEND_SRC_ALPHA_SAT;
    case F::Source1Color_D3D:
    case F::Source1Color_GL:
        return for_alpha ? D3D12_BLEND_SRC1_ALPHA : D3D12_BLEND_SRC1_COLOR;
    case F::OneMinusSource1Color_D3D:
    case F::OneMinusSource1Color_GL:
        return for_alpha ? D3D12_BLEND_INV_SRC1_ALPHA : D3D12_BLEND_INV_SRC1_COLOR;
    case F::Source1Alpha_D3D:
    case F::Source1Alpha_GL:
        return D3D12_BLEND_SRC1_ALPHA;
    case F::OneMinusSource1Alpha_D3D:
    case F::OneMinusSource1Alpha_GL:
        return D3D12_BLEND_INV_SRC1_ALPHA;
    // D3D12 has a single blend factor for color and alpha: the constant-alpha variants read
    // its alpha only in the alpha equation. Close enough until a game needs the difference.
    case F::BlendFactor_D3D:
    case F::ConstantColor_GL:
    case F::BothSourceAlpha_D3D:
    case F::ConstantAlpha_GL:
        return D3D12_BLEND_BLEND_FACTOR;
    case F::OneMinusBlendFactor_D3D:
    case F::OneMinusConstantColor_GL:
    case F::OneMinusBothSourceAlpha_D3D:
    case F::OneMinusConstantAlpha_GL:
        return D3D12_BLEND_INV_BLEND_FACTOR;
    }
    LOG_ERROR(Render, "D3D12: unknown blend factor {}", static_cast<u32>(factor));
    return D3D12_BLEND_ONE;
}

D3D12_CULL_MODE CullMode(bool enabled, Maxwell::CullFace face) {
    if (!enabled) {
        return D3D12_CULL_MODE_NONE;
    }
    switch (face) {
    case Maxwell::CullFace::Front:
        return D3D12_CULL_MODE_FRONT;
    case Maxwell::CullFace::Back:
        return D3D12_CULL_MODE_BACK;
    case Maxwell::CullFace::FrontAndBack:
        // No D3D12 equivalent; the rasterizer discards these draws (phase 4.3).
        return D3D12_CULL_MODE_NONE;
    }
    return D3D12_CULL_MODE_NONE;
}

D3D12_FILL_MODE FillMode(Maxwell::PolygonMode mode) {
    return mode == Maxwell::PolygonMode::Line ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
}

DXGI_FORMAT VertexFormat(Type type, Size size) {
    switch (size) {
    case Size::Size_R8:
    case Size::Size_A8:
        return Pick(ROW_8, type, 1);
    case Size::Size_R8_G8:
    case Size::Size_G8_R8:
        return Pick(ROW_8, type, 2);
    case Size::Size_R8_G8_B8:
        return Pick(ROW_8, type, 3);
    case Size::Size_R8_G8_B8_A8:
    case Size::Size_X8_B8_G8_R8:
        return Pick(ROW_8, type, 4);
    case Size::Size_R16:
        return Pick(ROW_16, type, 1);
    case Size::Size_R16_G16:
        return Pick(ROW_16, type, 2);
    case Size::Size_R16_G16_B16:
        return Pick(ROW_16, type, 3);
    case Size::Size_R16_G16_B16_A16:
        return Pick(ROW_16, type, 4);
    case Size::Size_R32:
        return Pick(ROW_32, type, 1);
    case Size::Size_R32_G32:
        return Pick(ROW_32, type, 2);
    case Size::Size_R32_G32_B32:
        return Pick(ROW_32, type, 3);
    case Size::Size_R32_G32_B32_A32:
        return Pick(ROW_32, type, 4);
    case Size::Size_A2_B10_G10_R10:
        // Same bit layout as Vulkan's A2B10G10R10_PACK32: R in the low bits. DXGI has only the
        // unsigned variants; SNORM is fetched as the packed word and unpacked in the shader
        // (Shader::AttributeType::SignedNormA2B10G10R10). R32_UINT is the one fetch every IA
        // handles; on the Series every object whose normals came as R10G10B10A2_UINT went NaN.
        switch (type) {
        case Type::UNorm:
            return DXGI_FORMAT_R10G10B10A2_UNORM;
        case Type::UInt:
        case Type::UScaled:
            return DXGI_FORMAT_R10G10B10A2_UINT;
        case Type::SNorm:
            return DXGI_FORMAT_R32_UINT;
        default:
            return DXGI_FORMAT_UNKNOWN;
        }
    case Size::Size_B10_G11_R11:
        return type == Type::Float ? DXGI_FORMAT_R11G11B10_FLOAT : DXGI_FORMAT_UNKNOWN;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}

AttributeFetch SplitAttributeFetch(Type type, Size size, u32 offset, u32 stride) {
    const FormatRow* row = nullptr;
    u32 component_bytes = 0;
    u32 components = 0;
    switch (size) {
    case Size::Size_R8_G8:
    case Size::Size_G8_R8:
        row = &ROW_8, component_bytes = 1, components = 2;
        break;
    case Size::Size_R8_G8_B8:
    case Size::Size_R8_G8_B8_A8:
    case Size::Size_X8_B8_G8_R8:
        row = &ROW_8, component_bytes = 1, components = 4;
        break;
    case Size::Size_R16_G16:
        row = &ROW_16, component_bytes = 2, components = 2;
        break;
    case Size::Size_R16_G16_B16:
    case Size::Size_R16_G16_B16_A16:
        row = &ROW_16, component_bytes = 2, components = 4;
        break;
    default:
        return {}; // one component, or 32-bit ones (4-aligned in practice)
    }
    // Largest power of two (up to 4) every vertex's address is aligned to.
    const u32 bits = offset | stride | 4U;
    const u32 alignment = bits & (~bits + 1U);
    const u32 required = std::min(4U, component_bytes * components);
    if (alignment >= required || alignment < component_bytes) {
        return {};
    }
    const u32 part_components = alignment / component_bytes;
    return {
        .parts = components / part_components,
        .part_components = part_components,
        .part_bytes = alignment,
        .part_format = Pick(*row, type, part_components),
    };
}

DXGI_FORMAT IndexFormat(Maxwell::IndexFormat format) {
    switch (format) {
    case Maxwell::IndexFormat::UnsignedShort:
        return DXGI_FORMAT_R16_UINT;
    case Maxwell::IndexFormat::UnsignedInt:
        return DXGI_FORMAT_R32_UINT;
    case Maxwell::IndexFormat::UnsignedByte:
        return DXGI_FORMAT_UNKNOWN;
    }
    return DXGI_FORMAT_UNKNOWN;
}

u32 SampleCount(Tegra::Texture::MsaaMode mode) {
    switch (mode) {
    case Tegra::Texture::MsaaMode::Msaa2x1:
    case Tegra::Texture::MsaaMode::Msaa2x1_D3D:
        return 2;
    case Tegra::Texture::MsaaMode::Msaa2x2:
    case Tegra::Texture::MsaaMode::Msaa2x2_VC4:
    case Tegra::Texture::MsaaMode::Msaa2x2_VC12:
        return 4;
    case Tegra::Texture::MsaaMode::Msaa4x2:
    case Tegra::Texture::MsaaMode::Msaa4x2_D3D:
    case Tegra::Texture::MsaaMode::Msaa4x2_VC8:
    case Tegra::Texture::MsaaMode::Msaa4x2_VC24:
        return 8;
    case Tegra::Texture::MsaaMode::Msaa4x4:
        return 16;
    default:
        return 1;
    }
}

} // namespace D3D12::MaxwellToD3D12
