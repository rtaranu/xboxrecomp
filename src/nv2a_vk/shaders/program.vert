// The title's vertex program, interpreted: its instruction words, constants
// and raw vertex data are read from the buffer, one vertex an invocation.
//
// The operations follow the OpenGL extensions NV_vertex_program and
// NV_vertex_program1_1 (section 2.14.1.10), and their rule that 0 times
// anything is 0. The instruction words are laid out as the table at the top
// of nv2a_vsh_interp.c says. Not in either document and done as there: the
// 0.001 ARL adds, temporary 12 being oPos, an ILU op paired with a MAC op
// writing R1, and what an index out of range reads.
#version 460
#include "draw.glsl"
#include "vertex.glsl"

const float INF = uintBitsToFloat(0x7F800000u);

// A word at any byte address.
uint word_at(uint at)
{
    uint w = words[at >> 2], s = (at & 3u) * 8u;

    return s == 0u ? w : (w >> s) | (words[(at >> 2) + 1u] << (32u - s));
}

// Vertex attribute a: four words of the header. An array: its address, stride,
// type | size << 8. Otherwise the value itself.
vec4 fetch(uint a)
{
    uint h = push.vertex + 8u + 4u * a, at, kind, n, w;
    vec4 v = vec4(0.0, 0.0, 0.0, 1.0);

    if ((words[push.vertex + 3u] >> a & 1u) == 0u)
        return vec4(uintBitsToFloat(words[h]), uintBitsToFloat(words[h + 1u]),
                    uintBitsToFloat(words[h + 2u]), uintBitsToFloat(words[h + 3u]));
    at = words[h] + uint(gl_VertexIndex) * words[h + 1u];
    kind = words[h + 2u] & 0xFFu;
    n = words[h + 2u] >> 8;
    w = word_at(at);
    switch (kind) {
    case 0u:                                    // D3DCOLOR
        return unpackUnorm4x8(w).zyxw;
    case 6u:                                    // packed 11:11:10
        v.x = float(bitfieldExtract(int(w), 0, 11)) / 1023.0;
        v.y = float(bitfieldExtract(int(w), 11, 11)) / 1023.0;
        v.z = float(bitfieldExtract(int(w), 22, 10)) / 511.0;
        return v;
    case 4u: {                                  // unsigned bytes
        vec4 u = unpackUnorm4x8(w);
        return vec4(u.x, n > 1u ? u.y : 0.0, n > 2u ? u.z : 0.0, n > 3u ? u.w : 1.0);
    }
    case 1u: case 5u: {                         // signed shorts
        float d = kind == 1u ? 32767.0 : 1.0;
        v.x = float(bitfieldExtract(int(w), 0, 16)) / d;
        if (n > 1u)
            v.y = float(bitfieldExtract(int(w), 16, 16)) / d;
        if (n > 2u) {
            w = word_at(at + 4u);
            v.z = float(bitfieldExtract(int(w), 0, 16)) / d;
            if (n > 3u)
                v.w = float(bitfieldExtract(int(w), 16, 16)) / d;
        }
        return v;
    }
    default:                                    // floats
        v.x = uintBitsToFloat(w);
        if (n > 1u) v.y = uintBitsToFloat(word_at(at + 4u));
        if (n > 2u) v.z = uintBitsToFloat(word_at(at + 8u));
        if (n > 3u) v.w = uintBitsToFloat(word_at(at + 12u));
        return v;
    }
}

vec4 constant(int c)
{
    uint at;

    if (c < 0 || c >= 192)
        c = 0;
    at = words[push.vertex + 2u] + 4u * uint(c);
    return vec4(uintBitsToFloat(words[at]), uintBitsToFloat(words[at + 1u]),
                uintBitsToFloat(words[at + 2u]), uintBitsToFloat(words[at + 3u]));
}

vec4 operand(vec4 v, uint swizzle, bool negate)
{
    vec4 s = vec4(v[swizzle >> 6 & 3u], v[swizzle >> 4 & 3u], v[swizzle >> 2 & 3u],
                  v[swizzle & 3u]);

    return negate ? -s : s;
}

bvec4 mask_of(uint m)
{
    return bvec4((m & 8u) != 0u, (m & 4u) != 0u, (m & 2u) != 0u, (m & 1u) != 0u);
}

vec4 mac(uint op, vec4 a, vec4 b, vec4 c)
{
    precise vec4 p = mul(a, b);
    precise float s;

    switch (op) {
    case 2u: return p;                                          // MUL
    case 3u: p = a + c; return p;                               // ADD
    case 4u: return mad(a, b, c);                               // MAD
    case 5u: s = p.x + p.y; s = s + p.z; return vec4(s);        // DP3
    case 6u: s = p.x + p.y; s = s + p.z; s = s + b.w; return vec4(s);   // DPH
    case 7u: return vec4(sum4(p));                              // DP4
    case 8u: return vec4(1.0, p.y, a.z, b.w);                   // DST
    case 9u: return mix(b, a, lessThan(a, b));                  // MIN
    case 10u: return mix(b, a, greaterThanEqual(a, b));         // MAX
    case 11u: return vec4(lessThan(a, b));                      // SLT
    case 12u: return vec4(greaterThanEqual(a, b));              // SGE
    default: return a;                                          // MOV
    }
}

vec4 ilu(uint op, vec4 c)
{
    float x = c.x, f;
    uint bits = floatBitsToUint(x);

    switch (op) {
    case 2u: case 3u:                                           // RCP, RCC
        return vec4(op == 3u ? rcc(x) : rcp(x));
    case 4u:                                                    // RSQ
        return vec4(x == 0.0 ? INF : 1.0 / sqrt(abs(x)));
    case 5u:                                                    // EXP
        f = floor(x);
        return vec4(exp2(f), x - f, exp2(x), 1.0);
    case 6u:                                                    // LOG
        bits &= 0x7FFFFFFFu;
        if (bits < 0x00800000u)
            return vec4(-INF, 1.0, -INF, 1.0);
        if (bits >= 0x7F800000u)
            return vec4(INF, 1.0, INF, 1.0);
        return vec4(float(int(bits >> 23) - 127),
                    uintBitsToFloat(bits & 0x007FFFFFu | 0x3F800000u), log2(abs(x)), 1.0);
    case 7u: {                                                  // LIT
        float nh = max(c.y, 0.0), p = clamp(c.w, -127.9961, 127.9961);
        f = nh == 0.0 ? (p == 0.0 ? 1.0 : p > 0.0 ? 0.0 : INF) : exp2(p * log2(nh));
        return vec4(1.0, max(c.x, 0.0), c.x > 0.0 ? f : 0.0, 1.0);
    }
    default:                                                    // MOV
        return c;
    }
}

// To 0..1, NaN to 0.
vec4 colour(vec4 c)
{
    return mix(mix(vec4(0.0), c, greaterThanEqual(c, vec4(0.0))), vec4(1.0),
               greaterThan(c, vec4(1.0)));
}

void main()
{
    vec4 r[13], o[13], pos, tex[4];
    uint at = words[push.vertex], end = at + 4u * words[push.vertex + 1u];
    int a0 = 0;

    for (int i = 0; i < 13; i++) {
        r[i] = i == 12 ? vec4(0.0, 0.0, 0.0, 1.0) : vec4(0.0);
        o[i] = i == 3 || i == 4 || i >= 7 ? vec4(0.0, 0.0, 0.0, 1.0) : vec4(0.0);
    }
    for (; at < end; at += 4u) {
        uint w1 = words[at + 1u], w2 = words[at + 2u], w3 = words[at + 3u];
        uint mop = w1 >> 21 & 15u, iop = w1 >> 25 & 7u;
        uint ma = w2 >> 26 & 3u, mb = w2 >> 11 & 3u, mc = w3 >> 28 & 3u;
        uint ra = w2 >> 28, rb = w2 >> 13 & 15u, rc = (w2 & 3u) << 2 | w3 >> 30;
        uint dst = w3 >> 20 & 15u, outs = w3 >> 12 & 15u;
        vec4 vtx = vec4(0.0), con = vec4(0.0), a, b, c, m = vec4(0.0), s = vec4(0.0);

        // A source is a temporary (1), the attribute (2) or the constant
        // (3, and 0).
        if (ma == 2u || mb == 2u || mc == 2u)
            vtx = fetch(w1 >> 9 & 15u);
        if (ma == 3u || mb == 3u || mc == 3u || ma == 0u || mb == 0u || mc == 0u)
            con = constant(int(w1 >> 13 & 255u) + ((w3 & 2u) != 0u ? a0 : 0));
        a = ma == 1u ? r[ra <= 12u ? ra : 0u] : ma == 2u ? vtx : con;
        b = mb == 1u ? r[rb <= 12u ? rb : 0u] : mb == 2u ? vtx : con;
        c = mc == 1u ? r[rc <= 12u ? rc : 0u] : mc == 2u ? vtx : con;
        a = operand(a, w1 & 255u, (w1 & 0x100u) != 0u);
        b = operand(b, w2 >> 17 & 255u, (w2 & 0x02000000u) != 0u);
        c = operand(c, w2 >> 2 & 255u, (w2 & 0x400u) != 0u);

        if (mop == 13u)                         // ARL: a0 alone
            a0 = int(floor(a.x + 0.001));
        if (mop != 0u)
            m = mac(mop, a, b, c);
        if (iop != 0u)
            s = ilu(iop, c);
        if (mop != 0u && mop != 13u && dst <= 12u)
            r[dst] = mix(r[dst], m, mask_of(w3 >> 24));
        if (iop != 0u) {
            uint d = mop != 0u ? 1u : dst;
            if (d <= 12u)
                r[d] = mix(r[d], s, mask_of(w3 >> 16));
        }
        if (outs != 0u && (w3 & 0x800u) != 0u) {
            uint to = w3 >> 3 & 255u;
            vec4 v = (w3 & 4u) != 0u ? s : m;
            if (to == 0u)
                r[12] = mix(r[12], v, mask_of(outs));
            else if (to < 13u)
                o[to] = mix(o[to], v, mask_of(outs));
        }
    }

    // What transform_vertex() in nv2a_pb_exec.c does with w = 0 and +inf.
    pos = r[12];
    if (floatBitsToUint(pos.w) == 0u || floatBitsToUint(pos.w) == 0x7F800000u)
        pos.w = 1.0;
    pos = placed(pos, push.vertex + 4u);
    for (int i = 0; i < 4; i++)
        tex[i] = o[9 + i];
    emit(pos, colour(o[3]), colour(o[4]), colour(o[7]), colour(o[8]),
         o[6].x * uintBitsToFloat(words[push.vertex + 5u]), o[5].x, tex);
}
