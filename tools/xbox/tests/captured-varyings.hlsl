// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
// Signature order matches the captured linked VS: TEXCOORD0/1, then SV_Position.
struct Input {
    noperspective float4 screen : TEXCOORD0;
    noperspective float4 view : TEXCOORD1;
    float4 position : SV_Position;
};
float4 main(Input input) : SV_Target {
    return float4(input.screen.xy, input.view.xy);
}
