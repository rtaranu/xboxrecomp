/*
 * A batch's vertices for program.vert: which of them, where they are and how
 * they are laid out in the buffer the shader reads. No Vulkan.
 */
#ifndef NV2A_VK_VTX_H
#define NV2A_VK_VTX_H

#include <stdint.h>
#include "nv2a_backend_batch.h"

typedef struct {
    uint32_t count;             /* instructions, to the final one */
    uint32_t inputs;            /* bit a: some operation reads attribute a */
    int      complete;          /* 0: no final instruction, nothing to run */
    int      writes_constants;  /* results a later vertex would see */
} Nv2aVkProgram;

void nv2a_vk_program_scan(const uint32_t (*program)[4], uint32_t start,
                          Nv2aVkProgram *out);

#define NV2A_VK_VTX_HEADER 72u          /* words: program.vert's header */

typedef struct {
    uint32_t header[NV2A_VK_VTX_HEADER];    /* addresses are within the data */
    struct { uint32_t from, at, bytes; } range[16];     /* what is read */
    uint32_t ranges;
    uint32_t bytes;             /* of data */
    int      gather;            /* one record a vertex, in the index list's
                                 * order; else the ranges as they are */
    uint32_t first;             /* the lowest index */
    uint32_t record;            /* gather: bytes of one */
    struct { uint32_t offset, stride, bytes, at; } attr[16];
    uint32_t attrs;
} Nv2aVkVertices;

/* 0: the CPU has to transform this batch. Guest addresses in range[] are not
 * checked here; with inline data they are checked against its size. */
int nv2a_vk_vertices_plan(const Nv2aRawBatch *b, uint32_t inputs, Nv2aVkVertices *out);

/* mem: the start of guest memory, or the inline data. */
void nv2a_vk_vertices_copy(const Nv2aVkVertices *v, const Nv2aRawBatch *b,
                           const uint8_t *mem, uint8_t *out);

#endif
