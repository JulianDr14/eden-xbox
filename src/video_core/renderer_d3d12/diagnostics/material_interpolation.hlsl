// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
// Isolated clip/interpolation controls. Never bound for a guest draw.
struct Vertex {
    float4 screen : TEXCOORD0;
    float4 view : TEXCOORD1;
    float4 position : SV_Position;
};

[maxvertexcount(3)]
void geometry(triangle Vertex vertices[3], inout TriangleStream<Vertex> output) {
    [unroll] for (uint i = 0; i < 3; ++i) {
        Vertex vertex = vertices[i];
#if MATERIAL_LINEAR
        // At clipping: lerp(a0*w0,a1*w1,t)/lerp(w0,w1,t) is the
        // screen-linear attribute, including edges crossing the camera plane.
        vertex.screen *= vertex.position.w;
        vertex.view *= vertex.position.w;
#endif
        output.Append(vertex);
    }
}

struct Pixel {
    float4 screen : TEXCOORD0;
    float4 view : TEXCOORD1;
    float4 position : SV_Position;
};
#if MATERIAL_SAMPLES
Texture2D<float4> depth_input : register(t5);
Texture2D<float4> mask_input : register(t6);
SamplerState depth_sampler : register(s5);
SamplerState mask_sampler : register(s6);
#endif
float4 pixel(Pixel input) : SV_Target {
    // D3D SV_Position.w is interpolated clip W (not GL FragCoord.w).
    float2 screen = input.screen.xy / input.position.w;
    float2 view = input.view.xy / input.position.w;
#if MATERIAL_SAMPLES
    return float4(depth_input.SampleLevel(depth_sampler, screen, 0).xy,
                  mask_input.SampleLevel(mask_sampler, screen, 0).x,
                  mask_input.SampleLevel(mask_sampler, screen, 9).x);
#else
    return float4(screen, view);
#endif
}
