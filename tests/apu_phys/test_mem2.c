/* The APU reads the page a physical address names.
 *
 * The contiguous window hands out physical numbers below the RAM size. The
 * heap used to sit in RAM below 64 MB too, where MmGetPhysicalAddress is the
 * identity, so one number could name a window page and a heap page, and the
 * resolver had to be told which a title meant. The heap is in the arena
 * above RAM now: its physical numbers are its own addresses, none of them a
 * window number.
 *
 * Checked: a heap block is above RAM and its physical address is itself; the
 * resolver reads the block's own bytes for it, with a window block live at
 * the low numbers and after the heap block is freed; a window page reads the
 * window; an image page reads RAM. */
#include "kernel.h"
#include "xbox_memory_layout.h"
#include "apu_shim.h"

#include <stdio.h>
#include <stdlib.h>

/* Provided by the generated title; this harness has none. */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va);
recomp_func_t recomp_lookup(uint32_t xbox_va) { (void)xbox_va; return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
recomp_func_t recomp_lookup_manual(uint32_t xbox_va) { (void)xbox_va; return NULL; }

static int failures;

static void check(int ok, const char *what)
{
    printf("  %-62s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static unsigned char *load_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    unsigned char *buf;
    long n;

    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    rewind(f);
    buf = malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(buf);
        return NULL;
    }
    fclose(f);
    *len = (size_t)n;
    return buf;
}

/* Where the resolver sends a physical address: 0 for the guest address of
 * the same value (RAM or the arena), 1 for the window. */
static int side(uint32_t phys)
{
    uintptr_t off = (uintptr_t)(mcpx_apu_phys(phys) - g_apu_ram_ptr);
    if (off == phys) return 0;
    if (off == 0x80000000u + (uintptr_t)phys) return 1;
    return -1;
}

static uint8_t *host(uint32_t va)
{
    return (uint8_t *)((uintptr_t)va + xbox_GetMemoryOffset());
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "tools/conformance/test.xbe";
    size_t xbe_len = 0;
    unsigned char *xbe;
    uint32_t h, w, wpa;

    setvbuf(stdout, NULL, _IONBF, 0);
#if defined(_WIN32)
    _putenv_s("RECOMP_MEM_MODEL", "2");
#else
    setenv("RECOMP_MEM_MODEL", "2", 1);
#endif
    xbe = load_file(path, &xbe_len);
    if (!xbe) {
        printf("cannot read %s -- run: python tools/conformance/mkxbe.py\n", path);
        return 2;
    }
    if (!xbox_MemoryLayoutInit(xbe, xbe_len)) {
        puts("xbox_MemoryLayoutInit failed");
        return 2;
    }
    /* mcpx_apu_init's job in the runtime; only the base matters here. */
    g_apu_ram_ptr = (uint8_t *)(uintptr_t)xbox_GetMemoryOffset();

    h = xbox_HeapAlloc(0x3000, 4096);
    w = xbox_ContiguousAlloc(0x3000, 4096);
    if (!h || !w) {
        printf("allocations failed: heap 0x%08X window 0x%08X\n", h, w);
        return 2;
    }
    wpa = w - XBOX_CONTIG_BASE;
    printf("apu_phys_mem2: heap block 0x%08X, window block 0x%08X\n", h, w);

    check(h >= g_xbox_arena_base && h >= XBOX_CONTIG_SIZE,
          "a heap block is in the arena, above every window number");
    check(xbox_MmGetPhysicalAddress((PVOID)(uintptr_t)h) == h,
          "MmGetPhysicalAddress is the identity for the heap");
    check(xbox_MmGetPhysicalAddress((PVOID)(uintptr_t)w) == wpa,
          "MmGetPhysicalAddress of a window address is its offset");

    host(h)[0x10] = 0xA5;
    host(w)[0x10] = 0x5A;
    check(side(h + 0x10) == 0 && *mcpx_apu_phys(h + 0x10) == 0xA5,
          "a heap physical address reads the heap block's bytes");
    check(side(wpa + 0x10) == 1 && *mcpx_apu_phys(wpa + 0x10) == 0x5A,
          "a window physical address reads the window's bytes");
    {
        /* An image address and the window page with the same number. */
        uint32_t ia = ((g_xbox_image_lo + 0xFFFu) & ~0xFFFu) + 0x10;
        uint32_t ipa = (uint32_t)xbox_MmGetPhysicalAddress((PVOID)(uintptr_t)ia);
        uint32_t pin = xbox_ContiguousAllocEx(0x1000, 4096, ia & ~0xFFFu,
                                              ia | 0xFFFu);
        uint8_t b = host(ia)[0];

        check(g_xbox_image_phys && ipa == g_xbox_image_phys + ia
                  && ipa >= XBOX_CONTIG_SIZE && ipa < g_xbox_arena_top,
              "an image address has a number above every window number");
        check(h + 0x3000 <= g_xbox_image_phys,
              "the heap ends below the image's numbers");
        check(pin == XBOX_CONTIG_BASE + (ia & ~0xFFFu),
              "a window block can be pinned at the image's address");
        if (pin) {
            host(pin)[0x10] = (uint8_t)~b;
            check(side(ia) == 1 && *mcpx_apu_phys(ia) == (uint8_t)~b,
                  "that number below 64 MB reads the window");
        }
        check(mcpx_apu_phys(ipa) != host(ia) && *mcpx_apu_phys(ipa) == b,
              "the image's own number reads the image");
        check(host(XBOX_CONTIG_BASE + ipa)[0] == b,
              "and so does 0x80000000 + that number");
        if (pin)
            xbox_ContiguousFree(pin);
    }
    check(side(XBOX_STACK_BASE + 0x100) == 0,
          "a stack address, in the arena, reads itself");

    xbox_HeapFree(h);
    check(side(h + 0x10) == 0,
          "a freed heap block's number still is not a window number");
    xbox_ContiguousFree(w);

    printf("apu_phys_mem2: %s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
