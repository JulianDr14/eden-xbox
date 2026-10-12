// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
// Build (Developer Command Prompt, from the repo root):
//   cl /std:c++20 /EHsc /Zc:preprocessor /utf-8 /W4 /Isrc /Iexternals\FidelityFX-FSR\ffx-fsr
//      tools\xbox\tests\fsr-plan.cpp src\video_core\fsr.cpp
//
// Checks the D3D12 FSR 1 plan (d3d12_fsr_plan.h): Eden's constant setup against AMD's own
// (ffx_fsr1.h, compiled for the CPU), when the present runs FSR at all, and where EASU samples
// for each output pixel, flipped crops included.
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

// AMD's reference; its unused helpers would bury the harness's own warnings.
#pragma warning(push, 0)
#define A_CPU 1
#include "ffx_a.h"
#include "ffx_fsr1.h"
#pragma warning(pop)

#include "video_core/renderer_d3d12/d3d12_fsr_plan.h"

namespace {
void Check(bool ok, const char* what, int line) {
    if (!ok) {
        std::fprintf(stderr, "FAIL line %d: %s\n", line, what);
        std::abort();
    }
}
#define CHECK(expr) Check((expr), #expr, __LINE__)

float AsFloat(u32 bits) {
    return std::bit_cast<float>(bits);
}

/// Eden divides where AMD multiplies by a reciprocal: the two may differ in the last bit.
bool NearlyEqual(u32 eden, u32 amd) {
    const float a = AsFloat(eden);
    const float b = AsFloat(amd);
    return std::abs(a - b) <= 1e-6f * std::max({1.0f, std::abs(a), std::abs(b)});
}

/// Source coordinate (texel units, continuous) that EASU reconstructs for the center of output
/// pixel (x, y): ffx_fsr1.h FsrEasuF takes pp = ip * con0.xy + con0.zw, whose +0.5 is the
/// pixel center in texel space.
std::pair<float, float> EasuSource(const D3D12::Fsr::Plan& plan, u32 x, u32 y) {
    const float px = static_cast<float>(x) * AsFloat(plan.easu[0]) + AsFloat(plan.easu[2]);
    const float py = static_cast<float>(y) * AsFloat(plan.easu[1]) + AsFloat(plan.easu[3]);
    return {px + 0.5f, py + 0.5f};
}

bool Near(float a, float b) {
    return std::abs(a - b) <= 1e-3f;
}
} // namespace

int main() {
    using namespace D3D12::Fsr;

    // 1. Eden's FsrEasuConOffset (video_core/fsr.cpp) against AMD's, flipped viewports included.
    std::size_t easu_cases = 0;
    const float sizes[][2] = {{1, 1}, {640, 360}, {1280, 720}, {1366, 768}, {1920, 1080}};
    const float outputs[][2] = {{1920, 1080}, {2560, 1440}, {3840, 2160}, {1281, 721}};
    const float offsets[] = {0.0f, 3.0f, 640.0f};
    for (const auto& in : sizes) {
        for (const auto& out : outputs) {
            for (const float offset : offsets) {
                for (const float sign_x : {1.0f, -1.0f}) {
                    for (const float sign_y : {1.0f, -1.0f}) {
                        u32 eden[16]{};
                        AU1 amd[16]{};
                        const float vw = sign_x * in[0];
                        const float vh = sign_y * in[1];
                        FSR::FsrEasuConOffset(eden, eden + 4, eden + 8, eden + 12, vw, vh, in[0],
                                              in[1], out[0], out[1], offset, offset);
                        ::FsrEasuConOffset(amd, amd + 4, amd + 8, amd + 12, vw, vh, in[0], in[1],
                                           out[0], out[1], offset, offset);
                        for (int i = 0; i < 16; ++i) {
                            CHECK(NearlyEqual(eden[i], amd[i]));
                        }
                        ++easu_cases;
                    }
                }
            }
        }
    }

    // 2. FsrRcasCon for every slider value: the fp16 pair comes from the same tables, so the
    //    constants must match exactly.
    for (int slider = 0; slider <= MAX_SHARPENING_SLIDER; ++slider) {
        u32 eden[4]{};
        AU1 amd[4]{};
        FSR::FsrRcasCon(eden, SharpnessStops(slider));
        ::FsrRcasCon(amd, SharpnessStops(slider));
        for (int i = 0; i < 4; ++i) {
            CHECK(eden[i] == amd[i]);
        }
    }
    CHECK(SharpnessStops(-10) == 0.0f);
    CHECK(SharpnessStops(25) == 0.25f);
    CHECK(SharpnessStops(MAX_SHARPENING_SLIDER + 50) == 2.0f);

    // 3. When the present runs FSR.
    const Common::Rectangle<f32> full{0.0f, 0.0f, 1.0f, 1.0f};
    CHECK(!MakePlan(0, 720, full, 1920, 1080, 25));
    CHECK(!MakePlan(1280, 720, full, 0, 1080, 25));
    CHECK(!MakePlan(1280, 720, {0.5f, 0.5f, 0.5f, 1.0f}, 1920, 1080, 25)); // empty crop
    CHECK(!MakePlan(1280, 720, {0.0f, 0.0f, NAN, 1.0f}, 1920, 1080, 25));
    CHECK(!MakePlan(1920, 1080, full, 1920, 1080, 25)); // same size: nothing to reconstruct
    CHECK(!MakePlan(1920, 1080, full, 1280, 720, 25));  // EASU does not minify
    CHECK(!MakePlan(1280, 720, full, 1920, 700, 25));   // minifies one axis
    {
        const auto plan = MakePlan(1280, 720, full, 1920, 1080, 25);
        CHECK(plan && plan->output_width == 1920 && plan->output_height == 1080);
        CHECK(plan->scale_x == 1.5f && plan->scale_y == 1.5f && plan->WithinTunedRange());
        CHECK(plan->sharpness == 0.25f && AsFloat(plan->rcas[0]) == std::exp2(-0.25f));
    }
    {
        const auto plan = MakePlan(1280, 720, full, 3840, 2160, 0); // 3x: beyond EASU's tuning
        CHECK(plan && plan->scale_x == 3.0f && !plan->WithinTunedRange());
    }
    {
        const auto plan = MakePlan(1280, 720, full, 1280, 1080, 0); // stretches one axis only
        CHECK(plan && plan->scale_x == 1.0f && plan->scale_y == 1.5f);
    }

    // 4. Where EASU samples: the screen's top edge samples crop.top (Tegra::NormalizeCrop), for
    //    plain, flipped and partial crops, at the first and last output pixels.
    struct Case {
        Common::Rectangle<f32> crop;
        u32 tex_w, tex_h, out_w, out_h;
    };
    const Case cases[] = {
        {full, 1280, 720, 1920, 1080},
        {{0.0f, 1.0f, 1.0f, 0.0f}, 1280, 720, 1920, 1080}, // FlipV
        {{1.0f, 0.0f, 0.0f, 1.0f}, 1280, 720, 1920, 1080}, // FlipH
        {{0.0f, 0.0f, 0.5f, 0.75f}, 1920, 1088, 1920, 1080}, // guest image larger than shown
        {full, 1280, 720, 3840, 2160},
    };
    for (const Case& c : cases) {
        const auto plan = MakePlan(c.tex_w, c.tex_h, c.crop, c.out_w, c.out_h, 25);
        CHECK(plan.has_value());
        const float w = static_cast<float>(c.tex_w);
        const float h = static_cast<float>(c.tex_h);
        const float step_x = (c.crop.right - c.crop.left) * w / static_cast<float>(c.out_w);
        const float step_y = (c.crop.bottom - c.crop.top) * h / static_cast<float>(c.out_h);
        for (const u32 x : {0u, c.out_w - 1}) {
            for (const u32 y : {0u, c.out_h - 1}) {
                const auto [sx, sy] = EasuSource(*plan, x, y);
                CHECK(Near(sx, c.crop.left * w + (static_cast<float>(x) + 0.5f) * step_x));
                CHECK(Near(sy, c.crop.top * h + (static_cast<float>(y) + 0.5f) * step_y));
            }
        }
    }

    std::printf("FSR plan: %zu EASU constant sets and %d RCAS sharpness values match AMD's "
                "ffx_fsr1.h; plan selection and EASU sampling of %zu crops PASS\n",
                easu_cases, MAX_SHARPENING_SLIDER + 1, std::size(cases));
}
