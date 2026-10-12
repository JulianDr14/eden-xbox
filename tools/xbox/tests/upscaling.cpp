// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
// Build with src/eden_uwp/localization.cpp and src/eden_uwp/locales/*.cpp.
//
// The image upscaling choice (eden_uwp/upscaling.h): what each choice sets in Eden's settings,
// the way back from any settings, stored values, boot.cfg values and the menu texts.
#include <cstdio>
#include <cstdlib>
#include "eden_uwp/upscaling.h"
#include "video_core/renderer_d3d12/d3d12_fsr_plan.h"

int main() {
    using namespace EdenXbox;
    using Settings::ScalingFilter;
    auto check = [](bool ok) { if (!ok) std::abort(); };

    // Every choice survives a trip through the settings, and its slider is a valid RCAS level.
    for (const Upscaling upscaling : SelectableUpscalings()) {
        const UpscalingSettings values = ToSettings(upscaling);
        check(FromSettings(values.filter, values.fsr_sharpening_slider) == upscaling);
        check(values.fsr_sharpening_slider >= 0 &&
              values.fsr_sharpening_slider <= D3D12::Fsr::MAX_SHARPENING_SLIDER);
        check((upscaling == Upscaling::Bilinear) == (values.filter != ScalingFilter::Fsr));
        check(UpscalingFromValue(static_cast<std::int32_t>(upscaling)) == upscaling);
    }
    // Sharper choices sharpen more (fewer RCAS stops).
    check(D3D12::Fsr::SharpnessStops(ToSettings(Upscaling::FsrSharp).fsr_sharpening_slider) <
          D3D12::Fsr::SharpnessStops(ToSettings(Upscaling::FsrBalanced).fsr_sharpening_slider));
    check(D3D12::Fsr::SharpnessStops(ToSettings(Upscaling::FsrBalanced).fsr_sharpening_slider) <
          D3D12::Fsr::SharpnessStops(ToSettings(Upscaling::FsrSoft).fsr_sharpening_slider));
    // Balanced is Eden's default slider: the image the other backends give.
    check(ToSettings(Upscaling::FsrBalanced).fsr_sharpening_slider == 25);

    // Settings another frontend wrote: any non-FSR filter is the bilinear blit D3D12 has, and a
    // slider picks the nearest level, out of range values included.
    check(FromSettings(ScalingFilter::NearestNeighbor, 0) == Upscaling::Bilinear);
    check(FromSettings(ScalingFilter::Sgsr, 0) == Upscaling::Bilinear);
    check(FromSettings(ScalingFilter::Fsr, -20) == Upscaling::FsrSharp);
    check(FromSettings(ScalingFilter::Fsr, 12) == Upscaling::FsrSharp);
    check(FromSettings(ScalingFilter::Fsr, 13) == Upscaling::FsrBalanced);
    check(FromSettings(ScalingFilter::Fsr, 62) == Upscaling::FsrBalanced);
    check(FromSettings(ScalingFilter::Fsr, 63) == Upscaling::FsrSoft);
    check(FromSettings(ScalingFilter::Fsr, 200) == Upscaling::FsrSoft);
    check(FromSettings(ScalingFilter::Fsr, 9999) == Upscaling::FsrSoft);

    // Stored values from another build fall back to nothing (the loader then uses Bilinear).
    check(!UpscalingFromValue(-1) && !UpscalingFromValue(4) && !UpscalingFromValue(255));

    // Stepping visits every choice once in each direction and wraps.
    for (const bool back : {false, true}) {
        Upscaling current = Upscaling::Bilinear;
        std::size_t visited = 0;
        do {
            current = StepUpscaling(current, back);
            ++visited;
        } while (current != Upscaling::Bilinear && visited < 10);
        check(visited == SelectableUpscalings().size());
    }
    check(StepUpscaling(Upscaling::Bilinear, false) == Upscaling::FsrSharp);
    check(StepUpscaling(Upscaling::Bilinear, true) == Upscaling::FsrSoft);
    check(StepUpscaling(static_cast<Upscaling>(200), false) == Upscaling::Bilinear);

    // boot.cfg values.
    check(ParseUpscaling("bilinear") == Upscaling::Bilinear);
    check(ParseUpscaling("fsr") == Upscaling::FsrBalanced);
    check(ParseUpscaling("fsr-sharp") == Upscaling::FsrSharp);
    check(ParseUpscaling("fsr-balanced") == Upscaling::FsrBalanced);
    check(ParseUpscaling("fsr-soft") == Upscaling::FsrSoft);
    check(!ParseUpscaling("") && !ParseUpscaling("FSR") && !ParseUpscaling("fsr2"));

    // Every choice has its own text in each language.
    for (const auto& language : I18n::UiLanguages()) {
        I18n::SetUiLanguage(language);
        for (const Upscaling a : SelectableUpscalings())
            for (const Upscaling b : SelectableUpscalings())
                check((a == b) == (I18n::Tr(UpscalingText(a)) == I18n::Tr(UpscalingText(b))));
    }

    std::puts("upscaling: settings round trips, nearest levels, stored and boot.cfg values, "
              "stepping and texts PASS");
}
