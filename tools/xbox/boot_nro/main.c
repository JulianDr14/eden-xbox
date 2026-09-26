// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
//
// GATE-2/GATE-3 payload for the Xbox/UWP boot (src/eden_uwp).
//
// This is ordinary Switch homebrew: unencrypted, built from source here, and it touches nothing
// but kernel syscalls and the HLE display services. It needs NO keys, NO firmware and NO
// commercial content - which is the whole point, both for the house rule and because HLE covers
// everything it calls.
//
// 1. GATE 2 (JIT liveness): runs a small loop, checks the answer, and only then emits the
//    sentinel that src/eden_uwp/uwp_boot.cpp watches for through Kernel::Svc::SetDebugStringObserver.
// 2. GATE 3 (first pixels): draws an animated test pattern with the CPU into a libnx framebuffer
//    for a few seconds - no GPU commands, so it only needs the renderer's present path - then
//    emits the GFX_DONE sentinel so the frontend knows the loop ran to the end.

#include <switch.h>

#define SENTINEL   "EDEN_XBOX_JIT_ALIVE"
#define MISCOMPILE "EDEN_XBOX_JIT_MISCOMPILE"
#define GFX_DONE   "EDEN_XBOX_GFX_DONE"

#define FB_WIDTH   1280
#define FB_HEIGHT  720
#define GFX_FRAMES 600 // ~10 s at 60 Hz

// volatile so -O2 cannot constant-fold the loop away: the arithmetic must actually be translated
// and executed by dynarmic on the console, which is the entire thing being proven.
static volatile u64 g_n = 1000;

// A loop with an add, a compare and a backward branch - a real basic block for the JIT to
// translate, not just a lone SVC instruction.
static u64 SumTo(u64 n) {
    u64 acc = 0;
    for (u64 i = 1; i <= n; ++i) {
        acc += i;
    }
    return acc;
}

// Horizontal red ramp, vertical green ramp, blue pulsing with time, and a white bar sweeping left
// to right. A frozen image, swapped channels or a torn/garbled layout are each obvious on a TV.
// The top band is green when the JIT check passed and red when it did not.
static void DrawFrame(u32* pixels, u32 stride, u32 frame, bool jit_ok) {
    const u32 blue = (frame * 4) & 0xFF;
    const u32 bar_x = (frame * 8) % FB_WIDTH;
    const u32 band = jit_ok ? RGBA8_MAXALPHA(0, 200, 0) : RGBA8_MAXALPHA(200, 0, 0);
    for (u32 y = 0; y < FB_HEIGHT; ++y) {
        u32* row = pixels + y * stride;
        if (y < 48) {
            for (u32 x = 0; x < FB_WIDTH; ++x) {
                row[x] = band;
            }
            continue;
        }
        const u32 green = y * 255 / FB_HEIGHT;
        for (u32 x = 0; x < FB_WIDTH; ++x) {
            const bool on_bar = x >= bar_x && x < bar_x + 32;
            row[x] = on_bar ? RGBA8_MAXALPHA(255, 255, 255)
                            : RGBA8_MAXALPHA(x * 255 / FB_WIDTH, green, blue);
        }
    }
}

int main(void) {
    const u64 result = SumTo(g_n); // 1000 * 1001 / 2 == 500500
    const bool jit_ok = result == 500500;

    // Distinguish "the JIT ran guest code correctly" from "the JIT ran guest code and got it
    // wrong" - the second is a real and very different bug, and silently claiming liveness for
    // it would make GATE 2 meaningless.
    if (jit_ok) {
        svcOutputDebugString(SENTINEL, sizeof(SENTINEL) - 1);
    } else {
        svcOutputDebugString(MISCOMPILE, sizeof(MISCOMPILE) - 1);
    }

    Framebuffer fb;
    if (R_SUCCEEDED(framebufferCreate(&fb, nwindowGetDefault(), FB_WIDTH, FB_HEIGHT,
                                      PIXEL_FORMAT_RGBA_8888, 2))) {
        framebufferMakeLinear(&fb);
        for (u32 frame = 0; frame < GFX_FRAMES; ++frame) {
            u32 stride_bytes;
            u32* pixels = (u32*)framebufferBegin(&fb, &stride_bytes);
            DrawFrame(pixels, stride_bytes / sizeof(u32), frame, jit_ok);
            framebufferEnd(&fb);
        }
        framebufferClose(&fb);
    }
    svcOutputDebugString(GFX_DONE, sizeof(GFX_DONE) - 1);
    return 0;
}
