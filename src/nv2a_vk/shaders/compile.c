/*
 * GLSL to SPIR-V in a C header, with glslang's library (CMakeLists.txt).
 *
 *   nv2a-vk-compile <folder of the sources> <header to write>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glslang/Include/glslang_c_interface.h>
#include <glslang/Public/resource_limits_c.h>

static char *read_file(const char *dir, const char *name)
{
    char path[1024];
    FILE *f;
    char *s;
    size_t n;

    snprintf(path, sizeof path, "%s/%s", dir, name);
    f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "cannot read %s\n", path);
        exit(1);
    }
    s = (char *)calloc(1, 1 << 20);
    n = fread(s, 1, (1 << 20) - 1, f);
    s[n] = 0;
    fclose(f);
    return s;
}

/* A source with its #include lines replaced by the files they name. */
static char *source(const char *dir, const char *name)
{
    static const char tag[] = "#include \"";
    char *s = read_file(dir, name), *at;

    while ((at = strstr(s, tag)) != NULL) {
        char *file = at + sizeof tag - 1, *end = strchr(file, '"'), *inc, *out;

        if (!end) {
            fprintf(stderr, "%s: bad include\n", name);
            exit(1);
        }
        *end = 0;
        inc = read_file(dir, file);
        out = (char *)malloc(strlen(inc) + strlen(end + 1) + (size_t)(at - s) + 1);
        memcpy(out, s, (size_t)(at - s));
        strcpy(out + (at - s), inc);
        strcat(out, end + 1);
        free(s);
        free(inc);
        s = out;
    }
    return s;
}

static void one(FILE *out, const char *dir, const char *name, const char *symbol,
                glslang_stage_t stage, const char *preamble)
{
    glslang_input_t in;
    glslang_shader_t *sh;
    glslang_program_t *pr;
    const unsigned *w;
    size_t n, i;

    memset(&in, 0, sizeof in);
    in.language = GLSLANG_SOURCE_GLSL;
    in.stage = stage;
    in.client = GLSLANG_CLIENT_VULKAN;
    in.client_version = GLSLANG_TARGET_VULKAN_1_3;
    in.target_language = GLSLANG_TARGET_SPV;
    in.target_language_version = GLSLANG_TARGET_SPV_1_6;
    in.code = source(dir, name);
    in.default_version = 460;
    in.default_profile = GLSLANG_NO_PROFILE;
    in.messages = (glslang_messages_t)(GLSLANG_MSG_SPV_RULES_BIT
                                       | GLSLANG_MSG_VULKAN_RULES_BIT);
    in.resource = glslang_default_resource();

    sh = glslang_shader_create(&in);
    glslang_shader_set_preamble(sh, preamble);
    if (!glslang_shader_preprocess(sh, &in) || !glslang_shader_parse(sh, &in)) {
        fprintf(stderr, "%s:\n%s\n", name, glslang_shader_get_info_log(sh));
        exit(1);
    }
    pr = glslang_program_create();
    glslang_program_add_shader(pr, sh);
    if (!glslang_program_link(pr, in.messages)) {
        fprintf(stderr, "%s:\n%s\n", name, glslang_program_get_info_log(pr));
        exit(1);
    }
    glslang_program_SPIRV_generate(pr, stage);
    if (glslang_program_SPIRV_get_messages(pr))
        fprintf(stderr, "%s: %s\n", name, glslang_program_SPIRV_get_messages(pr));
    n = glslang_program_SPIRV_get_size(pr);
    w = glslang_program_SPIRV_get_ptr(pr);
    fprintf(out, "static const uint32_t %s[%u] = {", symbol, (unsigned)n);
    for (i = 0; i < n; i++)
        fprintf(out, "%s0x%08X,", i % 8 ? "" : "\n", w[i]);
    fprintf(out, "\n};\n");
    printf("%s: %u words\n", name, (unsigned)n);
}

int main(int argc, char **argv)
{
    FILE *out;

    if (argc != 3)
        return 2;
    glslang_initialize_process();
    out = fopen(argv[2], "wb");
    if (!out)
        return 2;
    fprintf(out, "/* Generated from shaders/ by nv2a-vk-compile "
                 "(shaders/CMakeLists.txt): do not edit. */\n");
    one(out, argv[1], "batch.vert", "nv2a_vk_vert_spv", GLSLANG_STAGE_VERTEX, "");
    one(out, argv[1], "program.vert", "nv2a_vk_prog_spv", GLSLANG_STAGE_VERTEX, "");
    one(out, argv[1], "batch.vert", "nv2a_vk_w_vert_spv", GLSLANG_STAGE_VERTEX,
        "#define RECIPROCAL\n");
    one(out, argv[1], "program.vert", "nv2a_vk_w_prog_spv", GLSLANG_STAGE_VERTEX,
        "#define RECIPROCAL\n");
    one(out, argv[1], "batch.frag", "nv2a_vk_frag_spv", GLSLANG_STAGE_FRAGMENT, "");
    one(out, argv[1], "batch.frag", "nv2a_vk_depth_frag_spv", GLSLANG_STAGE_FRAGMENT,
        "#define DEPTH_REPLACE\n");
    one(out, argv[1], "batch.frag", "nv2a_vk_cutout_frag_spv", GLSLANG_STAGE_FRAGMENT,
        "#define CUTOUT\n");
    one(out, argv[1], "batch.frag", "nv2a_vk_each_frag_spv", GLSLANG_STAGE_FRAGMENT,
        "#define EACH\n");
    one(out, argv[1], "batch.frag", "nv2a_vk_each_depth_frag_spv", GLSLANG_STAGE_FRAGMENT,
        "#define EACH\n#define DEPTH_REPLACE\n");
    one(out, argv[1], "rearrange.vert", "nv2a_vk_rearrange_vert_spv", GLSLANG_STAGE_VERTEX, "");
    one(out, argv[1], "rearrange.frag", "nv2a_vk_rearrange_frag_spv", GLSLANG_STAGE_FRAGMENT, "");
    one(out, argv[1], "depths.frag", "nv2a_vk_depths_frag_spv", GLSLANG_STAGE_FRAGMENT, "");
    return fclose(out) != 0;
}
