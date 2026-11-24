/* Windows only: the GDI framebuffer presenter is Windows-only; POSIX hosts present through
 * fb_present_sdl.c.
 */
#if defined(_WIN32)
#include <windows.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define xbox_GetMemoryOffset present_test_memory_offset
#define nv2a_hook_pvideo_snapshot present_test_pvideo_snapshot
#define nv2a_hook_pvideo_consume present_test_pvideo_consume
#include "../../src/video/fb_present.c"
#undef xbox_GetMemoryOffset

#define FRAME_VA 0x1000u
#define FRAME_PIXELS (640u * 480u)
#define FRAME_BYTES (FRAME_PIXELS * 4u)
#define THREAD_TIMEOUT_MS 5000

typedef void (*recomp_func_t)(void);

static uint8_t *guest;
static ptrdiff_t guest_at;              /* the guest address it is for */
static uint32_t pvideo[0x1000 / 4];
static int pvideo_there;
static uint32_t *output;
static HANDLE started, finished;
static int copy_result;
static volatile LONG stress_done;

ptrdiff_t present_test_memory_offset(void)
{
    return (ptrdiff_t)guest - guest_at;
}

bool present_test_pvideo_snapshot(uint32_t regs[0x1000 / 4])
{
    if (pvideo_there)
        memcpy(regs, pvideo, sizeof pvideo);
    return pvideo_there != 0;
}

void present_test_pvideo_consume(unsigned bank)
{
    pvideo[NV_PVIDEO_BUFFER / 4] &= ~(1u << (bank * 4));
}

recomp_func_t recomp_lookup(uint32_t xbox_va)
{
    (void)xbox_va;
    return NULL;
}

recomp_func_t recomp_lookup_manual(uint32_t xbox_va)
{
    (void)xbox_va;
    return NULL;
}

static void fill32(uint32_t value)
{
    uint32_t *frame = (uint32_t *)(guest + FRAME_VA);
    size_t i;
    for (i = 0; i < FRAME_PIXELS; ++i)
        frame[i] = value;
}

static int output_is(uint32_t value)
{
    size_t i;
    for (i = 0; i < FRAME_PIXELS; ++i)
        if (output[i] != value)
            return 0;
    return 1;
}

static int test_formats_and_gates(void)
{
    uint16_t *rgb565 = (uint16_t *)(guest + FRAME_VA);
    uint32_t *wide = (uint32_t *)(guest + FRAME_VA);
    size_t i;

    if (fb_copy_presented_frame(output))
        return 0;
    fill32(0x00112233u);
    xbox_FramebufferWindowPresent(FRAME_VA, 640u * 4u);
    if (!fb_copy_presented_frame(output) || !output_is(0x00112233u))
        return 0;
    if (_putenv_s("RECOMP_FB_VA", "1") != 0)
        return 0;
    fill32(0x00445566u);
    xbox_FramebufferWindowPresent(FRAME_VA, 640u * 4u);
    if (_putenv_s("RECOMP_FB_VA", "") != 0 ||
        !fb_copy_presented_frame(output) || !output_is(0x00112233u))
        return 0;
    InterlockedExchange(&s_fb_running, 0);
    xbox_FramebufferWindowPresent(FRAME_VA, 640u * 4u);
    InterlockedExchange(&s_fb_running, 1);
    if (!fb_copy_presented_frame(output) || !output_is(0x00112233u))
        return 0;

    for (i = 0; i < FRAME_PIXELS; ++i)
        rgb565[i] = 0xF800u;
    xbox_FramebufferWindowPresent(FRAME_VA, 640u * 2u);
    if (!fb_copy_presented_frame(output) || !output_is(0x00FF0000u))
        return 0;

    for (i = 0; i < FRAME_PIXELS * 4u; ++i)
        wide[i] = (i / 1280u) % 2u == 0 && i % 2u == 0
                    ? 0x00010203u : 0x00AABBCCu;
    xbox_FramebufferWindowSetAA(2);
    xbox_FramebufferWindowPresent(FRAME_VA, 640u * 8u);
    xbox_FramebufferWindowSetAA(1);
    return fb_copy_presented_frame(output) && output_is(0x00010203u);
}

/* The video overlay for a renderer that draws it itself. */
static int test_overlay(void)
{
    static const uint8_t white[4] = { 235, 128, 235, 128 };
    uint32_t *rgb = output;
    int ok;

#define PV(r) pvideo[(r) / 4]
#define DRAWN (rgb[5 * 640 + 7] == 0xFFFFFFFFu && rgb[5 * 640 + 8] == 0xFFFFFFFFu \
               && rgb[5 * 640 + 6] == 5 && rgb[5 * 640 + 9] == 5 && rgb[4 * 640 + 7] == 5)
#define CLEAR() do { size_t n; for (n = 0; n < FRAME_PIXELS; n++) rgb[n] = 5; } while (0)
    if (xbox_FramebufferOverlayState() != 0)
        return 0;
    pvideo_there = 1;
    if (xbox_FramebufferOverlayState() != 0)
        return 0;
    guest_at = 0x80000000;
    memcpy(guest + 0x2000, white, 4);
    PV(NV_PVIDEO_BASE) = 0x1000;
    PV(NV_PVIDEO_OFFSET) = 0x1000;
    PV(NV_PVIDEO_LIMIT) = 0xFFFFFFFFu;
    PV(NV_PVIDEO_BUFFER) = NV_PVIDEO_BUFFER_0_USE;
    PV(NV_PVIDEO_FORMAT) = (NV_PVIDEO_FORMAT_COLOR_LE_CR8YB8CB8YA8 << 16) | 4;
    PV(NV_PVIDEO_SIZE_IN) = (1u << 16) | 2u;
    PV(NV_PVIDEO_POINT_OUT) = (5u << 16) | 7u;
    PV(NV_PVIDEO_SIZE_OUT) = (1u << 16) | 2u;
    PV(NV_PVIDEO_DS_DX) = PV(NV_PVIDEO_DT_DY) = NV_PVIDEO_DIN_DOUT_UNITY;
    CLEAR();
    ok = xbox_FramebufferOverlayState() == 2;           /* a new frame */
    xbox_FramebufferOverlay(rgb);
    ok = ok && DRAWN && PV(NV_PVIDEO_BUFFER) == 0 && xbox_FramebufferOverlayState() == 1;
    memset(guest + 0x2000, 0, 4);                       /* its memory is used again */
    CLEAR();
    xbox_FramebufferOverlay(rgb);
    ok = ok && DRAWN && xbox_FramebufferOverlayState() == 1;
    PV(NV_PVIDEO_STOP) = 1;
    CLEAR();
    ok = ok && xbox_FramebufferOverlayState() == 2;     /* it is to go */
    xbox_FramebufferOverlay(rgb);
    ok = ok && rgb[5 * 640 + 7] == 5 && xbox_FramebufferOverlayState() == 0;
    pvideo_there = 0;
    guest_at = 0;
    return ok;
}

static DWORD WINAPI publish_frame(LPVOID unused)
{
    (void)unused;
    SetEvent(started);
    xbox_FramebufferWindowPresent(FRAME_VA, 640u * 4u);
    SetEvent(finished);
    return 0;
}

static DWORD WINAPI copy_frame(LPVOID unused)
{
    (void)unused;
    SetEvent(started);
    copy_result = fb_copy_presented_frame(output);
    SetEvent(finished);
    return 0;
}

static void finish_thread(HANDLE thread)
{
    if (WaitForSingleObject(thread, THREAD_TIMEOUT_MS) != WAIT_OBJECT_0) {
        fprintf(stderr, "Presentation fixture thread did not finish\n");
        ExitProcess(2);
    }
    CloseHandle(thread);
}

static int test_lock_excludes_writer(void)
{
    HANDLE thread;
    int ok;

    fill32(0x00556677u);
    ResetEvent(started);
    ResetEvent(finished);
    AcquireSRWLockShared(&s_present_lock);
    thread = CreateThread(NULL, 0, publish_frame, NULL, 0, NULL);
    ok = thread != NULL &&
         WaitForSingleObject(started, THREAD_TIMEOUT_MS) == WAIT_OBJECT_0 &&
         WaitForSingleObject(finished, 100) == WAIT_TIMEOUT;
    ReleaseSRWLockShared(&s_present_lock);
    if (thread == NULL)
        return 0;
    finish_thread(thread);
    return ok && fb_copy_presented_frame(output) && output_is(0x00556677u);
}

static int test_lock_excludes_reader(void)
{
    HANDLE thread;
    int ok;

    copy_result = 0;
    ResetEvent(started);
    ResetEvent(finished);
    AcquireSRWLockExclusive(&s_present_lock);
    thread = CreateThread(NULL, 0, copy_frame, NULL, 0, NULL);
    ok = thread != NULL &&
         WaitForSingleObject(started, THREAD_TIMEOUT_MS) == WAIT_OBJECT_0 &&
         WaitForSingleObject(finished, 100) == WAIT_TIMEOUT;
    ReleaseSRWLockExclusive(&s_present_lock);
    if (thread == NULL)
        return 0;
    finish_thread(thread);
    return ok && copy_result && output_is(0x00556677u);
}

static DWORD WINAPI stress_publish(LPVOID unused)
{
    unsigned i;
    (void)unused;
    for (i = 0; i < 200; ++i) {
        fill32(i & 1u ? 0x00112233u : 0x00445566u);
        xbox_FramebufferWindowPresent(FRAME_VA, 640u * 4u);
    }
    InterlockedExchange(&stress_done, 1);
    return 0;
}

static int test_concurrent_frames(void)
{
    HANDLE thread;
    int ok = 1;

    fill32(0x00112233u);
    xbox_FramebufferWindowPresent(FRAME_VA, 640u * 4u);
    thread = CreateThread(NULL, 0, stress_publish, NULL, 0, NULL);
    if (thread == NULL)
        return 0;
    do {
        uint32_t value;
        if (!fb_copy_presented_frame(output)) {
            ok = 0;
            break;
        }
        value = output[0];
        if ((value != 0x00112233u && value != 0x00445566u) || !output_is(value)) {
            ok = 0;
            break;
        }
    } while (!InterlockedCompareExchange(&stress_done, 0, 0));
    finish_thread(thread);
    return ok;
}

int main(void)
{
    int ok;

    guest = (uint8_t *)calloc(1, FRAME_VA + FRAME_BYTES * 4u);
    output = (uint32_t *)malloc(FRAME_BYTES);
    started = CreateEventA(NULL, TRUE, FALSE, NULL);
    finished = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!guest || !output || !started || !finished ||
        _putenv_s("RECOMP_FB_VA", "") != 0) {
        fprintf(stderr, "Cannot initialize the presentation fixture\n");
        return 2;
    }
    InterlockedExchange(&s_fb_running, 1);
    ok = test_formats_and_gates();
    if (!ok)
        fprintf(stderr, "Presentation format or gate regression\n");
    if (ok && !(ok = test_overlay()))
        fprintf(stderr, "The video overlay was not drawn, kept or ended\n");
    if (ok && !(ok = test_lock_excludes_writer()))
        fprintf(stderr, "Presentation writer did not respect the snapshot lock\n");
    if (ok && !(ok = test_lock_excludes_reader()))
        fprintf(stderr, "Presentation reader did not respect the snapshot lock\n");
    if (ok && !(ok = test_concurrent_frames()))
        fprintf(stderr, "Concurrent presentation produced a torn frame\n");

    InterlockedExchange(&s_fb_running, 0);
    CloseHandle(started);
    CloseHandle(finished);
    free(s_present[0]);
    free(s_present[1]);
    free(output);
    free(guest);
    return ok ? 0 : 1;
}

#else
#include <stdio.h>
int main(void) { printf("fb_present: Windows-only, skipped\n"); return 77; }
#endif
