/*
 * Memory model 2: RECOMP_MEM_MODEL=2.
 *
 * The classic model keeps the heap and the stacks in RAM above the image,
 * where an address doubles as a physical number, while the contiguous window
 * (separate storage at 0x80000000) hands out the same numbers for different
 * pages. A device given a physical address then has to guess which page was
 * meant, and each wrong guess was a bug: noise for sound, a pad that froze.
 *
 * Here each kind of guest memory has physical numbers no other kind has:
 *
 *   contiguous window, 0x80000000 + P     P, below 64 MB
 *   heap, stacks (the arena)              the address itself
 *   RAM from 0 to the end of the image    g_xbox_image_phys + address
 *
 * The arena is guest address space from the end of RAM to 256 MB with host
 * storage of its own: what is only virtual memory on the console. It stops
 * at 256 MB so every address in it survives the 28-bit mask the XDK's
 * drivers put on physical addresses. Its top is the image a second time: a
 * view of RAM from 0 to the end of the image, also mapped at 0x80000000 +
 * the number, where a title following the window rule looks. So a number at
 * or above 64 MB is the guest address of the same value.
 *
 * With the model on, freed memory is always given back (as under
 * RECOMP_HEAP_RECLAIM) and the RAM mirrors are off unless RECOMP_MIRRORS=1.
 * The hooks in the classic files are marked "mem2".
 *
 * docs/technical/memory-experiment.md has the background and the findings.
 */

#include "xbox_mem2.h"
#include "xbox_memory_layout.h"
#include "kernel.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint32_t g_xbox_arena_base = 0;
uint32_t g_xbox_heap_top   = 0;
uint32_t g_xbox_arena_top  = 0;
uint32_t g_xbox_image_phys = 0;

static void *g_arena_memory;
static void *g_image_phys_view;      /* the image at its physical numbers */
static void *g_image_window_view;    /* and at 0x80000000 + those */

/* 1 = the window handed this 4 KB page out at some time. */
#define MEM2_PAGE   4096u
#define MEM2_PAGES  (XBOX_CONTIG_SIZE / MEM2_PAGE)
static uint8_t g_contig_ever[MEM2_PAGES];

int xbox_mem2_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_MEM_MODEL");
        on = e && e[0] == '2';
    }
    return on;
}

int xbox_mem2_mirrors_on(void)
{
    const char *e = getenv("RECOMP_MIRRORS");
    return e && e[0] == '1';
}

int xbox_mem2_place(void *ram, size_t ram_size, void *mapping,
                    xbox_mem2_map_fn map_view, xbox_mem2_commit_fn commit_at)
{
    const uint32_t MB = 1024u * 1024u;
    /* To 256 MB, or as far as the title asked for with xbox_SetMapSize
     * (rounded to 64 KB, and kept clear of the contiguous window). */
    uint64_t arena_top = 0x10000000u;
    size_t bytes;

    if (g_xbox_map_size > arena_top)
        arena_top = ((uint64_t)g_xbox_map_size + 0xFFFFu) & ~(uint64_t)0xFFFFu;
    if (arena_top > 0x70000000u)
        arena_top = 0x70000000u;
    if (arena_top < (uint64_t)ram_size + 32 * MB)
        arena_top = (uint64_t)ram_size + 192 * MB;

    /* The image's physical numbers: a view of RAM from guest 0 to the end
     * of the image, at the top of the arena's range. */
    if (g_xbox_image_hi && mapping) {
        uint64_t span = ((uint64_t)g_xbox_image_hi + MB - 1) & ~(uint64_t)(MB - 1);

        if (span <= ram_size && arena_top - span >= (uint64_t)ram_size + 32 * MB)
            g_image_phys_view = map_view(mapping, (size_t)span,
                (void *)((uintptr_t)ram + (uintptr_t)(arena_top - span)));
        if (g_image_phys_view) {
            g_xbox_image_phys = (uint32_t)(arena_top - span);
            /* Where a title reaches the same page through the window rule,
             * 0x80000000 + physical. Best effort. */
            g_image_window_view = map_view(mapping, (size_t)span,
                (void *)((uintptr_t)ram + XBOX_CONTIG_BASE + g_xbox_image_phys));
            fprintf(stderr, "  Image physical numbers: 0x%08X..0x%08X%s\n",
                    g_xbox_image_phys, (uint32_t)arena_top,
                    g_image_window_view ? ", also through the window"
                                        : " (no window view)");
        } else {
            fprintf(stderr, "  WARNING: no view for the image's physical "
                    "numbers (error %lu); an image address stays its own "
                    "physical number\n", (unsigned long)GetLastError());
        }
    }
    g_xbox_arena_top = (uint32_t)arena_top;
    if (g_xbox_image_phys)
        arena_top = g_xbox_image_phys;          /* the heap ends below it */
    bytes = (size_t)(arena_top - ram_size);

    g_arena_memory = commit_at((void *)((uintptr_t)ram + ram_size), bytes);
    if (!g_arena_memory) {
        fprintf(stderr,
                "  ERROR: no host memory for the %u MB arena above RAM at %p "
                "(error %lu); the stack and the heap live there\n",
                (unsigned)(bytes / MB), (void *)((uintptr_t)ram + ram_size),
                (unsigned long)GetLastError());
        return 0;
    }
    g_xbox_arena_base = (uint32_t)ram_size;
    g_xbox_heap_top   = (uint32_t)arena_top;
    fprintf(stderr, "  Arena (memory model 2): %u MB above RAM, Xbox VA "
            "0x%08X..0x%08X\n", (unsigned)(bytes / MB), g_xbox_arena_base,
            g_xbox_heap_top);

    /* The image does not compete with these for room: the arena is at least
     * 32 MB, so the stack keeps its 8 MB. */
    g_xbox_region_base = g_xbox_arena_base;
    g_xbox_stack_base  = g_xbox_arena_base + XBOX_REGION_STACK_OFF;
    g_xbox_stack_size  = 8 * MB;
    fprintf(stderr, "  Toolkit region: 0x%08X, in the arena (image ends "
            "0x%08X)\n", g_xbox_region_base, g_xbox_image_hi);
    return 1;
}

void xbox_mem2_shutdown(void)
{
    if (g_arena_memory)
        VirtualFree(g_arena_memory, 0, MEM_RELEASE);
    if (g_image_phys_view)
        UnmapViewOfFile(g_image_phys_view);
    if (g_image_window_view)
        UnmapViewOfFile(g_image_window_view);
    g_arena_memory = g_image_phys_view = g_image_window_view = NULL;
    g_xbox_arena_base = g_xbox_heap_top = 0;
    g_xbox_arena_top = g_xbox_image_phys = 0;
}

int xbox_mem2_arena_has(uint32_t va, uint32_t bytes)
{
    return g_xbox_arena_base && va >= g_xbox_arena_base
        && (uint64_t)va + bytes <= g_xbox_arena_top;
}

int xbox_mem2_guest_to_phys(uint32_t va, uint32_t *pa)
{
    if (!g_xbox_arena_base)
        return 0;
    if (va >= XBOX_CONTIG_BASE
            && (uint64_t)va < (uint64_t)XBOX_CONTIG_BASE + XBOX_CONTIG_SIZE)
        *pa = va - XBOX_CONTIG_BASE;
    /* RAM up to the end of the image; page zero holds nothing, so a null
     * pointer stays 0. */
    else if (g_xbox_image_phys && va >= 0x1000
            && va < g_xbox_arena_top - g_xbox_image_phys)
        *pa = g_xbox_image_phys + va;
    else
        *pa = va;
    return 1;
}

int xbox_mem2_phys_to_guest(uint32_t pa, uint32_t *va)
{
    if (!g_xbox_arena_base)
        return 0;
    if (pa >= XBOX_CONTIG_SIZE) {
        /* The arena and the image's numbers are addresses already. */
        *va = pa;
    } else if (pa >= g_xbox_image_lo && pa < g_xbox_image_hi
            && !g_contig_ever[pa / MEM2_PAGE]) {
        /* A number in the image's own address range that the window has
         * never handed out did not come from MmGetPhysicalAddress: the title
         * took an image address as its own physical number. Read it that
         * way, and say so. */
        static int said;
        if (!said++)
            fprintf(stderr, "[PHYS] 0x%08X is an image address used as a "
                    "physical number; resolved to the image (said once)\n", pa);
        *va = pa;
    } else {
        *va = XBOX_CONTIG_BASE + pa;
    }
    return 1;
}

void xbox_mem2_contig_taken(uint32_t first, uint32_t pages)
{
    if (first < MEM2_PAGES && pages <= MEM2_PAGES - first)
        memset(&g_contig_ever[first], 1, pages);
}

int xbox_mem2_query(uint32_t page_base, uint32_t *bytes)
{
    if (!g_xbox_arena_base || page_base < g_xbox_arena_base
            || page_base >= g_xbox_heap_top)
        return 0;
    /* The toolkit region and the main stack, at the foot of the arena. */
    if (page_base < XBOX_HEAP_BASE) {
        *bytes = XBOX_HEAP_BASE - page_base;
        return 1;
    }
    /* Inside a live heap block: committed to the end of the block. The arena
     * is not one committed run the way RAM is reported, so a title that
     * walks memory does not find 192 MB more of it. */
    *bytes = (xbox_HeapBlockSize(page_base) + 0xFFFu) & ~0xFFFu;
    return *bytes != 0;
}
