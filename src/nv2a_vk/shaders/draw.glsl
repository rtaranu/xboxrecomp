// One batch's state, as nv2a_vk.c's GpuDraw lays it out (std430). The words
// are the title's own register values unless they say otherwise.
struct Stage {
    uint kind;          // what the image is: 0 none, 1 2D, 2 3D, 3 cube
    uint image;         // into the descriptor array of that kind
    uint smp;
    uint flags;         // 1: alpha kill, 2: the image has four border texels
                        // on each side, 4: depths, 8: four compared, 16: what
                        // is there is 1 over each
    vec2 scale;         // coordinates to normalised ones
    vec2 lum;           // bump luminance: scale, offset; or what a depth is
                        // taken times, both of them
    vec4 bump;          // a1 a2 a3 a4 (NV_texture_shader's offset matrix)
};

struct Draw {
    vec4 surface;       // 2 / width, 2 / height, 1 / depth range, point size
    uint color_icw[8], alpha_icw[8], color_ocw[8], alpha_ocw[8];
    uint factor0[8], factor1[8];
    uint final0, final1, final_c0, final_c1;
    uint control, stage_program, other_stage, clip_plane;
    uint flags;         // 1: combiners set up, 2: fog on, 4: flat shading,
                        // 8: the surface has blue first (B8, G8B8)
                        // 16: dithered to five bits, 32: six for green
                        // 64: no specular colour, 128: its alpha is 1
                        // 256: the program's point size, 512: point sprites
                        // 1024: depths take another 2^-64, 2048: and clamp
                        // from bit 12: log2 of the samples of a pixel
                        // from bit 14: cut-outs, 1: as soft as their alpha, 2: an
                        // edge a pixel wide, 3: a test for each sample
                        // from bit 16: the scale, or 0
                        // 1 << 24: the depth buffer holds 1 over each depth
                        // 1 << 25: the depth is w, 1 << 26: no w outside `range`
    uint alpha_func;    // 0: no test, else the GL compare function
    uint alpha_ref;
    uint fog_color;
    uint fog_mode;
    float fog0, fog1;
    uint dot_mapping;
    vec3 eye;
    uint shadow_func;
    vec2 range;         // of w
    float offset;       // what is added to depths
    float unused;
    Stage stage[4];
};

layout(std430, set = 0, binding = 0) readonly buffer Draws { Draw draws[]; };
// Back faces have colours of their own.
layout(constant_id = 0) const bool TWO_SIDED = false;

layout(push_constant) uniform Push {
    uint index;         // into draws
    uint vertex;        // program.vert: where its header is, in words
} push;
