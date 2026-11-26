/* A batch's vertices for program.vert (nv2a_vk_vtx.h). */
#include <string.h>
#include "nv2a_vk_vtx.h"

/* ---- the program -------------------------------------------------------- */

static uint32_t field(const uint32_t *ins, int word, int lo, int width)
{
    return (ins[word] >> lo) & ((1u << width) - 1u);
}

/* The fields are nv2a_vsh_interp.c's table. */
void nv2a_vk_program_scan(const uint32_t (*program)[4], uint32_t start,
                          Nv2aVkProgram *out)
{
    uint32_t i;

    memset(out, 0, sizeof *out);
    for (i = start; i < NV2A_VSH_SLOTS; i++) {
        const uint32_t *ins = program[i];
        uint32_t mac = field(ins, 1, 21, 4), ilu = field(ins, 1, 25, 3);
        uint32_t input = 1u << field(ins, 1, 9, 4);

        out->count++;
        if (mac && field(ins, 2, 26, 2) == 2)                       /* A */
            out->inputs |= input;
        if ((mac == 2 || (mac >= 4 && mac <= 12)) && field(ins, 2, 11, 2) == 2)
            out->inputs |= input;                                   /* B */
        if ((mac == 3 || mac == 4 || ilu) && field(ins, 3, 28, 2) == 2)
            out->inputs |= input;                                   /* C */
        if (field(ins, 3, 12, 4) && !field(ins, 3, 11, 1))
            out->writes_constants = 1;
        if (field(ins, 3, 0, 1)) {
            out->complete = 1;
            return;
        }
    }
}

/* ---- the vertices ------------------------------------------------------- */

/* Bytes of one element, as the executor's fetch_attr reads it; 0: a type it
 * does not read. */
static uint32_t element_bytes(uint32_t type, uint32_t size)
{
    uint32_t n = size < 4 ? size : 4;

    switch (type) {
    case 0: case 6: return 4;
    case 1: case 5: return 2u * n;
    case 4:         return n;
    case 2:         return 4u * n;
    default:        return 0;
    }
}

int nv2a_vk_vertices_plan(const Nv2aRawBatch *b, uint32_t inputs, Nv2aVkVertices *v)
{
    uint32_t lo = UINT32_MAX, hi = 0, i, k, n, gathered = 0;
    uint64_t whole = 0;

    v->attrs = v->ranges = 0;
    memset(v->header, 0, sizeof v->header);
    for (i = 0; i < b->index_count; i++) {
        if (b->indices[i] < lo) lo = b->indices[i];
        if (b->indices[i] > hi) hi = b->indices[i];
    }
    v->first = lo;
    for (i = 0; i < 16; i++) {
        const Nv2aBatchAttr *a = &b->attrs[i];
        uint32_t *h = &v->header[8 + 4 * i], bytes = element_bytes(a->type, a->size);
        float none[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        uint64_t end;

        if (!(inputs & (1u << i)))
            continue;
        if (!a->size) {                         /* the current value */
            memcpy(h, a->current, sizeof a->current);
            continue;
        }
        if (!a->stride || !bytes || (!b->inline_data && !a->offset)) {
            memcpy(h, none, sizeof none);       /* as the executor reads it */
            continue;
        }
        end = (uint64_t)a->offset + (uint64_t)hi * a->stride + bytes;
        if (end > 0xFFFFFFFFu || (b->inline_data && end > b->inline_bytes))
            return 0;
        v->header[3] |= 1u << i;
        h[1] = a->stride;
        h[2] = a->type | (a->size < 4 ? a->size : 4u) << 8;
        k = v->attrs++;
        v->attr[k].offset = a->offset;
        v->attr[k].stride = a->stride;
        v->attr[k].bytes = bytes;
        v->attr[k].at = i;                      /* which, until placed */
        gathered += (bytes + 3u) & ~3u;
        /* Its range, kept sorted by address. */
        for (n = v->ranges++; n; n--) {
            if (v->range[n - 1].from <= a->offset + lo * a->stride)
                break;
            v->range[n] = v->range[n - 1];
        }
        v->range[n].from = a->offset + lo * a->stride;
        v->range[n].bytes = (uint32_t)(end - v->range[n].from);
    }

    /* Interleaved attributes are one range. Each starts on a word, so that an
     * address keeps its alignment. */
    for (i = n = 0; i < v->ranges; i++) {
        uint32_t from = v->range[i].from & ~3u;
        uint64_t end = (uint64_t)v->range[i].from + v->range[i].bytes;

        if (n && from <= (uint64_t)v->range[n - 1].from + v->range[n - 1].bytes + 64u) {
            uint64_t old = (uint64_t)v->range[n - 1].from + v->range[n - 1].bytes;
            v->range[n - 1].bytes = (uint32_t)((end > old ? end : old) - v->range[n - 1].from);
        } else {
            v->range[n].from = from;
            v->range[n].bytes = (uint32_t)(end - from);
            n++;
        }
    }
    v->ranges = n;
    for (i = 0; i < n; i++) {
        v->range[i].at = (uint32_t)whole;
        whole += (v->range[i].bytes + 3u) & ~3u;
    }

    /* A few vertices out of a large array: one by one. */
    v->record = gathered;
    v->gather = whole > 4ull * gathered * b->index_count + 4096u;
    whole = v->gather ? (uint64_t)gathered * b->index_count : whole;
    if (whole > (32u << 20))
        return 0;
    v->bytes = (uint32_t)whole + 16u;           /* the shader reads whole words */
    for (k = gathered = 0; k < v->attrs; k++) {
        uint32_t *h = &v->header[8 + 4 * v->attr[k].at];
        uint32_t from = v->attr[k].offset + lo * v->attr[k].stride;

        if (v->gather) {
            v->attr[k].at = h[0] = gathered;
            h[1] = v->record;
            gathered += (v->attr[k].bytes + 3u) & ~3u;
        } else {
            for (i = v->ranges - 1; v->range[i].from > from; i--)
                ;
            h[0] = v->range[i].at + (from - v->range[i].from);
        }
    }
    return 1;
}

void nv2a_vk_vertices_copy(const Nv2aVkVertices *v, const Nv2aRawBatch *b,
                           const uint8_t *mem, uint8_t *out)
{
    uint32_t i, k;

    if (!v->gather) {
        for (i = 0; i < v->ranges; i++)
            memcpy(out + v->range[i].at, mem + v->range[i].from, v->range[i].bytes);
        return;
    }
    for (i = 0; i < b->index_count; i++, out += v->record)
        for (k = 0; k < v->attrs; k++)
            memcpy(out + v->attr[k].at,
                   mem + v->attr[k].offset + (size_t)b->indices[i] * v->attr[k].stride,
                   v->attr[k].bytes);
}
