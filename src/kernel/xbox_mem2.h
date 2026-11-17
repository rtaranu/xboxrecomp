/*
 * Memory model 2 (RECOMP_MEM_MODEL=2): see xbox_mem2.c.
 *
 * Everything here answers "no" or does nothing while the model is off, so a
 * caller in the classic code is one line: ask, and carry on as before when
 * the answer is 0.
 */
#ifndef XBOX_MEM2_H
#define XBOX_MEM2_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* RECOMP_MEM_MODEL=2. Read once. */
int xbox_mem2_on(void);
/* RECOMP_MIRRORS=1: the RAM mirrors are wanted although the model is on. */
int xbox_mem2_mirrors_on(void);

/* The arena: guest addresses from the end of RAM to g_xbox_arena_top, with
 * host storage of their own. The toolkit region, the main stack and the heap
 * are in [g_xbox_arena_base, g_xbox_heap_top); a second view of RAM from 0 to
 * the end of the image is in [g_xbox_image_phys, g_xbox_arena_top). All 0
 * while the model is off. */
extern uint32_t g_xbox_arena_base;
extern uint32_t g_xbox_heap_top;
extern uint32_t g_xbox_arena_top;
extern uint32_t g_xbox_image_phys;

/* Map the arena above RAM and put the toolkit region, the stack and the heap
 * in it (sets g_xbox_region_base, g_xbox_stack_base, g_xbox_stack_size).
 * `ram` is the host address of guest 0, `mapping` the section behind RAM.
 * The two callbacks are the layout's own fixed-address helpers. Returns 0 on
 * failure, having said why. */
typedef void *(*xbox_mem2_map_fn)(void *mapping, size_t size, void *addr);
typedef void *(*xbox_mem2_commit_fn)(void *addr, size_t size);
int xbox_mem2_place(void *ram, size_t ram_size, void *mapping,
                    xbox_mem2_map_fn map_view, xbox_mem2_commit_fn commit_at);
void xbox_mem2_shutdown(void);

/* 1 when [va, va + bytes) is inside the arena. */
int xbox_mem2_arena_has(uint32_t va, uint32_t bytes);

/* Physical numbers. 1 and the answer when the model is on, 0 when the caller
 * should use its own rule. */
int xbox_mem2_guest_to_phys(uint32_t va, uint32_t *pa);
int xbox_mem2_phys_to_guest(uint32_t pa, uint32_t *va);

/* 1 when the contiguous window is filled from the top, as the console's
 * kernel fills it: with the model on, unless RECOMP_CONTIG_TOP_DOWN=0. */
int xbox_mem2_contig_top_down(void);

/* The contiguous allocator handed out `pages` 4 KB pages from `first`. */
void xbox_mem2_contig_taken(uint32_t first, uint32_t pages);

/* NtQueryVirtualMemory for an address in the arena: 1 and the bytes committed
 * from `page_base` on, 0 when it is not committed arena memory. */
int xbox_mem2_query(uint32_t page_base, uint32_t *bytes);

#ifdef __cplusplus
}
#endif

#endif /* XBOX_MEM2_H */
