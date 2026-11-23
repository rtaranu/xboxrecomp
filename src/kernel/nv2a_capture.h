/*
 * Saving what the GPU was asked to do, to draw it again offline.
 *
 * The executor calls these hooks as it goes, whoever draws (its own
 * rasteriser, a back end, or a host renderer with nv2a_pb_set_draws_off). The
 * one user is nv2a_capture.c (RECOMP_FRAME_CAPTURE), which writes a frame's
 * commands and the guest memory they read to a file; tools/nv2a_replay reads
 * it. The file holds the title's own data and is never to be distributed.
 *
 * Everything runs on the executor's thread.
 */
#ifndef NV2A_CAPTURE_H
#define NV2A_CAPTURE_H

#include <stdint.h>
#include "nv2a_backend_batch.h"

typedef struct {
    /* A command, before it is executed. */
    void (*word)(uint32_t subch, uint32_t method, uint32_t param);
    /* A DMA offset and the guest address it was taken to mean. */
    void (*resolved)(uint32_t offset, uint32_t va);
    /* A batch at its END, before anything draws it. With inline data the
     * attributes are the title's registers, not the batch's layout. */
    void (*batch)(const Nv2aRawBatch *b);
    /* A clear, before it is carried out. */
    void (*clear)(const Nv2aTarget *t);
    /* A FLIP_STALL, after it was executed. */
    void (*flip)(void);
} Nv2aCaptureHooks;

/* NULL: no hooks. Before the title submits work. */
void nv2a_pb_exec_set_capture(const Nv2aCaptureHooks *hooks);

/* For a replay: what a DMA offset means, asked before the executor's own rule.
 * fn returns 0 for an offset it does not know. NULL: the executor's rule. */
void nv2a_pb_exec_set_resolver(int (*fn)(uint32_t offset, uint32_t *va));

/* Installs the hooks if RECOMP_FRAME_CAPTURE is set. */
void nv2a_capture_init(void);

/* The file: these 8 bytes, then records, each a type word and its payload
 * (32-bit little-endian words). */
#define NV2A_CAPTURE_MAGIC "NVCAP01"
enum {
    NV2A_CAPTURE_WORD = 1,      /* subchannel, method, parameter */
    NV2A_CAPTURE_MEM,           /* guest address, bytes, then the bytes */
    NV2A_CAPTURE_RESOLVE,       /* DMA offset, guest address */
    NV2A_CAPTURE_STATE_END      /* what came before set up the state the
                                 * frame starts from */
};

#endif /* NV2A_CAPTURE_H */
