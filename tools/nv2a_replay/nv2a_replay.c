/*
 * Draws a captured frame again, offline, with the executor's own code
 * (RECOMP_FRAME_CAPTURE, src/kernel/nv2a_capture.c).
 *
 *   nv2a-replay <file.nvcap> [--mode draw|vulkan|transform|walk] [--repeat N]
 *               [--bmp out.bmp] [--raw out.bin] [--expect in.bin]
 *
 * --mode says how far the executor goes, and so where its time goes:
 *   walk       every command, no vertices (a batch back end takes each batch)
 *   transform  the same, and every vertex is transformed on the CPU
 *   draw       everything: the CPU rasteriser, into the replay's own memory
 *   vulkan     everything: the Vulkan rasteriser (built with XBOXRECOMP_NV2A_VK)
 * The time printed covers the commands, not the copying of saved memory; the
 * mean is over all passes but the first two.
 *
 * --bmp and --raw write the frame shown by the last flip; --expect compares it
 * with a --raw file and fails if a pixel differs.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "nv2a_backend_batch.h"
#include "nv2a_capture.h"
#include "nv2a_perf.h"
#if defined(XBOXRECOMP_NV2A_VK)
#include "nv2a_vk.h"
#endif

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#endif

/* ---- guest memory: 4 GB of addresses, pages appear as they are touched -- */

static uint8_t *s_mem;

#if defined(_WIN32)
static LONG CALLBACK commit_on_touch(EXCEPTION_POINTERS *e)
{
    const EXCEPTION_RECORD *r = e->ExceptionRecord;
    ULONG_PTR at;

    if (r->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || r->NumberParameters < 2)
        return EXCEPTION_CONTINUE_SEARCH;
    at = r->ExceptionInformation[1];
    if (at < (ULONG_PTR)s_mem || at >= (ULONG_PTR)s_mem + 0x100000000ull)
        return EXCEPTION_CONTINUE_SEARCH;
    if (!VirtualAlloc((void *)(at & ~(ULONG_PTR)0xFFFF), 0x10000, MEM_COMMIT,
                      PAGE_READWRITE))
        return EXCEPTION_CONTINUE_SEARCH;
    return EXCEPTION_CONTINUE_EXECUTION;
}
#endif

static int memory_init(void)
{
#if defined(_WIN32)
    s_mem = (uint8_t *)VirtualAlloc(NULL, (SIZE_T)0x100000000ull, MEM_RESERVE,
                                    PAGE_READWRITE);
    AddVectoredExceptionHandler(1, commit_on_touch);
#else
    s_mem = (uint8_t *)mmap(NULL, 0x100000000ull, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (s_mem == (uint8_t *)MAP_FAILED)
        s_mem = NULL;
#endif
    return s_mem != NULL;
}

/* ---- what the executor needs from the runtime --------------------------- */

static struct { uint32_t offset, va; } *s_res;
static uint32_t s_res_count;

static uint32_t s_shown_va, s_shown_pitch, s_flips;

ptrdiff_t xbox_GetMemoryOffset(void) { return (ptrdiff_t)s_mem; }
uint32_t g_xbox_image_lo, g_xbox_image_hi;
uint32_t g_xbox_stack_base, g_xbox_stack_size;
uint32_t xbox_ContiguousAllocatedBytes(void) { return 0; }
/* Every DMA offset means what it meant when the frame was captured. */
static int captured_va(uint32_t pa, uint32_t *va)
{
    uint32_t lo = 0, hi = s_res_count;

    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (s_res[mid].offset == pa) {
            *va = s_res[mid].va;
            return 1;
        }
        if (s_res[mid].offset < pa)
            lo = mid + 1;
        else
            hi = mid;
    }
    return 0;
}
void xbox_FramebufferWindowSet(uint32_t va, uint32_t pitch) { (void)va; (void)pitch; }
void xbox_FramebufferWindowSetAA(uint32_t sy) { (void)sy; }
void xbox_FramebufferWindowPresent(uint32_t va, uint32_t pitch)
{
    s_shown_va = va;
    s_shown_pitch = pitch;
    s_flips++;
}
uint32_t xbox_Nv2aFenceWordVa(void) { return 0; }
int nv2a_pb_dma_walk(void) { return 1; }
void xbox_FramebufferWindowStart(void) {}
void xbox_FramebufferWindowFrameStats(uint32_t draws) { (void)draws; }
void xbox_Nv2aFrameCounterFlip(void) {}
int nv2a_pb_scan_in_call(void) { return 0; }
uint32_t xbox_VblankCount(void) { return 0; }
uint32_t xbox_Nv2aFrameCountersOwned(void) { return 0; }
void xbox_RegisterTitleEvent(uint32_t guest_va) { (void)guest_va; }
void xbox_SetTitleEvent(uint32_t guest_va) { (void)guest_va; }
int xbox_Nv2aPgraphRaise(uint32_t addr, uint32_t data) { (void)addr; (void)data; return 0; }
int xbox_Nv2aPgraphWaitAck(uint32_t timeout_ms) { (void)timeout_ms; return 1; }
volatile uint32_t *xbox_Nv2aRegPtr(uint32_t offset) { (void)offset; return NULL; }

extern void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param);
extern int nv2a_pb_exec_run(uint32_t method, const uint32_t *p, uint32_t n);
extern int nv2a_pb_exec_constant_word(uint32_t method, uint32_t param);

/* ---- how far to go ------------------------------------------------------ */

static int s_transform;
static uint32_t s_batches, s_vertices, s_max;
static Nv2aVshOutput *s_out;

static int take_batch(const Nv2aRawBatch *b)
{
    s_batches++;
    s_vertices += b->index_count;
    if (s_transform) {
        if (b->index_count > s_max) {
            s_max = b->index_count;
            s_out = (Nv2aVshOutput *)realloc(s_out, s_max * sizeof *s_out);
            if (!s_out)
                exit(2);
        }
        nv2a_batch_transform(0, b->index_count, s_out);
    }
    return 1;
}

static int take_clear(const Nv2aTarget *t, const Nv2aRenderState *rs,
                      const Nv2aRawClear *c)
{
    (void)t; (void)rs; (void)c;
    return 1;
}

static const Nv2aBatchBackend s_taker = { NV2A_BATCH_VERSION, take_batch, take_clear };

/* ---- the file ----------------------------------------------------------- */

static uint8_t *s_file;
static size_t s_size;

static uint32_t u32(size_t at)
{
    uint32_t v;
    memcpy(&v, s_file + at, 4);
    return v;
}

/* The record at `at`: its type and the offset of the next; 0 at the end. */
static uint32_t record(size_t at, size_t *next)
{
    uint32_t type;

    if (at + 4 > s_size)
        return 0;
    type = u32(at);
    switch (type) {
    case NV2A_CAPTURE_WORD:      *next = at + 16; break;
    case NV2A_CAPTURE_RESOLVE:   *next = at + 12; break;
    case NV2A_CAPTURE_STATE_END: *next = at + 4; break;
    case NV2A_CAPTURE_MEM:
        if (at + 12 > s_size)
            return 0;
        *next = at + 12 + (size_t)u32(at + 8);
        break;
    default:
        fprintf(stderr, "bad record %u at %zu\n", type, at);
        return 0;
    }
    return *next <= s_size ? type : 0;
}

static int by_offset(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

static int load(const char *path)
{
    FILE *f = fopen(path, "rb");
    size_t at, next;
    uint32_t type;
    long size;

    if (!f)
        return 0;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    s_file = (uint8_t *)malloc(size > 0 ? (size_t)size : 1);
    s_size = s_file && size > 0 ? fread(s_file, 1, (size_t)size, f) : 0;
    fclose(f);
    if (s_size < 8 || memcmp(s_file, NV2A_CAPTURE_MAGIC, 8) != 0)
        return 0;
    for (at = 8; (type = record(at, &next)) != 0; at = next)
        s_res_count += type == NV2A_CAPTURE_RESOLVE;
    s_res = malloc((s_res_count + 1) * sizeof *s_res);
    if (!s_res)
        return 0;
    s_res_count = 0;
    for (at = 8; (type = record(at, &next)) != 0; at = next)
        if (type == NV2A_CAPTURE_RESOLVE) {
            s_res[s_res_count].offset = u32(at + 4);
            s_res[s_res_count++].va = u32(at + 8);
        }
    qsort(s_res, s_res_count, sizeof *s_res, by_offset);
    nv2a_pb_exec_set_resolver(captured_va);
    return 1;
}

void xbox_HostSleepNs(uint64_t ns) { (void)ns; }
uint64_t xbox_HostNowNs(void)
{
#if defined(_WIN32)
    LARGE_INTEGER c, f;
    QueryPerformanceCounter(&c);
    QueryPerformanceFrequency(&f);
    return (uint64_t)((double)c.QuadPart * 1e9 / (double)f.QuadPart);
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
#endif
}

static double now_ms(void) { return (double)xbox_HostNowNs() / 1e6; }

/* The words from here on that are for this method, as the walker finds them. */
static uint32_t run(size_t at, uint32_t method, uint32_t *p, uint32_t most, size_t *next)
{
    uint32_t n = 0;

    while (n < most && at + 16 <= s_size && u32(at) == NV2A_CAPTURE_WORD && u32(at + 4) == 0
           && u32(at + 8) == method) {
        p[n++] = u32(at + 12);
        at += 16;
    }
    *next = at;
    return n;
}

/* One pass over the file. Returns the milliseconds its commands took, the
 * ones that only set the state up excluded. */
static double play(uint32_t *words)
{
    size_t at, next;
    uint32_t type;
    double spent = 0.0, from = now_ms();

    *words = 0;
    for (at = 8; (type = record(at, &next)) != 0; at = next) {
        if (type == NV2A_CAPTURE_WORD) {
            uint32_t subch = u32(at + 4), method = u32(at + 8), param = u32(at + 12);
            uint32_t p[128], n;
            size_t after;

            if (subch == 0 && (method == 0x1800 || method == 0x1808 || method == 0x1818)
                && (n = run(at, method, p, 128, &after)) > 1 && nv2a_pb_exec_run(method, p, n)) {
                next = after;
                *words += n;
                continue;
            }
            if (!(subch == 0 && method >= 0x0B80u && method < 0x0C00u
                  && nv2a_pb_exec_constant_word(method, param)))
                nv2a_pb_exec_method(subch, method, param);
            ++*words;
        } else if (type == NV2A_CAPTURE_MEM) {
            spent += now_ms() - from;
            memcpy(s_mem + u32(at + 4), s_file + at + 12, u32(at + 8));
            from = now_ms();
        } else if (type == NV2A_CAPTURE_STATE_END) {
            spent = 0.0;
            *words = 0;
            s_batches = s_vertices = 0;
            from = now_ms();
        }
    }
    return spent + (now_ms() - from);
}

/* The surface last shown, as 0xAARRGGBB. The height is not part of a present:
 * the last clip sent says it, kept from the commands. */
static uint32_t shown_height(void)
{
    size_t at, next;
    uint32_t type, h = 0;

    for (at = 8; (type = record(at, &next)) != 0; at = next)
        if (type == NV2A_CAPTURE_WORD && u32(at + 4) == 0 && u32(at + 8) == 0x0204) {
            uint32_t n = (u32(at + 12) & 0xFFFF) + (u32(at + 12) >> 16);
            if (n > h)
                h = n;
        }
    return h;
}

static int write_bmp(const char *path, uint32_t w, uint32_t h)
{
    uint8_t head[54] = { 'B', 'M' };
    uint32_t row = (w * 3 + 3) & ~3u, size = 54 + row * h, v, x, y;
    uint8_t *line = (uint8_t *)calloc(1, row);
    FILE *f = fopen(path, "wb");

    if (!f || !line)
        return 0;
    memcpy(head + 2, &size, 4);
    v = 54;  memcpy(head + 10, &v, 4);
    v = 40;  memcpy(head + 14, &v, 4);
    memcpy(head + 18, &w, 4);
    memcpy(head + 22, &h, 4);
    head[26] = 1;
    head[28] = 24;
    fwrite(head, 1, sizeof head, f);
    for (y = h; y-- > 0; ) {
        for (x = 0; x < w; x++)
            memcpy(line + 3 * x, s_mem + s_shown_va + (size_t)y * s_shown_pitch + 4 * x, 3);
        fwrite(line, 1, row, f);
    }
    free(line);
    return fclose(f) == 0;
}

int main(int argc, char **argv)
{
    const char *path = NULL, *mode = "draw", *bmp = NULL, *raw = NULL, *expect = NULL;
    int repeat = 1, i, status = 0;
    uint32_t words = 0, h;
    double best = 0.0, sum = 0.0;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--mode") && i + 1 < argc) mode = argv[++i];
        else if (!strcmp(argv[i], "--repeat") && i + 1 < argc) repeat = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--bmp") && i + 1 < argc) bmp = argv[++i];
        else if (!strcmp(argv[i], "--raw") && i + 1 < argc) raw = argv[++i];
        else if (!strcmp(argv[i], "--expect") && i + 1 < argc) expect = argv[++i];
        else if (!path) path = argv[i];
        else path = "";
    }
    if (!path || !*path || (strcmp(mode, "draw") && strcmp(mode, "transform")
                            && strcmp(mode, "vulkan") && strcmp(mode, "walk"))) {
        fprintf(stderr, "usage: nv2a-replay <file.nvcap> [--mode draw|vulkan|transform|walk]"
                        " [--repeat N]\n                   [--bmp out.bmp] [--raw out.bin]"
                        " [--expect in.bin]\n");
        return 2;
    }
    if (!memory_init() || !load(path)) {
        fprintf(stderr, "cannot read %s\n", path);
        return 2;
    }
    if (!strcmp(mode, "vulkan")) {
#if defined(XBOXRECOMP_NV2A_VK)
        if (!nv2a_vk_init())
            return 77;                  /* no Vulkan here: a skipped test */
#else
        fprintf(stderr, "built without XBOXRECOMP_NV2A_VK\n");
        return 77;
#endif
    } else if (strcmp(mode, "draw")) {
        s_transform = !strcmp(mode, "transform");
        nv2a_backend_register_batches(&s_taker);
    }
    nv2a_perf_phase(NV2A_PERF_WORK);    /* RECOMP_PERF_LOG: there is no title */
    for (i = 0; i < repeat; i++) {
        double ms = play(&words);
        printf("%s pass %d: %.2f ms\n", mode, i + 1, ms);
        if (i == 0 || ms < best)
            best = ms;
        if (i > 1)
            sum += ms;
    }
#if defined(XBOXRECOMP_NV2A_VK)
    if (!strcmp(mode, "vulkan")) {
        double from = now_ms();
        Nv2aVkStats st;

        nv2a_vk_wait();
        sum += now_ms() - from;
        nv2a_vk_stats(&st);
        printf("vulkan: a pass: %.1f waits, %.1f surfaces back\n",
               (double)st.flushes / repeat, (double)st.readbacks / repeat);
    }
#endif
    printf("%s: best %.2f ms, ", mode, best);
    if (repeat > 2)
        printf("mean %.2f ms, ", sum / (repeat - 2));
    printf("%u words, %u flips", words, s_flips);
    if (strcmp(mode, "draw") && strcmp(mode, "vulkan"))
        printf(", %u batches, %u vertices", s_batches, s_vertices);
    printf("\n");
    h = shown_height();
    if (bmp || raw || expect)
        printf("shown 0x%08X, %u bytes a row, %u rows\n", s_shown_va, s_shown_pitch, h);
    if ((bmp || raw || expect) && (!s_shown_va || !h)) {
        fprintf(stderr, "nothing was shown\n");
        return 1;
    }
    if (bmp && !write_bmp(bmp, s_shown_pitch / 4, h))
        status = 1;
    if (raw) {
        FILE *f = fopen(raw, "wb");
        if (!f || fwrite(s_mem + s_shown_va, s_shown_pitch, h, f) != h)
            status = 1;
        if (f)
            fclose(f);
    }
    if (expect) {
        FILE *f = fopen(expect, "rb");
        size_t n = (size_t)s_shown_pitch * h, k, bad = 0;
        uint8_t *want = (uint8_t *)malloc(n);

        if (!f || !want || fread(want, 1, n, f) != n) {
            fprintf(stderr, "cannot read %s\n", expect);
            return 2;
        }
        fclose(f);
        for (k = 0; k + 4 <= n; k += 4)
            bad += memcmp(want + k, s_mem + s_shown_va + k, 4) != 0;
        printf("%zu pixels differ\n", bad);
        status |= bad != 0;
    }
    return status;
}
