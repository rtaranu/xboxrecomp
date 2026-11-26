// Vertices the CPU transformed (Nv2aVshOutput), or all of them but the position.
#version 460
#include "draw.glsl"
#include "vertex.glsl"

layout(location = 0) in vec4 pos;
layout(location = 1) in vec4 d0;
layout(location = 2) in vec4 d1;
layout(location = 3) in vec4 fog;
layout(location = 4) in vec4 tex[4];
layout(location = 8) in vec4 b0;
layout(location = 9) in vec4 b1;
layout(location = 10) in vec4 size;

vec4 row(uint at)
{
    return vec4(uintBitsToFloat(words[at]), uintBitsToFloat(words[at + 1u]),
                uintBitsToFloat(words[at + 2u]), uintBitsToFloat(words[at + 3u]));
}

// The fixed function: the composite matrix (with the viewport's scale in it),
// divided by w, and the viewport's offset. As the programs D3D makes end: DP4s,
// RCC and MAD, so a pass drawn with one has the depths of a pass drawn with this.
vec4 fixed_function()
{
    uint at = push.vertex;
    vec4 p;

    for (int i = 0; i < 4; i++)
        p[i] = sum4(mul(pos, row(at + 4u * uint(i))));
    p.xyz = mad(p, vec4(rcc(p.w)), row(at + 16u)).xyz;
    return placed(p, at + 20u);
}

void main()
{
    emit(push.vertex != 0xFFFFFFFFu ? fixed_function() : pos, d0, d1, b0, b1, size.x, fog.x, tex);
}
