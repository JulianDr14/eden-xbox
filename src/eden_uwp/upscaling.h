// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// How the game image is enlarged to the screen: the bilinear blit or AMD FSR 1 at one of three
// sharpness levels. One choice in the interface, two Eden settings underneath (scaling_filter and
// fsr_sharpening_slider), which the D3D12 present reads (d3d12_present_fsr.h).

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "common/settings_enums.h"
#include "eden_uwp/localization.h"

namespace EdenXbox {

enum class Upscaling : std::uint8_t { Bilinear, FsrSharp, FsrBalanced, FsrSoft };

/// The values offered, in the order left/right step through them.
inline std::span<const Upscaling> SelectableUpscalings() {
    static constexpr std::array values{Upscaling::Bilinear, Upscaling::FsrSharp,
                                       Upscaling::FsrBalanced, Upscaling::FsrSoft};
    return values;
}

struct UpscalingSettings {
    Settings::ScalingFilter filter;
    int fsr_sharpening_slider; ///< RCAS stops x 100: 0 is the sharpest, each 100 halves it
};

/// Sharp is RCAS at its maximum; balanced is Eden's default slider (0.25 stops), the same image
/// as the other backends give; soft is one stop, half the sharpening.
inline constexpr int SliderSharp = 0;
inline constexpr int SliderBalanced = 25;
inline constexpr int SliderSoft = 100;

inline UpscalingSettings ToSettings(Upscaling upscaling) {
    using Settings::ScalingFilter;
    switch (upscaling) {
    case Upscaling::FsrSharp: return {ScalingFilter::Fsr, SliderSharp};
    case Upscaling::FsrBalanced: return {ScalingFilter::Fsr, SliderBalanced};
    case Upscaling::FsrSoft: return {ScalingFilter::Fsr, SliderSoft};
    case Upscaling::Bilinear: break;
    }
    // The D3D12 present only has the bilinear blit and FSR; Bilinear names what it shows.
    return {ScalingFilter::Bilinear, SliderBalanced};
}

/// The choice closest to arbitrary settings (another frontend's): any filter but FSR shows the
/// bilinear blit, and a slider picks the nearest level.
inline Upscaling FromSettings(Settings::ScalingFilter filter, int slider) {
    if (filter != Settings::ScalingFilter::Fsr) return Upscaling::Bilinear;
    if (slider < (SliderSharp + SliderBalanced + 1) / 2) return Upscaling::FsrSharp;
    if (slider < (SliderBalanced + SliderSoft + 1) / 2) return Upscaling::FsrBalanced;
    return Upscaling::FsrSoft;
}

/// A stored value, or nullopt when it is not one of the choices (an older or newer build's).
inline std::optional<Upscaling> UpscalingFromValue(std::int32_t value) {
    for (const Upscaling upscaling : SelectableUpscalings())
        if (static_cast<std::int32_t>(upscaling) == value) return upscaling;
    return std::nullopt;
}

inline Upscaling StepUpscaling(Upscaling current, bool back) {
    const auto values = SelectableUpscalings();
    const std::size_t count = values.size();
    std::size_t index = 0;
    while (index < count && values[index] != current) ++index;
    if (index == count) return values.front();
    return values[(index + (back ? count - 1 : 1)) % count];
}

/// boot.cfg "upscaling=": bilinear, fsr (balanced), fsr-sharp, fsr-balanced or fsr-soft.
inline std::optional<Upscaling> ParseUpscaling(std::string_view text) {
    if (text == "bilinear") return Upscaling::Bilinear;
    if (text == "fsr" || text == "fsr-balanced") return Upscaling::FsrBalanced;
    if (text == "fsr-sharp") return Upscaling::FsrSharp;
    if (text == "fsr-soft") return Upscaling::FsrSoft;
    return std::nullopt;
}

inline I18n::Text UpscalingText(Upscaling upscaling) {
    using I18n::Text;
    switch (upscaling) {
    case Upscaling::FsrSharp: return Text::UpscalingFsrSharp;
    case Upscaling::FsrBalanced: return Text::UpscalingFsrBalanced;
    case Upscaling::FsrSoft: return Text::UpscalingFsrSoft;
    case Upscaling::Bilinear: break;
    }
    return Text::UpscalingBilinear;
}

} // namespace EdenXbox
