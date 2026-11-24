/*
 * smcsim: the control core on the host (docs/core.md, "Simulator and scenarios"; SPEC LOG-40).
 *
 *   smcsim [--log FILE] [--config FILE]   commands on stdin, results on stdout
 *
 * The core runs on the test configuration (or the factory one: set config; or the binary form in
 * --config FILE, as smcconf encode writes it, which takes no settings) with a virtual clock:
 * every command carries its time in µs, and the log's timestamps are those times, so a run is
 * reproducible to the byte. With --log
 * the smclog stream (a header, then frames, as the device sends them) goes to FILE; decode it with
 * the dictionary of this binary (tools/smclog/smclog.py dict smcsim).
 *
 * Commands, one per line; indexes are numbers (`names` lists them):
 *   set NAME VALUE          a setting (core_settings.h), before the first event; config
 *                           (test, the default, or factory) before any other setting
 *   level MODULE LEVEL      smclog run-time level 0-5 of a module, or of "all"
 *   b T BUTTON 0|1 [inj]    release / press; inj marks an injected event (LOG-33)
 *   e T ENCODER CLICKS [inj]
 *   f T FADER POSITION [inj]   0-16383
 *   m T usb|ble HEX HEX HEX [inj]   incoming MIDI
 *   t T                     tick
 *   select T PRESET PAGE    0-based, -1 keeps
 *   edit T PRESET HEX       a live edit (P-1, CFG_SET): records of the binary form for preset
 *                           PRESET (0-based), checked and applied without a restart, as the
 *                           control task does (ctl.c, apply_edit)
 *   state                   prints "S PRESET PAGE SHIFT" (0-based)
 *   step N                  prints "# N" and logs "step N" (module test): step markers
 *   names                   prints "N b|e|f|l INDEX NAME" for every control and LED
 *   quit                    (or the end of the input)
 *
 * Output: "M T CAUSE HEX..." per MIDI message, "L T LED STATE" per LED change (all LEDs once at
 * start), "W TEXT" for a command it did not understand.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfgbin.h"
#include "core.h"
#include "core_settings.h"
#include "smclog.h"
#include "smclog_host.h"

static struct core core;
static struct core_config cfg;
static struct core_settings settings;
static int started;
static uint32_t now_us;
static FILE *log_file;
static struct core_config file_cfg; /* --config */
static int have_file_cfg;

static uint32_t sim_clock(void) { return now_us; }

static void on_midi(void *ctx, const uint8_t *m, unsigned len, uint16_t cause)
{
    (void)ctx;
    printf("M %u %u", now_us, cause);
    for (unsigned i = 0; i < len; i++)
        printf(" %02X", m[i]);
    printf("\n");
}

static void on_led(void *ctx, unsigned led, unsigned state)
{
    (void)ctx;
    printf("L %u %u %u\n", now_us, led, state);
}

static const struct core_io io = {on_midi, on_led, NULL};

static void drain_log(void)
{
    static uint8_t buf[4096];
    size_t n;
    if (!log_file)
        return;
    while ((n = smclog_drain(buf, sizeof buf)) > 0)
        fwrite(buf, 1, n, log_file);
    fflush(log_file);
}

static void start(void)
{
    if (started)
        return;
    started = 1;
    if (have_file_cfg)
        cfg = file_cfg;
    else
        core_settings_apply(&settings, &cfg);
    core_init(&core, &cfg, &io, NULL);
    core_set_bluetooth(&core, 1, false); /* as the controller with BLE: on, nobody connected */
}

/* "set": the settings are applied when the core starts (core_settings.h). */
static int cmd_set(const char *name, const char *value)
{
    return !started && !have_file_cfg && core_settings_add(&settings, name, value);
}

static int cmd_level(const char *module, int level)
{
    static const char *const names[] = {
#define SIM_MOD_NAME_(name) #name,
        SMCLOG_MODULES(SIM_MOD_NAME_)
#undef SIM_MOD_NAME_
    };
    if (level < 0 || level > SMCLOG_TRACE)
        return 0;
    for (int i = 0; i < SMCLOG_MOD_COUNT; i++)
        if (!strcmp(module, "all") || !strcmp(module, names[i]))
            smclog_set_level((enum smclog_module)i, (enum smclog_level)level);
    return 1;
}

static void names(void)
{
    for (unsigned i = 0; i < CORE_BUTTONS; i++)
        printf("N b %u %s\n", i, core_button_name(i));
    for (unsigned i = 0; i < CORE_STRIPS; i++)
        printf("N e %u enc%u\nN f %u fader%u\n", i, i + 1, i, i + 1);
    for (unsigned i = 0; i < CORE_LEDS; i++)
        printf("N l %u %s\n", i, core_led_name(i));
}

static void event(uint32_t t, uint8_t type, unsigned index, int value, const uint8_t *midi,
                  int injected)
{
    struct core_event ev;
    memset(&ev, 0, sizeof ev);
    now_us = t;
    start();
    ev.t_us = t;
    ev.type = type;
    ev.index = (uint8_t)index;
    ev.value = (int16_t)value;
    ev.flags = injected ? CORE_EV_INJECTED : 0;
    if (midi)
        memcpy(ev.midi, midi, 3);
    core_event(&core, &ev);
}

static unsigned edit_preset;

static void edit_changing(void *ctx, const struct core_control *old)
{
    (void)ctx;
    if (old)
        core_release_control(&core, edit_preset, old);
    else
        core_notes_off(&core);
}

static int cmd_edit(uint32_t t, unsigned preset, const char *hex)
{
    static uint8_t rec[512];
    size_t n = strlen(hex);
    if (n % 2 || n / 2 > sizeof rec)
        return 0;
    for (size_t i = 0; i < n / 2; i++) {
        unsigned x;
        if (sscanf(hex + 2 * i, "%2x", &x) != 1)
            return 0;
        rec[i] = (uint8_t)x;
    }
    now_us = t;
    start();
    struct cfgbin_error e;
    if (cfgbin_patch(rec, n / 2, preset, NULL, NULL, NULL, &e) != CFGBIN_OK)
        return 0;
    edit_preset = preset;
    smclog_cause_begin();
    LOG_INFO(engine, "edit: preset %u, %u bytes, revision %u", preset + 1u, (unsigned)(n / 2), 0u);
    cfgbin_patch(rec, n / 2, preset, &cfg, edit_changing, NULL, &e);
    core_reconfigured(&core);
    smclog_cause_end();
    return 1;
}

static int command(char *line)
{
    char op[16] = "", a[32] = "", b[32] = "", inj[8] = "";
    unsigned t = 0, i = 0, h[3] = {0, 0, 0};
    int v = 0;

    if (sscanf(line, "%15s", op) != 1)
        return 1; /* empty line */
    if (!strcmp(op, "quit"))
        return 0;
    if (!strcmp(op, "set") && sscanf(line, "set %31s %31s", a, b) == 2 && cmd_set(a, b))
        return 1;
    if (!strcmp(op, "level") && sscanf(line, "level %31s %d", a, &v) == 2 && cmd_level(a, v))
        return 1;
    if (!strcmp(op, "b") && sscanf(line, "b %u %u %d %7s", &t, &i, &v, inj) >= 3) {
        event(t, CORE_EV_BUTTON, i, v, NULL, !strcmp(inj, "inj"));
        return 1;
    }
    if (!strcmp(op, "e") && sscanf(line, "e %u %u %d %7s", &t, &i, &v, inj) >= 3) {
        event(t, CORE_EV_ENCODER, i, v, NULL, !strcmp(inj, "inj"));
        return 1;
    }
    if (!strcmp(op, "f") && sscanf(line, "f %u %u %d %7s", &t, &i, &v, inj) >= 3) {
        event(t, CORE_EV_FADER, i, v, NULL, !strcmp(inj, "inj"));
        return 1;
    }
    if (!strcmp(op, "m") &&
        sscanf(line, "m %u %31s %x %x %x %7s", &t, a, &h[0], &h[1], &h[2], inj) >= 5) {
        uint8_t m[3] = {(uint8_t)h[0], (uint8_t)h[1], (uint8_t)h[2]};
        event(t, CORE_EV_MIDI_IN, strcmp(a, "ble") ? CORE_FROM_USB : CORE_FROM_BLE, 0, m,
              !strcmp(inj, "inj"));
        return 1;
    }
    if (!strcmp(op, "t") && sscanf(line, "t %u", &t) == 1) {
        event(t, CORE_EV_TICK, 0, 0, NULL, 0);
        return 1;
    }
    int preset, page;
    if (!strcmp(op, "select") && sscanf(line, "select %u %d %d", &t, &preset, &page) == 3) {
        now_us = t;
        start();
        core_select(&core, preset, page);
        return 1;
    }
    static char hex[1100];
    if (!strcmp(op, "edit") && sscanf(line, "edit %u %u %1099s", &t, &i, hex) == 3 && cmd_edit(t, i, hex))
        return 1;
    if (!strcmp(op, "state")) {
        start();
        printf("S %u %u %u\n", core.preset, core.page, core.shift);
        return 1;
    }
    if (!strcmp(op, "step") && sscanf(line, "step %u", &i) == 1) {
        printf("# %u\n", i);
        LOG_INFO(test, "step %u", i);
        return 1;
    }
    if (!strcmp(op, "names")) {
        names();
        return 1;
    }
    line[strcspn(line, "\n")] = 0;
    printf("W %s\n", line);
    return 1;
}

/* --config FILE: a configuration's binary form (docs/config.md, "Binary form"). */
static int load_config(const char *path)
{
    static uint8_t buf[CFGBIN_MAX + 1];
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        return 0;
    }
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    struct cfgbin_error err;
    int rc = cfgbin_decode(buf, n, &file_cfg, NULL, &err);
    if (rc != CFGBIN_OK) {
        fprintf(stderr, "%s: %s at byte %u\n", path, cfgbin_error_name((unsigned)rc), (unsigned)err.offset);
        return 0;
    }
    have_file_cfg = 1;
    return 1;
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (i + 1 < argc && !strcmp(argv[i], "--log") && !log_file) {
            log_file = fopen(argv[++i], "wb");
            if (!log_file) {
                perror(argv[i]);
                return 2;
            }
        } else if (i + 1 < argc && !strcmp(argv[i], "--config") && !have_file_cfg) {
            if (!load_config(argv[++i]))
                return 2;
        } else {
            fprintf(stderr, "usage: smcsim [--log FILE] [--config FILE] < commands\n");
            return 2;
        }
    }
    smclog_host_set_clock(sim_clock);
    smclog_reset();
    core_settings_init(&settings, CORE_CONFIG_TEST);
    if (log_file) {
        uint8_t hdr[64];
        fwrite(hdr, 1, smclog_stream_header(hdr, sizeof hdr), log_file);
    }
    smclog_init();

    char line[256];
    while (fgets(line, sizeof line, stdin)) {
        int go = command(line);
        drain_log();
        if (!go)
            break;
    }
    start(); /* an empty run still starts the core once */
    drain_log();
    fflush(stdout);
    if (log_file)
        fclose(log_file);
    return 0;
}
