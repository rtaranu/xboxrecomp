/* BACK_END_WRITE_SEMAPHORE_RELEASE (0x1D70) writes into the memory object the
 * title bound with SET_CONTEXT_DMA_SEMAPHORE (0x1A4), at SET_SEMAPHORE_OFFSET
 * (0x1D6C). The executor finds the object the way the GPU does: the handle in
 * the object table PFIFO 0x2210 places in instance memory, then the object's
 * flags, limit and page.
 *
 * Guest memory is a buffer at the start of the contiguous window
 * (0x80000000); instance memory is an array behind xbox_Nv2aRegPtr. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define MEM_BYTES 0x10000u
static uint8_t s_mem[MEM_BYTES];

ptrdiff_t xbox_GetMemoryOffset(void) { return (ptrdiff_t)s_mem - (ptrdiff_t)0x80000000u; }
uint32_t g_xbox_image_lo, g_xbox_image_hi;
uint32_t g_xbox_stack_base, g_xbox_stack_size;
uint32_t xbox_ContiguousAllocatedBytes(void) { return 0; }
void xbox_FramebufferWindowSet(uint32_t va, uint32_t pitch) { (void)va; (void)pitch; }
void xbox_FramebufferWindowSetAA(uint32_t sy) { (void)sy; }
/* A project's fence mirror and the DMA walk, for the 0x0310 latch. */
static uint32_t s_fence_va;
static int s_dma_walk;
uint32_t xbox_Nv2aFenceWordVa(void) { return s_fence_va; }
int nv2a_pb_dma_walk(void) { return s_dma_walk; }
void xbox_FramebufferWindowStart(void) {}
void xbox_FramebufferWindowFrameStats(uint32_t draws) { (void)draws; }
void xbox_FramebufferWindowPresent(uint32_t va, uint32_t pitch) { (void)va; (void)pitch; }
void xbox_Nv2aFrameCounterFlip(void) {}
int nv2a_pb_scan_in_call(void) { return 0; }
uint32_t xbox_VblankCount(void) { return 0; }
uint64_t xbox_HostNowNs(void) { return 0; }
uint32_t xbox_Nv2aFrameCountersOwned(void) { return 0; }
/* Memory model 2, title events and the PGRAPH trap: off here. */
int xbox_mem2_phys_to_guest(uint32_t pa, uint32_t *va) { (void)pa; (void)va; return 0; }
int xbox_mem2_arena_has(uint32_t va, uint32_t bytes) { (void)va; (void)bytes; return 0; }
void xbox_RegisterTitleEvent(uint32_t guest_va) { (void)guest_va; }
void xbox_SetTitleEvent(uint32_t guest_va) { (void)guest_va; }
int xbox_Nv2aPgraphRaise(uint32_t addr, uint32_t data) { (void)addr; (void)data; return 0; }
int xbox_Nv2aPgraphWaitAck(uint32_t timeout_ms) { (void)timeout_ms; return 1; }

/* PFIFO 0x2210 = 0: a 4 KB object table at the start of instance memory
 * (0x700000). One entry pairs SEM_HANDLE with the instance at 0x701000. */
#define RAMIN      0x700000u
#define SEM_HANDLE 0x00000008u
#define SEM_INST   0x701000u
#define PAT_HANDLE 0x00000044u
#define PAT_INST   0x701100u
static volatile uint32_t s_ramht_cfg, s_latch;
static volatile uint32_t s_ramin[0x2000 / 4];

volatile uint32_t *xbox_Nv2aRegPtr(uint32_t offset)
{
    if (offset == 0x002210u)
        return &s_ramht_cfg;
    if (offset == 0x400B10u)
        return &s_latch;
    if (offset >= RAMIN && offset < RAMIN + sizeof s_ramin)
        return &s_ramin[(offset - RAMIN) / 4];
    return NULL;
}

extern void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param);
extern void nv2a_pb_set_semaphore_target(uint32_t guest_va);
static void m(uint32_t method, uint32_t param) { nv2a_pb_exec_method(0, method, param); }
static uint32_t get32(uint32_t phys) { uint32_t v; memcpy(&v, s_mem + phys, 4); return v; }

static int s_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); s_fail = 1; } } while (0)

int main(void)
{
    /* The object: present and linear (0x3000), starting 0x30 into physical
     * page 0x4000, limit 0x1F (32 bytes). */
    s_ramin[0] = SEM_HANDLE;
    s_ramin[1] = 0x80000000u | (SEM_INST >> 4 & 0xFFFFu);
    s_ramin[(SEM_INST - RAMIN) / 4 + 0] = 0x3000u | (0x30u << 20);
    s_ramin[(SEM_INST - RAMIN) / 4 + 1] = 0x1Fu;
    s_ramin[(SEM_INST - RAMIN) / 4 + 2] = 0x4000u | 3u;
    /* A pattern object (class 0x44), which the 0x0310 latch is keyed on. */
    s_ramin[2] = PAT_HANDLE;
    s_ramin[3] = 0x80000000u | (PAT_INST >> 4 & 0xFFFFu);
    s_ramin[(PAT_INST - RAMIN) / 4] = 0x44u;

    /* Before any object is bound, a word the project named is served. */
    nv2a_pb_set_semaphore_target(0x80000100u);
    m(0x1D70, 0x11);
    CHECK(get32(0x100) == 0x11);

    /* Bound: the release goes into the object at the offset. */
    m(0x01A4, SEM_HANDLE);
    m(0x1D6C, 0x10);
    m(0x1D70, 0x22);
    CHECK(get32(0x4040) == 0x22);
    CHECK(get32(0x100) == 0x11);

    /* The last word in the object is still inside it. */
    m(0x1D6C, 0x1C);
    m(0x1D70, 0x33);
    CHECK(get32(0x404C) == 0x33);

    /* An offset past the limit is refused, not written past the object; the
     * named word is the fallback. */
    m(0x1D6C, 0x20);
    m(0x1D70, 0x44);
    CHECK(get32(0x4050) == 0);
    CHECK(get32(0x100) == 0x44);

    /* The 0x0310 latch (the pattern object on subchannel 5), with a fence
     * mirror and no named word: it carries the mirror's fence bits until the
     * object has taken a release, so refused releases do not leave it
     * waiting on the object. Binding a handle that is not there, then the
     * object, rebinds it. */
    nv2a_pb_exec_method(5, 0, PAT_HANDLE);
    nv2a_pb_set_semaphore_target(0);
    s_dma_walk = 1;
    s_fence_va = 0x80000200u;
    memcpy(s_mem + 0x200, &(uint32_t){ 5 }, 4);
    m(0x01A4, 0x99);
    m(0x01A4, SEM_HANDLE);
    m(0x1D6C, 0x20);
    m(0x1D70, 0x55);                                 /* refused */
    nv2a_pb_exec_method(5, 0x0310, 0x80);
    CHECK(s_latch == (0x80u | 5u << 2));
    m(0x1D6C, 0x10);
    m(0x1D70, 0x66);                                 /* taken */
    nv2a_pb_exec_method(5, 0x0310, 0x80);
    CHECK(s_latch == 0x80u);

    if (!s_fail)
        printf("nv2a semaphore object: ok\n");
    return s_fail;
}
