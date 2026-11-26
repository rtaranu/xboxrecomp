// Texture stages, register combiners, alpha test: one shader that reads the
// title's register values and does what they say.
//
// The combiners follow the OpenGL extensions NV_register_combiners and
// NV_register_combiners2, the stage programs NV_texture_shader. The bit
// layouts are envytools' (rnndb nv10_3d.xml, nv20_3d.xml, nv_3ddefs.xml, MIT);
// what the NV2A adds to the extensions (the final combiner's own constants,
// blue to alpha) is xboxdevwiki's NV2A/Pixel_Combiner page.
#version 460
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_demote_to_helper_invocation : require
#include "draw.glsl"
#include "state.glsl"

layout(set = 0, binding = 1) uniform texture2D   images2d[];
layout(set = 0, binding = 2) uniform texture3D   images3d[];
layout(set = 0, binding = 3) uniform textureCube cubes[];
layout(set = 0, binding = 4) uniform sampler     samplers[];

#ifdef EACH
layout(location = 0) in vec4 i_d0;
layout(location = 1) in vec4 i_d1;
layout(location = 2) in float i_fog;
layout(location = 3) in vec4 i_tex[4];
layout(location = 9) in vec4 i_b0;
layout(location = 10) in vec4 i_b1;
#else
layout(location = 0) in vec4 d0;
layout(location = 1) in vec4 d1;
layout(location = 2) in float fog;
layout(location = 3) in vec4 tex[4];
layout(location = 9) in vec4 b0;
layout(location = 10) in vec4 b1;
#endif
layout(location = 7) flat in vec4 flat0;
layout(location = 8) flat in vec4 flat1;
layout(location = 11) flat in vec4 flat_b0;
layout(location = 12) flat in vec4 flat_b1;

layout(location = 0) out vec4 color;

#ifdef EACH
// For each sample, at its place, and with as much detail as that many pixels
// would have. But a flat picture's pixels stay what they are.
vec4 d0, d1, b0, b1, tex[4];
float fog, finer;

void inputs()
{
    vec2 none = vec2(0.0);

    if (abs(gl_FragCoord.w - 1.0) < 1e-6) {
        finer = 0.0;
        d0 = interpolateAtOffset(i_d0, none);
        d1 = interpolateAtOffset(i_d1, none);
        b0 = interpolateAtOffset(i_b0, none);
        b1 = interpolateAtOffset(i_b1, none);
        fog = interpolateAtOffset(i_fog, none);
        tex[0] = interpolateAtOffset(i_tex[0], none);
        tex[1] = interpolateAtOffset(i_tex[1], none);
        tex[2] = interpolateAtOffset(i_tex[2], none);
        tex[3] = interpolateAtOffset(i_tex[3], none);
        return;
    }
    // Not a cut-out's, whose alpha is another one in a finer level.
    finer = (state_flags() & 0xC000u) != 0u ? 0.0 : -0.5 * float(state_flags() >> 12 & 3u);
    d0 = interpolateAtSample(i_d0, gl_SampleID);
    d1 = interpolateAtSample(i_d1, gl_SampleID);
    b0 = interpolateAtSample(i_b0, gl_SampleID);
    b1 = interpolateAtSample(i_b1, gl_SampleID);
    fog = interpolateAtSample(i_fog, gl_SampleID);
    tex[0] = interpolateAtSample(i_tex[0], gl_SampleID);
    tex[1] = interpolateAtSample(i_tex[1], gl_SampleID);
    tex[2] = interpolateAtSample(i_tex[2], gl_SampleID);
    tex[3] = interpolateAtSample(i_tex[3], gl_SampleID);
}
#define FINER , finer
#else
#define FINER
#endif

vec4 argb(uint c)
{
    return unpackUnorm4x8(c).bgra;
}

// ---- texture stages ------------------------------------------------------

vec4 t[4];
float dots[4];          // of the stages that are a dot product

// SET_SHADOW_COMPARE_FUNC: the coordinate compared with the depth that is there.
// One of four neighbours or, with a linear filter, how many of them pass.
vec4 compared(float ref, vec4 z)
{
    switch (state_shadow_func() & 7u) {
    case 0u: return vec4(0.0);
    case 1u: return vec4(greaterThan(vec4(ref), z));
    case 2u: return vec4(equal(vec4(ref), z));
    case 3u: return vec4(greaterThanEqual(vec4(ref), z));
    case 4u: return vec4(lessThan(vec4(ref), z));
    case 5u: return vec4(notEqual(vec4(ref), z));
    case 6u: return vec4(lessThanEqual(vec4(ref), z));
    default: return vec4(1.0);
    }
}

// The depths over the largest, of a buffer that holds 1 over each too.
vec4 there(Stage s, vec4 z)
{
    return (s.flags & 16u) == 0u ? z : min(s.lum.x * s.lum.y / z, vec4(1.0));
}

vec4 shadow(Stage s, vec2 uv, float ref)
{
    ref = clamp(ref * s.lum.x * s.lum.y, 0.0, 1.0);
    if ((s.flags & 8u) == 0u)
        return compared(ref, there(s, texture(sampler2D(images2d[s.image], samplers[s.smp]),
                                              uv).rrrr));
    vec2 size = vec2(textureSize(sampler2D(images2d[s.image], samplers[s.smp]), 0));
    // The corner they share, or the GPU's rounding may choose other texels than
    // the weights are for.
    vec2 at = uv * size - 0.5, f = fract(at);

    uv = (floor(at) + 1.0) / size;
    vec4 p = compared(ref, there(s, textureGather(sampler2D(images2d[s.image], samplers[s.smp]),
                                                  uv)));
    vec2 row = mix(p.wx, p.zy, f.x);
    return vec4(mix(row.x, row.y, f.y));
}

vec4 sample_stage(uint st, vec3 c)
{
    Stage s = draws[push.index].stage[st];
    s.kind = state_kind(st);
    s.flags = state_sampling(st);
    vec4 v;

    switch (s.kind) {
    case 1u: {
        vec2 uv = c.xy * s.scale;
        if ((s.flags & 2u) != 0u)
            uv += (1.0 - s.scale) * 0.5;    // of 0..1, the border left over
        if ((s.flags & 4u) != 0u)
            v = shadow(s, uv, c.z);
        else
            v = texture(sampler2D(images2d[s.image], samplers[s.smp]), uv FINER);
        break;
    }
    case 2u: v = texture(sampler3D(images3d[s.image], samplers[s.smp]), c FINER); break;
    case 3u: v = texture(samplerCube(cubes[s.image], samplers[s.smp]), c FINER); break;
    default: return vec4(1.0);          // unusable: multiplies through
    }
    if ((s.flags & 1u) != 0u && v.a == 0.0)
        demote;
    return v;
}

// SET_SHADER_OTHER_STAGE_INPUT: which earlier stage a dependent one reads.
uint previous(uint st)
{
    uint w = state_other_stage();

    return st == 3u ? min((w >> 20) & 0xFu, 2u)
         : st == 2u ? min((w >> 16) & 0xFu, 1u) : 0u;
}

// SET_DOT_RGBMAPPING: what a dot product's vector is, of the texel it reads.
// Four bits each from stage 1 up, seen in titles' programs.
vec3 mapped(uint st)
{
    vec4 p = t[previous(st)];
    vec3 v = p.rgb * 255.0, two = v - 256.0 * step(127.5, v);   // two's complement

    switch ((state_dot_mapping() >> (4u * st - 4u)) & 7u) {
    case 1u: return (v - 128.0) / 127.0;
    case 2u: return (2.0 * two + 1.0) / 255.0;
    case 3u: return max(two / 127.0, -1.0);
    case 4u: return vec3((p.a * 256.0 + p.r) / 257.0, (p.g * 256.0 + p.b) / 257.0, 1.0);
    default: return p.rgb;
    }
}

float dotted(uint st)
{
    return st == 0u ? 0.0 : dot(tex[st].xyz, mapped(st));
}

// E mirrored at N, with both as long as they are.
vec3 reflected(vec3 n, vec3 e)
{
    return 2.0 * dot(n, e) / dot(n, n) * n - e;
}

void run_stage(uint st)
{
    uint mode = (state_stage_program() >> (st * 5u)) & 0x1Fu;
    vec4 c = tex[st];

    if (st == 3u && (state_flags() & 512u) != 0u)
        c = vec4(gl_PointCoord, 0.0, 1.0);
    float q = c.w != 0.0 ? c.w : 1.0;

    switch (mode) {
    case 0u:                                            // none
        t[st] = vec4(0.0, 0.0, 0.0, 1.0);
        break;
    case 1u:                                            // 2D, projective
        t[st] = sample_stage(st, vec3(c.xy / q, 0.0));
        break;
    case 2u:                                            // 3D, projective
        t[st] = sample_stage(st, c.xyz / q);
        break;
    case 3u:                                            // cube map
        t[st] = sample_stage(st, c.xyz);
        break;
    case 4u:                                            // pass through
        t[st] = clamp(c, 0.0, 1.0);
        break;
    case 5u: {                                          // cull fragment
        uint less = (state_clip_plane() >> (st * 4u)) & 0xFu;
        for (uint j = 0u; j < 4u; j++)
            if ((((less >> j) & 1u) != 0u) == (c[j] >= 0.0))
                demote;
        t[st] = vec4(0.0);
        break;
    }
    case 6u: case 7u: {                                 // offset 2D, and scale
        // ds and dt are the previous texel's blue and green: where D3D's U and
        // V are in the formats that carry them (V8U8 is G8B8's bits).
        vec4 p = t[previous(st)];
        vec4 m = draws[push.index].stage[st].bump;
        vec2 uv = c.xy / q + vec2(m.x * p.b + m.z * p.g, m.y * p.b + m.w * p.g);
        t[st] = sample_stage(st, vec3(uv, 0.0));
        if (mode == 7u) {
            vec2 l = draws[push.index].stage[st].lum;
            t[st].rgb *= clamp(l.y + l.x * p.r, 0.0, 1.0);
        }
        break;
    }
    case 9u:                                            // two dot products, 2D
        dots[st] = dotted(st);
        t[st] = sample_stage(st, vec3(dots[(st - 1u) & 3u], dots[st], 0.0));
        break;
    case 10u: {                                         // and as a depth, z / w
        dots[st] = dotted(st);
#ifdef DEPTH_REPLACE
        float z = dots[(st - 1u) & 3u] * draws[push.index].surface.z
                * ((state_flags() & 1024u) != 0u ? exp2(-64.0) : 1.0) / dots[st];
        if ((state_flags() & 2048u) == 0u && !(z >= 0.0 && z <= 1.0))
            demote;
        if ((state_flags() & 1u << 24) != 0u)
            z = draws[push.index].surface.z / max(z, draws[push.index].surface.z);
        gl_FragDepth = clamp(z, 0.0, 1.0);
#endif
        t[st] = vec4(0.0);
        break;
    }
    case 11u:                                           // three, the next one's too
        dots[st] = dotted(st);
        t[st] = sample_stage(st, vec3(dots[(st - 1u) & 3u], dots[st],
                                      dotted(min(st + 1u, 3u))));
        break;
    case 12u: case 18u: {                               // three, mirrored
        dots[st] = dotted(st);
        vec3 n = vec3(dots[(st - 2u) & 3u], dots[(st - 1u) & 3u], dots[st]);
        t[st] = sample_stage(st, reflected(n, mode == 18u ? draws[push.index].eye
                               : vec3(tex[(st - 2u) & 3u].w, tex[(st - 1u) & 3u].w, c.w)));
        break;
    }
    case 13u: case 14u:                                 // three: 3D, cube map
        dots[st] = dotted(st);
        t[st] = sample_stage(st, vec3(dots[(st - 2u) & 3u], dots[(st - 1u) & 3u], dots[st]));
        break;
    case 15u:                                           // at a texel's alpha, red
        t[st] = sample_stage(st, vec3(t[previous(st)].ar, 0.0));
        break;
    case 16u:                                           // green, blue
        t[st] = sample_stage(st, vec3(t[previous(st)].gb, 0.0));
        break;
    case 17u:                                           // a dot product
        dots[st] = dotted(st);
        t[st] = vec4(0.0);
        break;
    default:                                            // BRDF, not done
        t[st] = vec4(0.0);
        break;
    }
}

// ---- register combiners --------------------------------------------------

// The registers: 1-2 constants, 3 fog, 4 primary, 5 secondary, 8-11 textures,
// 12-13 spare, 14 spare0 + secondary, 15 E * F; 0 reads zero. No array: one that
// the state indexes is in memory, and took most of a frame's time.
vec4 r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11, r12, r13, r14, r15;

vec4 reg(uint i)
{
    switch (i) {
    case 1u: return r1;
    case 2u: return r2;
    case 3u: return r3;
    case 4u: return r4;
    case 5u: return r5;
    case 6u: return r6;
    case 7u: return r7;
    case 8u: return r8;
    case 9u: return r9;
    case 10u: return r10;
    case 11u: return r11;
    case 12u: return r12;
    case 13u: return r13;
    case 14u: return r14;
    case 15u: return r15;
    default: return vec4(0.0);
    }
}

// What a general combiner can write; the others stay.
void put_rgb(uint i, vec3 v)
{
    switch (i) {
    case 4u: r4.rgb = v; break;
    case 5u: r5.rgb = v; break;
    case 6u: r6.rgb = v; break;
    case 7u: r7.rgb = v; break;
    case 8u: r8.rgb = v; break;
    case 9u: r9.rgb = v; break;
    case 10u: r10.rgb = v; break;
    case 11u: r11.rgb = v; break;
    case 12u: r12.rgb = v; break;
    case 13u: r13.rgb = v; break;
    default: break;
    }
}

void put_alpha(uint i, float a)
{
    switch (i) {
    case 4u: r4.a = a; break;
    case 5u: r5.a = a; break;
    case 6u: r6.a = a; break;
    case 7u: r7.a = a; break;
    case 8u: r8.a = a; break;
    case 9u: r9.a = a; break;
    case 10u: r10.a = a; break;
    case 11u: r11.a = a; break;
    case 12u: r12.a = a; break;
    case 13u: r13.a = a; break;
    default: break;
    }
}

vec3 remap(uint mapping, vec3 e)
{
    switch (mapping) {
    case 0u: return max(e, 0.0);                        // unsigned identity
    case 1u: return 1.0 - clamp(e, 0.0, 1.0);           // unsigned invert
    case 2u: return 2.0 * max(e, 0.0) - 1.0;            // expand normal
    case 3u: return 1.0 - 2.0 * max(e, 0.0);            // expand negate
    case 4u: return max(e, 0.0) - 0.5;                  // half bias normal
    case 5u: return 0.5 - max(e, 0.0);                  // half bias negate
    case 6u: return e;                                  // signed identity
    default: return -e;                                 // signed negate
    }
}

// One input, its eight bits: register 0-3, alpha 4, mapping 5-7.
vec3 in_rgb(uint b)
{
    vec4 v = reg(b & 15u);
    return remap((b >> 5) & 7u, (b & 16u) != 0u ? v.aaa : v.rgb);
}

float in_alpha(uint b)
{
    vec4 v = reg(b & 15u);
    return remap((b >> 5) & 7u, vec3((b & 16u) != 0u ? v.a : v.b)).x;
}

// Bias in bit 15, scale in 16-17 of an output word.
vec3 scaled(uint w, vec3 v)
{
    const float scales[4] = float[](1.0, 2.0, 4.0, 0.5);
    float bias = (w & 0x8000u) != 0u ? -0.5 : 0.0;
    return clamp((v + bias) * scales[(w >> 16) & 3u], -1.0, 1.0);
}

void general(uint i)
{
    uint ic = state_color_icw(i), ia = state_alpha_icw(i);
    uint oc = state_color_ocw(i), oa = state_alpha_ocw(i);
    uint control = state_control();
    vec3 a = in_rgb(ic >> 24), b = in_rgb(ic >> 16);
    vec3 c = in_rgb(ic >> 8), d = in_rgb(ic);
    float aa = in_alpha(ia >> 24), ba = in_alpha(ia >> 16);
    float ca = in_alpha(ia >> 8), da = in_alpha(ia);
    // SET_COMBINER_CONTROL bit 8: the sign of "or" is spare0 alpha's top bit,
    // else its lowest.
    bool second = (control & 0x100u) != 0u
                ? r12.a >= 0.5
                : (uint(clamp(r12.a, 0.0, 1.0) * 255.0 + 0.5) & 1u) != 0u;
    vec3 ab = a * b, cd = c * d;
    vec3 sum = (oc & 0x4000u) != 0u ? (second ? cd : ab) : ab + cd;
    float aab = aa * ba, acd = ca * da;
    float asum = (oa & 0x4000u) != 0u ? (second ? acd : aab) : aab + acd;

    if ((oc & 0x2000u) != 0u) ab = vec3(dot(a, b));
    if ((oc & 0x1000u) != 0u) cd = vec3(dot(c, d));
    ab = scaled(oc, ab);
    cd = scaled(oc, cd);
    sum = scaled(oc, sum);
    vec3 alphas = scaled(oa, vec3(aab, acd, asum));

    uint rab = (oc >> 4) & 15u, rcd = oc & 15u, rsum = (oc >> 8) & 15u;
    put_rgb(rab, ab);
    if ((oc & 0x80000u) != 0u) put_alpha(rab, ab.b);        // blue to alpha
    put_rgb(rcd, cd);
    if ((oc & 0x40000u) != 0u) put_alpha(rcd, cd.b);
    put_rgb(rsum, sum);
    put_alpha((oa >> 4) & 15u, alphas.x);
    put_alpha(oa & 15u, alphas.y);
    put_alpha((oa >> 8) & 15u, alphas.z);
}

// The vertex's colours, as they arrive at the combiners (nxdk_pgraph_tests'
// Specular and Specular_back results, taken on the hardware, for the specular
// switches).
vec4 colours[2];

void pick_colours()
{
    uint f = state_flags();
    bool back = TWO_SIDED && !gl_FrontFacing;

    if ((f & 4u) != 0u) {
        colours[0] = back ? flat_b0 : flat0;
        colours[1] = back ? flat_b1 : flat1;
    } else {
        colours[0] = back ? b0 : d0;
        colours[1] = back ? b1 : d1;
    }
    if ((f & 64u) != 0u)
        colours[1] = vec4(0.0, 0.0, 0.0, 1.0);
    if ((f & 128u) != 0u)
        colours[1].a = 1.0;
}

void combine(uint i)
{
    uint control = state_control();

    // Bits 12 and 16: each stage has its own constant, else all use the first
    // stage's.
    r1 = argb(draws[push.index].factor0[(control & 0x1000u) != 0u ? i : 0u]);
    r2 = argb(draws[push.index].factor1[(control & 0x10000u) != 0u ? i : 0u]);
    general(i);
}

vec4 combiners()
{
    uint n = min(state_control() & 0xFu, 8u);
    uint f0 = state_final0(), f1 = state_final1();

    r3 = vec4(argb(draws[push.index].fog_color).bgr, fog);    // R in bits 0-7
    r4 = colours[0];
    r5 = colours[1];
    r6 = r7 = vec4(0.0);
    r8 = t[0];
    r9 = t[1];
    r10 = t[2];
    r11 = t[3];
    r12 = vec4(0.0, 0.0, 0.0, t[0].a);
    r13 = r14 = r15 = vec4(0.0);

    if (SPECIALIZED) {
        if (0u < n) combine(0u);
        if (1u < n) combine(1u);
        if (2u < n) combine(2u);
        if (3u < n) combine(3u);
        if (4u < n) combine(4u);
        if (5u < n) combine(5u);
        if (6u < n) combine(6u);
        if (7u < n) combine(7u);
    } else {
        for (uint i = 0u; i < n; i++)
            combine(i);
    }

    r1 = argb(draws[push.index].final_c0);
    r2 = argb(draws[push.index].final_c1);
    vec3 spare = max(r12.rgb, 0.0), second = max(r5.rgb, 0.0);
    if ((f1 & 0x20u) != 0u) spare = 1.0 - min(spare, 1.0);
    if ((f1 & 0x40u) != 0u) second = 1.0 - min(second, 1.0);
    vec3 total = spare + second;
    if ((f1 & 0x80u) != 0u) total = min(total, 1.0);
    r15 = vec4(in_rgb(f1 >> 24) * in_rgb(f1 >> 16), 0.0);
    r14 = vec4(total, 0.0);
    vec3 a = in_rgb(f0 >> 24), b = in_rgb(f0 >> 16);
    vec3 c = in_rgb(f0 >> 8), d = in_rgb(f0);
    return clamp(vec4(a * b + (1.0 - a) * c + d, in_alpha(f1 >> 8)), 0.0, 1.0);
}

#if defined(CUTOUT) || defined(EACH)
// How many samples pass a test for less (0x201, 0x203) or more: all or none
// where alpha does not change, and the edge between is as wide as a pixel. Or,
// softly, as many as alpha says.
int covered(float a)
{
    uint func = state_alpha_func();
    bool less = func == 0x201u || func == 0x203u;
    int most = 1 << (state_flags() >> 12 & 3u);
    float d = a * 255.0 - float(draws[push.index].alpha_ref);

    d += func == 0x204u || func == 0x203u ? -0.5 : 0.5;
    if (less)
        d = -d;
    float slope = fwidth(a) * 255.0, part = clamp(d / max(slope, 1e-3) + 0.5, 0.0, 1.0);
    if ((state_flags() & 0xC000u) == 16384u && slope > 0.5)
        part = less ? 1.0 - a : a;
    return int(part * float(most) + 0.5);
}
#endif

bool alpha_passes(float a)
{
    uint func = state_alpha_func(), ref = draws[push.index].alpha_ref;
    uint v = uint(a * 255.0 + 0.5);

    switch (func) {
    case 0x200u: return false;
    case 0x201u: return v <  ref;
    case 0x202u: return v == ref;
    case 0x203u: return v <= ref;
    case 0x204u: return v >  ref;
    case 0x205u: return v != ref;
    case 0x206u: return v >= ref;
    default:     return true;
    }
}

// One of the two values the surface can hold nearest to c, as OpenGL has it.
// Which is a 4x4 ordered pattern; the NV2A's own is not known. Whole steps, as
// devices differ in what they make of anything between.
vec3 dither(vec3 c)
{
    uvec2 p = uvec2(gl_FragCoord.xy);
    uint q = p.x ^ p.y;
    uint n = (q & 1u) << 3 | (p.y & 1u) << 2 | (q & 2u) | ((p.y >> 1) & 1u);
    vec3 most = vec3(31.0, (state_flags() & 32u) != 0u ? 63.0 : 31.0, 31.0);

    return floor(c * most + (float(n) + 0.5) / 16.0) / most;
}

void main()
{
#ifdef DEPTH_REPLACE
    gl_FragDepth = gl_FragCoord.z;
#endif
#ifdef EACH
    inputs();
#endif
    if ((state_flags() & 1u << 26) != 0u) {     // SET_CLIP_MIN and _MAX of a w-buffer
        float w = 1.0 / gl_FragCoord.w;
        if (w < draws[push.index].range.x || w > draws[push.index].range.y)
            demote;
    }
    dots = float[](0.0, 0.0, 0.0, 0.0);
    if (SPECIALIZED) {          // no loop, for what its index decides to be constant
        run_stage(0u);
        run_stage(1u);
        run_stage(2u);
        run_stage(3u);
    } else {
        for (uint i = 0u; i < 4u; i++)
            run_stage(i);
    }
    pick_colours();
    if ((state_flags() & 1u) != 0u)
        color = combiners();
    else            // never set up: stage 0 times the diffuse colour
        color = clamp(colours[0] * ((state_stage_program() & 0x1Fu) != 0u
                            ? t[0] : vec4(1.0)), 0.0, 1.0);
#ifdef CUTOUT
    int n = covered(color.a);
    if (n == 0)
        demote;
    gl_SampleMask[0] = (1 << n) - 1;
#else
#ifdef EACH                 // one of the samples its own alpha covers
    uint how = state_flags() >> 14 & 3u;
    if (how == 1u || how == 2u) {
        if (gl_SampleID >= covered(color.a))
            demote;
    } else
#endif
    if (state_alpha_func() != 0u && !alpha_passes(color.a))
        demote;
#endif
    if ((state_flags() & 16u) != 0u)
        color.rgb = dither(color.rgb);
    if ((state_flags() & 8u) != 0u)
        color = color.bgra;
}
