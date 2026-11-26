// What a vertex shader hands on, whichever way it came by it.
layout(location = 0) out vec4 o_d0;
layout(location = 1) out vec4 o_d1;
layout(location = 2) out float o_fog;
layout(location = 3) out vec4 o_tex[4];
layout(location = 7) flat out vec4 o_flat0;
layout(location = 8) flat out vec4 o_flat1;
layout(location = 9) out vec4 o_b0;
layout(location = 10) out vec4 o_b1;
layout(location = 11) flat out vec4 o_flat_b0;
layout(location = 12) flat out vec4 o_flat_b1;

invariant gl_Position;

// What the two vertex shaders place a vertex with: the same operations in both,
// so that one draw's depths equal another's of the same vertices.
layout(std430, set = 0, binding = 5) readonly buffer Words { uint words[]; };

// Neither infinite nor NaN? By its bits: a driver may assume every float is.
bool finite(float f) { return (floatBitsToUint(f) & 0x7F800000u) != 0x7F800000u; }

vec4 mul(vec4 a, vec4 b)
{
    precise vec4 p = a * b;

    p = mix(p, vec4(0.0), equal(a, vec4(0.0)));
    return mix(p, vec4(0.0), equal(b, vec4(0.0)));
}

// Of what mul() gave.
float sum4(vec4 p)
{
    precise float s = p.x + p.y;

    s = s + p.z;
    s = s + p.w;
    return s;
}

vec4 mad(vec4 a, vec4 b, vec4 c)
{
    precise vec4 p = mul(a, b);

    p = p + c;
    return p;
}

float rcp(float x)
{
    precise float f = x == 0.0 ? uintBitsToFloat(floatBitsToUint(x) & 0x80000000u | 0x7F800000u)
                               : 1.0 / x;

    return f;
}

float rcc(float x)
{
    return uintBitsToFloat(floatBitsToUint(x) & 0x80000000u
        | floatBitsToUint(clamp(abs(rcp(x)), uintBitsToFloat(0x1F800000u),
                                uintBitsToFloat(0x5F800000u))));
}

// With the surface's anti-aliasing scale, and no image for what is no number.
vec4 placed(vec4 pos, uint scale_at)
{
    pos.x *= uintBitsToFloat(words[scale_at]);
    pos.y *= uintBitsToFloat(words[scale_at + 1u]);
    return finite(pos.x) && finite(pos.y) && finite(pos.z) && finite(pos.w) ? pos : vec4(0.0);
}

// The fixed-function fog factor, the rules of fog_factor() in nv2a_pb_exec.c.
float fog_factor(float d)
{
    uint mode = draws[push.index].fog_mode;
    float a = draws[push.index].fog0, b = draws[push.index].fog1, f;

    if ((draws[push.index].flags & 2u) == 0u)
        return 1.0;
    if (isinf(d))
        return mode == 0x801u || mode == 0x802u || mode == 0x803u ? 0.0 : 1.0;
    if (mode == 0x800u || mode == 0x802u)
        f = a + exp2(d * b * 16.0) - 1.5;
    else if (mode == 0x801u || mode == 0x803u)
        f = a + exp2(-d * d * b * b * 32.0) - 1.5;
    else
        f = a + d * b - 1.0;
    if (mode == 0x802u || mode == 0x803u || mode == 0x804u)
        f = abs(f);
    return isnan(f) ? 1.0 : clamp(f, 0.0, 1.0);
}

// The position is in surface pixels with the clip w left in pos.w, so
// multiplying by w gives clip space back and the GPU clips and interpolates.
void emit(vec4 pos, vec4 d0, vec4 d1, vec4 b0, vec4 b1, float size, float fog, vec4 tex[4])
{
    vec4 s = draws[push.index].surface;
    // Positions truncate to 1/16 pixel. Inferred, not measured: D3D's viewport
    // adds 0.53125, a pixel centre and half of such a step, and without it a
    // quad that ends on a pixel's edge draws that pixel too. In front of the
    // eye only: behind it the sign of the step would turn.
    uint larger = draws[push.index].flags >> 16 & 0xFFu;
    float step = larger != 0u && pos.w != 1.0 ? 16.0 * float(larger) : 16.0;
    vec2 p = pos.w > 0.0 ? floor(pos.xy * step) / step : pos.xy;
    // Larger than the title's: a picture's edge through the centre of a pixel
    // of its size (Direct3D's, without the half pixel) goes through the centre
    // of one that is drawn, or the first row and column are left out. With
    // several samples, through the edge of one: none is covered in part.
    bool many = (draws[push.index].flags >> 12 & 3u) != 0u;
    if ((larger != 0u || many) && pos.w == 1.0)
        p = mix(p, p - 0.5 + (many ? 0.0 : 0.5 / float(larger)), equal(fract(p), vec2(0.5)));

#ifdef RECIPROCAL
    // What the GPU interpolates is as linear across a triangle as 1 / w: that is
    // the depth of a w-buffer. Another draw's depth is taken so at the vertices.
    float add = draws[push.index].offset, z;
    if ((draws[push.index].flags & 1u << 25) != 0u)
        z = add != 0.0 && pos.w > 16.0 * abs(add) ? pos.w / (pos.w + add) : 1.0;
    else
        z = pos.w / max(pos.z + add, 1.0);
#else
    float z = pos.z * s.z * ((draws[push.index].flags & 1024u) != 0u ? exp2(-64.0) : 1.0) * pos.w;
#endif

    gl_Position = vec4((p.x * s.x - 1.0) * pos.w, (p.y * s.y - 1.0) * pos.w, z, pos.w);
    // Larger than 0: a driver may drop a point of no size, or NaN.
    gl_PointSize = (draws[push.index].flags & 256u) != 0u && size > 0.0
                 ? size * float(max(larger, 1u)) : s.w;
    o_d0 = d0;
    o_d1 = d1;
    o_flat0 = d0;
    o_flat1 = d1;
    o_b0 = o_flat_b0 = TWO_SIDED ? b0 : vec4(0.0);
    o_b1 = o_flat_b1 = TWO_SIDED ? b1 : vec4(0.0);
    o_fog = fog_factor(fog);
    for (int i = 0; i < 4; i++)
        o_tex[i] = tex[i];
}
