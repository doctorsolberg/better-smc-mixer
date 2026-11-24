/* SysEx bytes between the BLE task and the USB task (sxpipe.h). */
#include "sxpipe.h"

#include <string.h>

#define SXPIPE_BARRIER() __asm__ __volatile__("" ::: "memory")
#define MASK (SXPIPE_IN_SIZE - 1u)

_Static_assert((SXPIPE_IN_SIZE & MASK) == 0, "SXPIPE_IN_SIZE is a power of two");

void sxpipe_init(struct sxpipe *p)
{
    memset(p, 0, sizeof *p);
}

size_t sxpipe_put_in(struct sxpipe *p, const uint8_t *b, size_t n)
{
    uint16_t head = p->in_head, tail = p->in_tail;
    size_t room = MASK - ((head - tail) & MASK), k = n < room ? n : room;

    for (size_t i = 0; i < k; i++)
        p->in[(head + i) & MASK] = b[i];
    SXPIPE_BARRIER(); /* the bytes before the head that shows them */
    p->in_head = (uint16_t)((head + k) & MASK);
    p->in_dropped += (uint32_t)(n - k);
    return k;
}

size_t sxpipe_get_in(struct sxpipe *p, uint8_t *out, size_t cap)
{
    uint16_t head = p->in_head, tail = p->in_tail;
    size_t have = (head - tail) & MASK, k = have < cap ? have : cap;

    SXPIPE_BARRIER(); /* the head before the bytes it shows */
    for (size_t i = 0; i < k; i++)
        out[i] = p->in[(tail + i) & MASK];
    SXPIPE_BARRIER();
    p->in_tail = (uint16_t)((tail + k) & MASK);
    return k;
}

void sxpipe_drop_in(struct sxpipe *p)
{
    p->in_tail = p->in_head;
}

size_t sxpipe_put_out(struct sxpipe *p, const uint8_t *b, size_t n)
{
    if (p->out_len || !n || n > sizeof p->out)
        return 0;
    memcpy(p->out, b, n);
    SXPIPE_BARRIER();
    p->out_len = (uint16_t)n;
    return n;
}

const uint8_t *sxpipe_peek_out(const struct sxpipe *p, size_t *n)
{
    *n = p->out_len;
    SXPIPE_BARRIER();
    return *n ? p->out : NULL;
}

void sxpipe_done_out(struct sxpipe *p)
{
    SXPIPE_BARRIER();
    p->out_len = 0;
}
