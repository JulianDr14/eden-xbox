// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <d3d12.h>

#include "video_core/engines/maxwell_3d.h"

/// Maxwell fixed-function state -> D3D12, the counterpart of renderer_vulkan/maxwell_to_vk.
/// Values D3D12 lacks fall back to the closest one and are reported by the caller.
namespace D3D12::MaxwellToD3D12 {

using Maxwell = Tegra::Engines::Maxwell3D::Regs;

/// Topology class for the PSO. Quads, fans and loops are rewritten as triangle or line lists
/// by the draw (phase 4.3), so they map to their list class here.
D3D12_PRIMITIVE_TOPOLOGY_TYPE PrimitiveTopologyType(Maxwell::PrimitiveTopology topology);

/// Topology for IASetPrimitiveTopology, after the draw rewrote emulated ones. Patches take the
/// control point count.
D3D_PRIMITIVE_TOPOLOGY PrimitiveTopology(Maxwell::PrimitiveTopology topology, u32 patch_points);

D3D12_COMPARISON_FUNC ComparisonFunc(Maxwell::ComparisonOp comparison);

D3D12_STENCIL_OP StencilOp(Maxwell::StencilOp::Op op);

D3D12_BLEND_OP BlendOp(Maxwell::Blend::Equation equation);

/// D3D12 splits color and alpha factors: the alpha ones may not name a color (D3D12_BLEND_SRC_COLOR
/// is invalid in SrcBlendAlpha), so for_alpha picks the alpha equivalent.
D3D12_BLEND BlendFactor(Maxwell::Blend::Factor factor, bool for_alpha);

D3D12_CULL_MODE CullMode(bool enabled, Maxwell::CullFace face);

/// D3D12 has no point fill mode; it becomes solid and the caller warns.
D3D12_FILL_MODE FillMode(Maxwell::PolygonMode mode);

/// Vertex attribute format; DXGI_FORMAT_UNKNOWN when D3D12 has no equivalent. Three-component 8-
/// and 16-bit formats widen to four components (the extra one is read but the shader ignores it).
/// Scaled types never reach here: Shader::Profile::support_scaled_attributes is false, so the
/// recompiler reads them as integers and converts in the shader.
DXGI_FORMAT VertexFormat(Maxwell::VertexAttribute::Type type, Maxwell::VertexAttribute::Size size);

/// Index buffer format; UnsignedByte has none (the draw widens it to 16 bits).
DXGI_FORMAT IndexFormat(Maxwell::IndexFormat format);

/// Sample count of an MSAA mode.
u32 SampleCount(Tegra::Texture::MsaaMode mode);

} // namespace D3D12::MaxwellToD3D12
