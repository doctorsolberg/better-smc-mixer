/*
 * Without BLE=1 (firmware/app/Makefile): the BLE module's entry points the SDK expects from the
 * application (the test-box update code switches BLE off and on around an update), only logged,
 * and ble_br25.h's calls, which do nothing.
 */
#include <stdint.h>

#include "ble_br25.h"
#include "smclog.h"

void ble_module_enable(uint8_t en)
{
    LOG_WARN(ble, "ble_module_enable(%u): no BLE module in this build", en);
}

void ble_br25_set_midi(const struct ble_br25_midi *m)
{
    (void)m;
}

struct sxpipe *ble_br25_link_pipe(void (*wake)(void))
{
    (void)wake;
    return NULL;
}

void ble_br25_wake(void)
{
}

void ble_br25_enable(bool on)
{
    (void)on;
}

void ble_br25_take_window(struct metrics_window *w)
{
    (void)w;
}

void ble_br25_log_stats(void)
{
}
