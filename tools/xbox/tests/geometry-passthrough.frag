// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#version 450
layout(location=1) in vec4 value;
layout(location=0) out vec4 color;
void main() { color = vec4(value.x); }
