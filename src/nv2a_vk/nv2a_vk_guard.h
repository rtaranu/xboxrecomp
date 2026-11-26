/*
 * Page protection over the title's memory, for copy-back when the title looks
 * (NV2A_VK_COPYBACK_TOUCH): not here yet. Nothing can be guarded, so that mode
 * is copy-back at each flip.
 */
#ifndef NV2A_VK_GUARD_H
#define NV2A_VK_GUARD_H

#include <stddef.h>
#include <stdint.h>

typedef enum { GUEST_OPEN = 0, GUEST_READ_ONLY, GUEST_CLOSED } GuestGuard;

typedef struct {
    uint32_t va, bytes;
    int      write;
    uintptr_t code;
} GuestTouch;

static inline uint8_t *guest_guard_start(int (*serve)(const GuestTouch *t))
{
    (void)serve;
    return NULL;
}
static inline void guest_guard_own(void) {}
static inline void guest_guard_set(uint32_t va, uint32_t bytes, GuestGuard g)
{
    (void)va; (void)bytes; (void)g;
}
static inline void guest_guard_name(uintptr_t code, char *out, size_t bytes)
{
    (void)code;
    if (bytes)
        out[0] = 0;
}

#endif
