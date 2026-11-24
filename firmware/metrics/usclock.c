/* The microsecond clock (usclock.h). */
#include "usclock.h"

#include <string.h>

#include "smc_ram.h"

void usclock_init(struct usclock *c, uint32_t start_us, uint32_t period_us, uint32_t ticks_per_us)
{
    memset(c, 0, sizeof *c);
    c->base_us = start_us;
    c->last_us = start_us;
    c->period_us = period_us;
    c->ticks_per_us = ticks_per_us ? ticks_per_us : 1u;
}

SMC_RAM_CODE void usclock_period(struct usclock *c)
{
    c->base_us += c->period_us;
}

SMC_RAM_CODE void usclock_sync(struct usclock *c, uint32_t ref_us)
{
    int32_t drift = (int32_t)(c->base_us - ref_us);
    if (!c->have_drift || drift < c->drift_min_us)
        c->drift_min_us = drift;
    if (!c->have_drift || drift > c->drift_max_us)
        c->drift_max_us = drift;
    c->have_drift = true;
    if (drift < -(int32_t)USCLOCK_RESYNC_US) {
        c->base_us = ref_us;
        c->resyncs++;
        c->win_resyncs++;
    }
}

SMC_RAM_CODE uint32_t usclock_read(struct usclock *c, uint32_t cnt1, bool pending, uint32_t cnt2)
{
    uint32_t off;
    if (pending) /* the period ended, its interrupt has not run: cnt2 is in the next one */
        off = c->period_us + cnt2 / c->ticks_per_us;
    else /* cnt1 is in this period; cnt2 too unless the counter wrapped after the flag was read */
        off = (cnt2 >= cnt1 ? cnt2 : cnt1) / c->ticks_per_us;
    uint32_t t = c->base_us + off;
    if ((int32_t)(t - c->last_us) < 0)
        return c->last_us;
    c->last_us = t;
    return t;
}

void usclock_take_window(struct usclock *c, uint32_t *resyncs, int32_t *drift_min_us,
                         int32_t *drift_max_us)
{
    *resyncs = c->win_resyncs;
    *drift_min_us = c->have_drift ? c->drift_min_us : 0;
    *drift_max_us = c->have_drift ? c->drift_max_us : 0;
    c->win_resyncs = 0;
    c->have_drift = false;
}
