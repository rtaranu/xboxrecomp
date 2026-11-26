// The same 32 bits as another kind of texel: a depth buffer's as A8R8G8B8, or
// those as depths.
#version 460
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_samplerless_texture_functions : require

layout(set = 0, binding = 1) uniform texture2D images2d[];
layout(set = 0, binding = 1) uniform utexture2D uimages2d[];

layout(push_constant) uniform Push {
    uint image;                 // of the colours or the depths
    uint how;                   // bit 0: to depths; bit 1: floating-point ones;
                                // bit 2: the image has 1 over each
    uint x, y;                  // of the first texel
    uint stencil;
} push;

layout(location = 0) out vec4 color;

const float most = uintBitsToFloat(0x7F7FFF80u);        // nv2a_vk_depth_most

void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy) + ivec2(push.x, push.y);
    vec4 t = texelFetch(images2d[push.image], p, 0);
    uint z;

    if ((push.how & 1u) != 0u) {
        uvec4 c = uvec4(t * 255.0 + 0.5);

        z = c.a << 16 | c.r << 8 | c.g;
        if ((push.how & 2u) == 0u) {
            color = vec4(float(z) / 16777215.0);
        } else {                // in two steps: 1 / most is too small a number
            precise float f = uintBitsToFloat(z << 7) * exp2(-64.0);

            f *= exp2(64.0) / most;
            color = vec4(z >= 0xFF0000u ? 1.0 : f);
        }
        return;
    }
    if ((push.how & 4u) != 0u)
        z = (push.how & 2u) == 0u ? uint(round(min(1.0 / t.r, 16777215.0)))
                                  : floatBitsToUint(min(1.0 / t.r, most)) >> 7;
    else
        z = (push.how & 2u) == 0u ? uint(round(t.r * 16777215.0))
                                  : floatBitsToUint(t.r * most) >> 7;
    z = min(z, 0xFFFFFFu);
    color = vec4(z >> 8 & 0xFFu, z & 0xFFu, texelFetch(uimages2d[push.stencil], p, 0).r, z >> 16)
          / 255.0;
}
