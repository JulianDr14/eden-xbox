// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
//
// GATE-2 JIT-liveness payload for the Xbox/UWP headless boot (src/eden_uwp).
//
// This is ordinary Switch homebrew: unencrypted, built from source here, and it touches nothing
// but kernel syscalls. It needs NO keys, NO firmware and NO commercial content - which is the
// whole point, both for the house rule and because HLE covers everything it calls.
//
// It runs a small loop, checks the answer, and only then emits the sentinel that
// src/eden_uwp/uwp_boot.cpp watches for through Kernel::Svc::SetDebugStringObserver.

#include <switch.h>

#define SENTINEL   "EDEN_XBOX_JIT_ALIVE"
#define MISCOMPILE "EDEN_XBOX_JIT_MISCOMPILE"

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

int main(void) {
    const u64 result = SumTo(g_n); // 1000 * 1001 / 2 == 500500

    // Distinguish "the JIT ran guest code correctly" from "the JIT ran guest code and got it
    // wrong" - the second is a real and very different bug, and silently claiming liveness for
    // it would make GATE 2 meaningless.
    if (result == 500500) {
        svcOutputDebugString(SENTINEL, sizeof(SENTINEL) - 1);
    } else {
        svcOutputDebugString(MISCOMPILE, sizeof(MISCOMPILE) - 1);
    }
    return 0;
}
