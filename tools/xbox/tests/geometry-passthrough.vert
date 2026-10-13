// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#version 450
layout(location=0) out vec4 a;
layout(location=31) out vec4 b;
void main() {
    gl_Position = vec4(float(gl_VertexIndex), 0, 0.25, 1);
    a = vec4(1, 2, 3, 4);
    b = vec4(5, 6, 7, 8);
}
