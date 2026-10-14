// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
// Diagnostic only. The input signature order matches linked generic 0/1 varyings.
struct Input {
    noperspective float4 screen : TEXCOORD0;
    noperspective float4 view : TEXCOORD1;
    float4 position : SV_Position;
};
#if MATERIAL_SAMPLES
Texture2D<float4> depth_input : register(t5);
Texture2D<float4> mask_input : register(t6);
SamplerState depth_sampler : register(s5);
SamplerState mask_sampler : register(s6);
#endif
float4 main(Input input) : SV_Target {
#if MATERIAL_SAMPLES
    // Controlled mask probes, not the original PS's projected mask coordinates.
    return float4(depth_input.SampleLevel(depth_sampler, input.screen.xy, 0).xy,
                  mask_input.SampleLevel(mask_sampler, input.screen.xy, 0).x,
                  mask_input.SampleLevel(mask_sampler, input.screen.xy, 9).x);
#else
    return float4(input.screen.xy, input.view.xy);
#endif
}
