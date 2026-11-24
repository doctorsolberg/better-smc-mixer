/*
 * Device toolchain check for smclog (built, never run): the call sites land in the smclog section,
 * which is not loaded (INFO), and the build-id note is in flash. Built by tools/env/check_device.sh,
 * which also checks the result with tools/smclog.
 */
#include "smclog.h"
#include "smclog_port.h"

static volatile uint32_t ticks;

uint32_t smclog_port_time_us(void)
{
    return ticks++;
}

uint32_t smclog_port_lock(void)
{
    return 0;
}

void smclog_port_unlock(uint32_t state)
{
    (void)state;
}

/* The GNU build-id note: namesz, descsz, type, "GNU\0", then the ID (check.ld). */
extern const uint32_t __build_id_note[];

const uint8_t *smclog_port_build_id(size_t *len)
{
    *len = __build_id_note[1];
    return (const uint8_t *)&__build_id_note[4];
}

uint8_t out[512];
volatile size_t out_len;

int main(void)
{
    smclog_init();
    LOG_INFO(test, "check %d %s", (int)ticks, "device");
    LOG_WARN(input, "i64 %lld f %f", (long long)ticks << 33, 0.5);
    out_len = smclog_stream_header(out, sizeof out);
    out_len += smclog_drain(out + out_len, sizeof out - out_len);
    return 0;
}
