// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Replaces the deko3d examples' main menu (devkitPro switch-examples, graphics/deko3d/deko_examples)
// for the headless UWP boot: there is no input to pick an entry, so each NRO runs one example,
// chosen at build time with -DEDEN_DEKO_EXAMPLE=<n>. See tools/xbox/build-deko3d-examples.ps1.

#ifndef EDEN_DEKO_EXAMPLE
#error "build with DEFINES=-DEDEN_DEKO_EXAMPLE=<1..9>"
#endif

#define EDEN_CONCAT_(a, b) a##b
#define EDEN_CONCAT(a, b) EDEN_CONCAT_(a, b)

#if EDEN_DEKO_EXAMPLE < 10
#define EDEN_EXAMPLE_FUNC EDEN_CONCAT(Example0, EDEN_DEKO_EXAMPLE)
#else
#define EDEN_EXAMPLE_FUNC EDEN_CONCAT(Example, EDEN_DEKO_EXAMPLE)
#endif

void EDEN_EXAMPLE_FUNC(void);

int main(int argc, char* argv[]) {
    EDEN_EXAMPLE_FUNC();
    return 0;
}
