// The same memory laid out differently: rows of one length, of another, or
// swizzled (the bits of x and y in turn, x first, the longer one's last).
#version 460
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_samplerless_texture_functions : require

layout(set = 0, binding = 1) uniform texture2D images2d[];

layout(push_constant) uniform Push {
    uint image;
    uint to, to_width;          // a layout: bit 0 swizzled, log2 width from
    uint from, from_width;      // bit 8 and height from bit 16; in `to`, the
                                // scale from bit 1
} push;

layout(location = 0) out vec4 color;

uint number_of(uvec2 p, uint how, uint width)
{
    uvec2 bits = uvec2((how >> 8) & 0xFFu, how >> 16);
    uint n = 0u, at = 0u;

    if ((how & 1u) == 0u)
        return p.y * width + p.x;
    for (uint i = 0u; i < max(bits.x, bits.y); i++) {
        if (i < bits.x) n |= ((p.x >> i) & 1u) << at++;
        if (i < bits.y) n |= ((p.y >> i) & 1u) << at++;
    }
    return n;
}

uvec2 place_of(uint n, uint how, uint width)
{
    uvec2 bits = uvec2((how >> 8) & 0xFFu, how >> 16), p = uvec2(0u);
    uint at = 0u;

    if ((how & 1u) == 0u)
        return uvec2(n % width, n / width);
    for (uint i = 0u; i < max(bits.x, bits.y); i++) {
        if (i < bits.x) p.x |= ((n >> at++) & 1u) << i;
        if (i < bits.y) p.y |= ((n >> at++) & 1u) << i;
    }
    return p;
}

void main()
{
    uint k = (push.to >> 1) & 0x7Fu;
    uvec2 p = uvec2(gl_FragCoord.xy);
    uint n = number_of(p / k, push.to, push.to_width);

    p = place_of(n, push.from, push.from_width) * k + p % k;
    color = texelFetch(images2d[push.image], ivec2(p), 0);
}
