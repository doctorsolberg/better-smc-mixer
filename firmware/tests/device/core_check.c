/*
 * Device toolchain check for the control core and the input layer (built, never run): the core,
 * the factory configuration, the scan layer, the event queue, input injection and smclog compile with
 * the JieLi toolchain (LTO, -Oz, -Werror) and link into one image, wired the way the hardware-test
 * firmware wires them (docs/hwtest.md); their log call sites must be in the dictionary. Built by
 * tools/env/check_device.sh, which also reports code and RAM sizes. The configuration protocol
 * (link/cfglink.c) and the codec it uses (config/cfgbin.c) are linked in too: commands from a stand-in
 * channel, the configuration in use read and replaced (a small transfer buffer here).
 */
#include "cfgbin.h"
#include "cfglink.h"
#include "core.h"
#include "evq.h"
#include "inject.h"
#include "scan.h"
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

extern const uint32_t __build_id_note[];

const uint8_t *smclog_port_build_id(size_t *len)
{
    *len = __build_id_note[1];
    return (const uint8_t *)&__build_id_note[4];
}

/* What the board would receive: kept volatile so nothing is optimized away. */
static volatile uint32_t midi_sum, led_sum;

static void on_midi(void *ctx, const uint8_t *m, unsigned len, uint16_t cause)
{
    (void)ctx;
    for (unsigned i = 0; i < len; i++)
        midi_sum += m[i];
    midi_sum += cause;
}

struct scan scan;
struct evq evq;
struct inject inject;

uint32_t input_port_lock(void)
{
    return 0;
}

void input_port_unlock(uint32_t state)
{
    (void)state;
}

static void on_led(void *ctx, unsigned led, unsigned state)
{
    (void)ctx;
    led_sum += led * 4 + state;
    scan_led(&scan, led, state != CORE_LED_OFF);
}

static const struct core_io io = {on_midi, on_led, NULL};

struct core core;
struct core_config config;
uint8_t out[512];
volatile size_t out_len;
/* Stand for the pins and the command channel. */
volatile uint8_t rows_in;
volatile uint16_t adc_in;
volatile uint32_t word_out;
volatile uint8_t cmd_in[1 + INJECT_MAX_EVENTS * INJECT_EVENT_BYTES];
volatile size_t cmd_len;
volatile uint8_t reply_status;
volatile uint8_t cfg_in[1 + 4 + CFGLINK_CHUNK]; /* cmd, payload */
volatile size_t cfg_len;
volatile uint8_t cfg_status;
volatile size_t cfg_out_len;
uint8_t cfg_out[256];
uint8_t cfg_buf[1024];
struct cfglink cfgl;

static size_t cfg_get(void *ctx, uint8_t *o, size_t cap, struct cfgbin_error *err)
{
    (void)ctx;
    return cfgbin_encode(&config, 0, o, cap, err);
}

static bool cfg_apply(void *ctx, const uint8_t *blob, uint32_t len)
{
    (void)ctx;
    if (cfgbin_decode(blob, len, &config, 0, 0) != CFGBIN_OK)
        core_config_factory(&config);
    core_init(&core, &config, &io, 0);
    return true;
}

static bool cfg_pending(void *ctx)
{
    (void)ctx;
    return false;
}

static void cfg_active(void *ctx, uint8_t *source, struct cfgbin_meta *meta)
{
    (void)ctx;
    (void)meta;
    *source = 0;
}

static uint32_t cfg_touched(void *ctx)
{
    (void)ctx;
    return ticks;
}

static uint32_t cfg_state(void *ctx)
{
    (void)ctx;
    return ticks;
}

static const struct cfglink_ops cfg_ops = {0, cfg_get, cfg_apply, cfg_pending, cfg_active, cfg_touched, cfg_state,
                                           NULL, NULL, NULL, NULL, NULL, NULL}; /* no edits, no revert; saved by itself; no panel, no sent */

/* A stand-in flash: the debugger's view of these bytes */
static volatile uint8_t flash_word;
static bool fl_read(void *ctx, uint32_t addr, uint8_t *buf, uint32_t len)
{
    (void)ctx;
    for (uint32_t i = 0; i < len; i++)
        buf[i] = (uint8_t)(flash_word + addr + i);
    return true;
}
static bool fl_erase(void *ctx, uint32_t addr)
{
    (void)ctx;
    flash_word = (uint8_t)addr;
    return true;
}
static bool fl_program(void *ctx, uint32_t addr, const uint8_t *buf, uint32_t len)
{
    (void)ctx;
    flash_word = (uint8_t)(addr + buf[0] + len);
    return true;
}
static const struct cfgstore_flash flash_ops = {0, fl_read, fl_erase, fl_program};
static struct cfgstore store;
static struct statestore state_store;

int main(void)
{
    smclog_init();
    struct cfgbin_meta meta;
    cfgstore_init(&store, &flash_ops);
    cfglink_init(&cfgl, &cfg_ops, cfg_buf, sizeof cfg_buf, &store);
    cfglink_load(&cfgl, &config, &meta); /* the factory configuration if none */
    statestore_init(&state_store, &flash_ops);
    statestore_load(&state_store, NULL);
    cfglink_set_state(&cfgl, &state_store);
    core_init(&core, &config, &io, 0);
    evq_init(&evq);
    inject_init(&inject, &evq);
    scan_init(&scan, &scan_default_params, evq_sink, &evq);
    for (;;) {
        uint32_t t = ticks;
        word_out = scan_step(&scan, t, rows_in);
        scan_pass(&scan, t);
        scan_fader(&scan, t, t & 7u, adc_in);
        size_t n = cmd_len;
        if (n && n <= sizeof cmd_in) {
            uint8_t c[sizeof cmd_in];
            for (size_t i = 0; i < n; i++)
                c[i] = cmd_in[i];
            reply_status = inject_command(&inject, t, c, n).status;
            cmd_len = 0;
        }
        struct core_event ev;
        while (evq_pop(&evq, &ev))
            core_event(&core, &ev);
        n = cfg_len;
        if (n && n <= sizeof cfg_in) {
            uint8_t c[sizeof cfg_in];
            for (size_t i = 0; i < n; i++)
                c[i] = cfg_in[i];
            size_t len = 0;
            cfg_status = cfglink_command(&cfgl, c[0], c + 1, n - 1, cfg_out, &len);
            cfg_out_len = len;
            cfg_len = 0;
        }
        cfglink_poll(&cfgl, t);
        out_len = smclog_drain(out, sizeof out);
    }
}
