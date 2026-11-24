/*
 * What smclog and TinyUSB need from the bootloader's platform (no SDK).
 *
 * The bootloader is polled: it enables no interrupts (main.c), so the log needs no lock. There is
 * no timer driver yet (stage 2), so timestamps are 0. TinyUSB's device core and DFU class do not
 * wait on time; only its default delay, which nothing calls here, would.
 */
#include <stdint.h>

#include "dcd_br25.h"
#include "smclog_port.h"
#include "tusb.h"

/* namesz, descsz, type, "GNU\0", then the ID (boot.ld) */
extern const uint32_t __build_id_note[];

uint32_t smclog_port_time_us(void)
{
    return 0;
}

uint32_t smclog_port_lock(void)
{
    return 0;
}

void smclog_port_unlock(uint32_t state)
{
    (void)state;
}

const uint8_t *smclog_port_build_id(size_t *len)
{
    *len = __build_id_note[1];
    return (const uint8_t *)&__build_id_note[4];
}

uint32_t tusb_time_millis_api(void)
{
    return smclog_port_time_us() / 1000u;
}

/* The USB controller's interrupt stays masked: main.c calls dcd_int_handler() from its loop. */
void br25_usb_irq_set(bool enabled)
{
    (void)enabled;
}
