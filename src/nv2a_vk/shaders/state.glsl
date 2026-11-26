// The state that decides what a fragment's program does: constants of the
// pipeline, which a driver makes plain code of, or the batch's own (then any
// pipeline will do). The order is specialized_words()'s, nv2a_vk.c.
layout(constant_id = 1) const bool SPECIALIZED = false;
layout(constant_id = 2) const uint K_CONTROL = 0u;
layout(constant_id = 3) const uint K_STAGE_PROGRAM = 0u;
layout(constant_id = 4) const uint K_OTHER_STAGE = 0u;
layout(constant_id = 5) const uint K_CLIP_PLANE = 0u;
layout(constant_id = 6) const uint K_FLAGS = 0u;
layout(constant_id = 7) const uint K_ALPHA_FUNC = 0u;
layout(constant_id = 8) const uint K_FINAL0 = 0u;
layout(constant_id = 9) const uint K_FINAL1 = 0u;
layout(constant_id = 10) const uint K_DOT_MAPPING = 0u;
layout(constant_id = 11) const uint K_SHADOW_FUNC = 0u;
layout(constant_id = 12) const uint K_COLOR_ICW_0 = 0u;
layout(constant_id = 13) const uint K_COLOR_ICW_1 = 0u;
layout(constant_id = 14) const uint K_COLOR_ICW_2 = 0u;
layout(constant_id = 15) const uint K_COLOR_ICW_3 = 0u;
layout(constant_id = 16) const uint K_COLOR_ICW_4 = 0u;
layout(constant_id = 17) const uint K_COLOR_ICW_5 = 0u;
layout(constant_id = 18) const uint K_COLOR_ICW_6 = 0u;
layout(constant_id = 19) const uint K_COLOR_ICW_7 = 0u;
layout(constant_id = 20) const uint K_ALPHA_ICW_0 = 0u;
layout(constant_id = 21) const uint K_ALPHA_ICW_1 = 0u;
layout(constant_id = 22) const uint K_ALPHA_ICW_2 = 0u;
layout(constant_id = 23) const uint K_ALPHA_ICW_3 = 0u;
layout(constant_id = 24) const uint K_ALPHA_ICW_4 = 0u;
layout(constant_id = 25) const uint K_ALPHA_ICW_5 = 0u;
layout(constant_id = 26) const uint K_ALPHA_ICW_6 = 0u;
layout(constant_id = 27) const uint K_ALPHA_ICW_7 = 0u;
layout(constant_id = 28) const uint K_COLOR_OCW_0 = 0u;
layout(constant_id = 29) const uint K_COLOR_OCW_1 = 0u;
layout(constant_id = 30) const uint K_COLOR_OCW_2 = 0u;
layout(constant_id = 31) const uint K_COLOR_OCW_3 = 0u;
layout(constant_id = 32) const uint K_COLOR_OCW_4 = 0u;
layout(constant_id = 33) const uint K_COLOR_OCW_5 = 0u;
layout(constant_id = 34) const uint K_COLOR_OCW_6 = 0u;
layout(constant_id = 35) const uint K_COLOR_OCW_7 = 0u;
layout(constant_id = 36) const uint K_ALPHA_OCW_0 = 0u;
layout(constant_id = 37) const uint K_ALPHA_OCW_1 = 0u;
layout(constant_id = 38) const uint K_ALPHA_OCW_2 = 0u;
layout(constant_id = 39) const uint K_ALPHA_OCW_3 = 0u;
layout(constant_id = 40) const uint K_ALPHA_OCW_4 = 0u;
layout(constant_id = 41) const uint K_ALPHA_OCW_5 = 0u;
layout(constant_id = 42) const uint K_ALPHA_OCW_6 = 0u;
layout(constant_id = 43) const uint K_ALPHA_OCW_7 = 0u;
layout(constant_id = 44) const uint K_KIND_0 = 0u;
layout(constant_id = 45) const uint K_KIND_1 = 0u;
layout(constant_id = 46) const uint K_KIND_2 = 0u;
layout(constant_id = 47) const uint K_KIND_3 = 0u;
layout(constant_id = 48) const uint K_SAMPLING_0 = 0u;
layout(constant_id = 49) const uint K_SAMPLING_1 = 0u;
layout(constant_id = 50) const uint K_SAMPLING_2 = 0u;
layout(constant_id = 51) const uint K_SAMPLING_3 = 0u;

uint state_control() { return SPECIALIZED ? K_CONTROL : draws[push.index].control; }
uint state_stage_program()
{
    return SPECIALIZED ? K_STAGE_PROGRAM : draws[push.index].stage_program;
}
uint state_other_stage() { return SPECIALIZED ? K_OTHER_STAGE : draws[push.index].other_stage; }
uint state_clip_plane() { return SPECIALIZED ? K_CLIP_PLANE : draws[push.index].clip_plane; }
uint state_flags() { return SPECIALIZED ? K_FLAGS : draws[push.index].flags; }
uint state_alpha_func() { return SPECIALIZED ? K_ALPHA_FUNC : draws[push.index].alpha_func; }
uint state_final0() { return SPECIALIZED ? K_FINAL0 : draws[push.index].final0; }
uint state_final1() { return SPECIALIZED ? K_FINAL1 : draws[push.index].final1; }
uint state_dot_mapping() { return SPECIALIZED ? K_DOT_MAPPING : draws[push.index].dot_mapping; }
uint state_shadow_func() { return SPECIALIZED ? K_SHADOW_FUNC : draws[push.index].shadow_func; }

uint state_color_icw(uint i)
{
    if (!SPECIALIZED)
        return draws[push.index].color_icw[i];
    switch (i) {
    case 0u: return K_COLOR_ICW_0;
    case 1u: return K_COLOR_ICW_1;
    case 2u: return K_COLOR_ICW_2;
    case 3u: return K_COLOR_ICW_3;
    case 4u: return K_COLOR_ICW_4;
    case 5u: return K_COLOR_ICW_5;
    case 6u: return K_COLOR_ICW_6;
    case 7u: return K_COLOR_ICW_7;
    default: return 0u;
    }
}

uint state_alpha_icw(uint i)
{
    if (!SPECIALIZED)
        return draws[push.index].alpha_icw[i];
    switch (i) {
    case 0u: return K_ALPHA_ICW_0;
    case 1u: return K_ALPHA_ICW_1;
    case 2u: return K_ALPHA_ICW_2;
    case 3u: return K_ALPHA_ICW_3;
    case 4u: return K_ALPHA_ICW_4;
    case 5u: return K_ALPHA_ICW_5;
    case 6u: return K_ALPHA_ICW_6;
    case 7u: return K_ALPHA_ICW_7;
    default: return 0u;
    }
}

uint state_color_ocw(uint i)
{
    if (!SPECIALIZED)
        return draws[push.index].color_ocw[i];
    switch (i) {
    case 0u: return K_COLOR_OCW_0;
    case 1u: return K_COLOR_OCW_1;
    case 2u: return K_COLOR_OCW_2;
    case 3u: return K_COLOR_OCW_3;
    case 4u: return K_COLOR_OCW_4;
    case 5u: return K_COLOR_OCW_5;
    case 6u: return K_COLOR_OCW_6;
    case 7u: return K_COLOR_OCW_7;
    default: return 0u;
    }
}

uint state_alpha_ocw(uint i)
{
    if (!SPECIALIZED)
        return draws[push.index].alpha_ocw[i];
    switch (i) {
    case 0u: return K_ALPHA_OCW_0;
    case 1u: return K_ALPHA_OCW_1;
    case 2u: return K_ALPHA_OCW_2;
    case 3u: return K_ALPHA_OCW_3;
    case 4u: return K_ALPHA_OCW_4;
    case 5u: return K_ALPHA_OCW_5;
    case 6u: return K_ALPHA_OCW_6;
    case 7u: return K_ALPHA_OCW_7;
    default: return 0u;
    }
}

uint state_kind(uint i)
{
    if (!SPECIALIZED)
        return draws[push.index].stage[i].kind;
    switch (i) {
    case 0u: return K_KIND_0;
    case 1u: return K_KIND_1;
    case 2u: return K_KIND_2;
    case 3u: return K_KIND_3;
    default: return 0u;
    }
}

uint state_sampling(uint i)
{
    if (!SPECIALIZED)
        return draws[push.index].stage[i].flags;
    switch (i) {
    case 0u: return K_SAMPLING_0;
    case 1u: return K_SAMPLING_1;
    case 2u: return K_SAMPLING_2;
    case 3u: return K_SAMPLING_3;
    default: return 0u;
    }
}
