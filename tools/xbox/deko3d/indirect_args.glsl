#version 460
// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Example 11 (tools/xbox/deko3d/Example11_EdenIndirect.cpp): writes the arguments of the indirect
// dispatch and draw from the GPU, so Eden sees them as GPU-modified memory.

layout (local_size_x = 1) in;

layout (std430, binding = 0) buffer Args
{
    uvec4 dispatch_args; // x, y, z groups, padding
    uvec4 draw_args;     // vertex count, instance count, first vertex, first instance
} args;

layout (std140, binding = 0) uniform Params
{
    uvec4 counts; // groups, vertices
} u;

void main()
{
    args.dispatch_args = uvec4(u.counts.x, 1, 1, 0);
    args.draw_args = uvec4(u.counts.y, 1, 0, 0);
}
