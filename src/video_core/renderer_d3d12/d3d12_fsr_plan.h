// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// What one FidelityFX Super Resolution 1 present does: whether to run it at all and the
// constants of its two passes. Free of D3D12 so it can be tested on its own
// (tools/xbox/tests/fsr-plan.cpp); d3d12_present_fsr.h records it.

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>

#include "common/common_types.h"
#include "common/math_util.h"
#include "video_core/fsr.h"

namespace D3D12::Fsr {

/// The largest scale per axis EASU is tuned for: "Quality from 1x to 4x area scaling is good"
/// (ffx_fsr1.h). Larger ones still work, softer; the renderer says so in the log.
inline constexpr f32 MAX_TUNED_SCALE = 2.0f;

/// fsr_sharpening_slider range, as the Vulkan and OpenGL presenters read it.
inline constexpr int MAX_SHARPENING_SLIDER = 200;

/// RCAS sharpness in stops from the slider: 0 is the maximum and each stop halves it
/// (FsrRcasCon). Same mapping as the other backends, so a configuration looks alike on all.
[[nodiscard]] constexpr f32 SharpnessStops(int slider) noexcept {
    return static_cast<f32>(std::clamp(slider, 0, MAX_SHARPENING_SLIDER)) / 100.0f;
}

struct Plan {
    u32 output_width{};  ///< EASU target and RCAS output: the screen rectangle
    u32 output_height{};
    f32 scale_x{}; ///< source texels per output pixel, inverted: how much EASU enlarges
    f32 scale_y{};
    f32 sharpness{}; ///< RCAS stops
    /// Root constants of the EASU pass: con0..con3 of FsrEasuConOffset, the shader's Const0..3.
    std::array<u32, 16> easu{};
    /// Root constants of the RCAS pass: con of FsrRcasCon, the shader's Const0.
    std::array<u32, 4> rcas{};

    [[nodiscard]] bool WithinTunedRange() const noexcept {
        return scale_x <= MAX_TUNED_SCALE && scale_y <= MAX_TUNED_SCALE;
    }
};

/// The FSR passes that show `crop` of a texture_width x texture_height texture on an
/// output_width x output_height screen rectangle. crop is normalized as Tegra::NormalizeCrop
/// returns it: the screen's top edge samples crop.top and its bottom edge crop.bottom, so
/// top > bottom (or left > right) shows the region flipped, as the bilinear blit does.
///
/// nullopt when FSR has nothing to add, and the caller presents with the bilinear blit:
/// - a size is zero or the crop is empty;
/// - the screen is smaller than the region on either axis: EASU only enlarges (its 2x2 clamp and
///   12 taps alias when minifying);
/// - the screen is exactly the region's size: there is no scaling to reconstruct.
[[nodiscard]] inline std::optional<Plan> MakePlan(u32 texture_width, u32 texture_height,
                                                  const Common::Rectangle<f32>& crop,
                                                  u32 output_width, u32 output_height,
                                                  int sharpening_slider) {
    if (texture_width == 0 || texture_height == 0 || output_width == 0 || output_height == 0) {
        return std::nullopt;
    }
    const f32 texture_w = static_cast<f32>(texture_width);
    const f32 texture_h = static_cast<f32>(texture_height);
    const f32 output_w = static_cast<f32>(output_width);
    const f32 output_h = static_cast<f32>(output_height);
    // Signed: a flipped crop maps the screen to the region backwards.
    const f32 region_w = (crop.right - crop.left) * texture_w;
    const f32 region_h = (crop.bottom - crop.top) * texture_h;
    const f32 abs_w = std::abs(region_w);
    const f32 abs_h = std::abs(region_h);
    if (!(abs_w >= 1.0f && abs_h >= 1.0f)) { // also rejects NaN
        return std::nullopt;
    }
    if (output_w < abs_w || output_h < abs_h || (output_w == abs_w && output_h == abs_h)) {
        return std::nullopt;
    }

    Plan plan{
        .output_width = output_width,
        .output_height = output_height,
        .scale_x = output_w / abs_w,
        .scale_y = output_h / abs_h,
        .sharpness = SharpnessStops(sharpening_slider),
    };
    // EASU maps output pixel p to source texel p * region / output + crop origin, from the
    // output's top-left corner (SV_Position), so the origin is the texel the top edge samples.
    FSR::FsrEasuConOffset(plan.easu.data() + 0, plan.easu.data() + 4, plan.easu.data() + 8,
                          plan.easu.data() + 12, region_w, region_h, texture_w, texture_h,
                          output_w, output_h, crop.left * texture_w, crop.top * texture_h);
    FSR::FsrRcasCon(plan.rcas.data(), plan.sharpness);
    return plan;
}

} // namespace D3D12::Fsr
