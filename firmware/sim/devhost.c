/*
 * devhost: the controller (firmware/app/controller.c) on the host, behind the command link, for
 * testing the device scenario runner (tools/smcsim, "smcsim device"; SPEC LOG-35) without hardware.
 *
 *   devhost SECONDS [--fault ignore-settings|drop-midi] [--keep FILE] [--reset-cause RAW]
 *           [--midi PATH] [--flash FILE] [--boot FILE] [--sysex-fd N]
 *
 * stdin/stdout are the "serial port" (a pseudo-terminal's master side). What runs is the device's
 * portable code: the link (firmware/link), injection and the event queue (firmware/input), the
 * control path with the core on the factory configuration (firmware/ctl, firmware/core), the
 * USB-MIDI queue (firmware/midi), CORE_SETUP and the configuration protocol (firmware/link/cfglink.c:
 * the host reads the running configuration and replaces it, as smcconf pull/push do) as the
 * controller does them. With --flash, FILE stands in for the configuration's two flash slots
 * (store/cfgstore.h) and the panel's state after them (store/statestore.h): a model of NOR flash (an erase sets 0xFF, a program only clears bits), mapped
 * shared, so a received configuration is saved after the quiet pause and the next start (a RESET,
 * or a new devhost on the same FILE) loads it. Without --flash: RAM only, every start on the
 * factory configuration. BLE: a BLE queue with no central, so Bluetooth is on (its LED
 * blinks) and what the queue gets is thrown away, as on the controller. What is not here: the
 * scan, the faders' ADC, USB itself (the MIDI packets are counted, and with --midi written as MIDI
 * bytes to PATH, as a host would receive them on the performance port), the RTOS. The loop
 * wakes on bytes from the host or every 10 ms, as the controller's tasks do. The clock is the host's
 * monotonic one in microseconds since the start, like the device's log clock once its scan runs.
 *
 * The metrics heartbeat (metrics.h, LOG-06) comes every 10 seconds and on METRICS_NOW, with what
 * runs here: the control path's latency, run time and traffic, the stand-in USB's latency (input
 * event → packet taken) and the log's counters.
 *
 * The boot journal (diag.h, LOG-11) starts it, and BOOT_LOG writes it again. With --keep, what the
 * device keeps in RAM across a reset (struct diag_keep: the boot count, the last heartbeat, a crash
 * record, the recent log records) lives in FILE, mapped shared: it survives the process being
 * killed, as the RAM survives a reset, and the next run reports the previous one. SIGUSR1 stands in
 * for a crash: a crash record (cause DIAG_CAUSE_HOST_SIGNAL, made-up registers) is written there and
 * the process exits with status 70. Without --keep every run is a first boot.
 *
 * RESET (the link's, docs/link.md) restarts the process in place, as a reset restarts the device:
 * about 100 ms after the reply, devhost executes itself again with the same arguments (the same
 * pseudo-terminal, the same keep file) and --reset-cause set to what the device's journal would
 * read (P3_RST_SRC bits: 0x40 soft reset, 0x04 watchdog). The fault test writes a crash record
 * first, as SIGUSR1 does; the watchdog test waits a second first. The debug modes need injection
 * on, as on the device.
 *
 * --sysex-fd N: the config port (USB MIDI cable 1) on the open file N, a socket the caller passes
 * (smcconf --midi's tests): MIDI bytes both ways, the command link over SysEx as the controller
 * runs it (link_init_sysex, docs/link.md, "Over SysEx"); the replies go out through the USB-MIDI
 * packing (usbmidi_sysex_pack, then the bytes each packet carries), as a host receives them.
 *
 * --boot FILE: the mixer's whole flash (512 KB as stored: sector 0 pointing at our area, M-Vave's
 * area at 0x1000; tests/bootsel_vec.py makes one), mapped shared, for STOCK (going back to M-Vave's
 * firmware, store/bootsel.h): CHECK reads it, BACK writes its sector 0 and resets as RESET does;
 * a start on a FILE whose sector 0 points at M-Vave's area exits at once (status 0), as our
 * firmware no longer starts on the mixer. Without it STOCK is not offered.
 *
 * --fault ignore-settings: CORE_SETUP takes the configuration but drops the other settings, so the
 * device disagrees with the simulator (the runner's own test). --fault drop-midi: every third MIDI
 * message the core sends is not written to --midi's PATH, so what the host receives differs from
 * what the device logged (the runner's own test too).
 */
#include <errno.h>
#include <inttypes.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "../app/include/app_version.h" /* the product's version, as the firmware reports it */
#include "cfgbin.h"
#include "cfglink.h"
#include "cfgstore.h"
#include "statestore.h"
#include "core.h"
#include "core_settings.h"
#include "ctl.h"
#include "diag.h"
#include "evq.h"
#include "inject.h"
#include "link.h"
#include "metrics.h"
#include "bleq.h"
#include "bootsel.h"
#include "midiq.h"
#include "smclog.h"
#include "smclog_host.h"
#include "smclog_port.h"
#include "usbmidi.h"

static struct core_config config;
static struct evq queue;
static struct inject inj;
static struct midiq midi_out;
static struct bleq ble_out; /* as the controller with BLE=1, with no central: emptied unsent */
static struct ctl ctl;
static struct link lk;
static struct cfglink cfgl;
static uint8_t cfg_buf[CFGLINK_BUF_BYTES]; /* as on the device */
static struct cfgstore store;
static struct statestore state_store;
static uint8_t *flash; /* --flash: the slots' bytes, CFGSTORE_FLASH_AT on */
static int ignore_settings;
static int drop_midi;    /* --fault drop-midi */
static const char *midi_path; /* --midi */
static int midi_fd = -1;
static uint32_t midi_sent;

/* A USB-MIDI packet's MIDI bytes (USB MIDI 1.0, code index numbers) to the --midi stream; the
 * performance port (cable 0) only. PATH is opened when there is something to write and reopened
 * after its reader went away (a FIFO the runner opens, like a host that comes and goes): until
 * then the bytes are lost, as on a USB port with no host. */
static void midi_write(const uint8_t *p)
{
    static const uint8_t len[16] = {0, 0, 2, 3, 3, 1, 2, 3, 3, 3, 3, 3, 2, 2, 3, 1};
    if (!midi_path || (p[0] >> 4) != 0 || !len[p[0] & 0x0Fu])
        return;
    if (drop_midi && ++midi_sent % 3 == 0)
        return;
    if (midi_fd < 0)
        midi_fd = open(midi_path, O_WRONLY | O_NONBLOCK | O_NOCTTY | O_CLOEXEC);
    if (midi_fd >= 0 && write(midi_fd, p + 1, len[p[0] & 0x0Fu]) < 0 && errno != EAGAIN) {
        close(midi_fd);
        midi_fd = -1;
    }
}
static int sysex_fd = -1; /* --sysex-fd */
static struct link sx;

/* The config port: bytes from the host to the SysEx link, its replies back packet by packet. */
static int sysex_service(void)
{
    static uint8_t in[256], out[LINK_SYSEX_LEN(LINK_REPLY_MAX)];
    static size_t in_len, in_pos;

    if (sysex_fd < 0)
        return 1;
    for (;;) {
        if (in_pos == in_len) {
            ssize_t r = read(sysex_fd, in, sizeof in);
            if (r == 0)
                return 0; /* the host went away */
            in_len = r > 0 ? (size_t)r : 0;
            in_pos = 0;
        }
        size_t used = link_rx(&sx, in + in_pos, in_len - in_pos);
        in_pos += used;
        size_t n = link_tx(&sx, out, sizeof out), pos = 0;
        while (pos < n) {
            uint8_t pkt[4], b[3];
            unsigned k = usbmidi_sysex_pack(USBMIDI_CABLE_CONFIG, out + pos, (unsigned)(n - pos), pkt);
            unsigned m = usbmidi_packet_bytes(pkt, b);
            if (!k || m != k)
                return 0;
            for (unsigned i = 0; i < m;) { /* a socket: room soon */
                ssize_t w = write(sysex_fd, b + i, m - i);
                if (w > 0)
                    i += (unsigned)w;
                else if (w < 0 && errno != EAGAIN)
                    return 0;
            }
            pos += k;
        }
        if (!used && !n)
            return 1;
    }
}

static uint32_t packets, packets_from;
static struct lat_hist usb_lat;
static struct metrics_state mstate;
static struct diag_keep keep_ram, *keep = &keep_ram;
static struct diag_boot boot;
static uint64_t started_ms;
static char **args;           /* main's argv, for a reset */
static uint32_t reset_cause;  /* --reset-cause */
static int reset_due;         /* how + 1 */
static uint64_t reset_asked_ms;

#define HEARTBEAT_US 10000000u

/* evq.h: one thread here */
uint32_t input_port_lock(void) { return 0; }
void input_port_unlock(uint32_t state) { (void)state; }

static uint64_t clock_start_us; /* the log's clock counts from the start, as the device's does */

static uint64_t mono_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static uint32_t us_clock(void)
{
    return (uint32_t)(mono_us() - clock_start_us);
}

static uint64_t mono_ms(void);

static void heartbeat(void)
{
    static struct metrics_window w;
    metrics_begin(&mstate, &w, smclog_port_time_us());
    diag_keep_heartbeat(keep, w.seq, (uint32_t)(mono_ms() - started_ms));
    ctl_take_window(&ctl, &w);
    w.lat_usb = usb_lat;
    lat_hist_clear(&usb_lat);
    w.midi_out = packets - packets_from;
    packets_from = packets;
    metrics_log(&w);
}

static void metrics(void *ctx)
{
    (void)ctx;
    ctl_log_stats(&ctl);
    LOG_INFO(midi_usb, "devhost: %u MIDI packets taken by the stand-in USB", packets);
    heartbeat();
}

static void boot_log(void *ctx)
{
    (void)ctx;
    diag_boot_log(&boot);
}

/* What the device's exception handler does, with made-up registers. */
static void write_crash(void)
{
    struct diag_crash *c = &keep->crash;
    memset(c, 0, sizeof *c);
    c->tag = boot.tag;
    c->boot = keep->boots;
    c->t_ms = (uint32_t)(mono_ms() - started_ms);
    c->cause = DIAG_CAUSE_HOST_SIGNAL;
    c->frame = 0x10F00u;
    for (unsigned i = 0; i < DIAG_REGS; i++)
        c->reg[i] = 0xC0DE0000u + i;
    c->trace[0] = 0xC0DE1000u;
    c->stack_at = 0x20000u;
    for (unsigned i = 0; i < DIAG_STACK; i++)
        c->stack[i] = 0x5A000000u + i;
    memcpy(c->task, "devhost", 8);
    diag_crash_seal(c);
}

/* SIGUSR1: a crash. */
static void crash(int sig)
{
    (void)sig;
    write_crash();
    _exit(70);
}

/* The reset: this program again, on the same terminal and keep file, with the reset's cause. */
static void restart(uint32_t cause)
{
    static char *argv[64], value[16];
    int n = 0;
    for (int i = 0; args[i] && n < 60; i++) {
        if (!strcmp(args[i], "--reset-cause") && args[i + 1]) {
            i++;
            continue;
        }
        argv[n++] = args[i];
    }
    snprintf(value, sizeof value, "0x%" PRIx32, cause);
    argv[n++] = "--reset-cause";
    argv[n++] = value;
    argv[n] = NULL;
    execv(argv[0], argv);
    perror("devhost: restart");
    _exit(71);
}

static void do_reset(void *ctx, uint8_t how, uint8_t out[1])
{
    (void)ctx;
    if (how != LINK_RESET_SOFT && !inj.on) {
        out[0] = LINK_RESET_DISABLED;
        return;
    }
    static const char *const names[] = {"soft reset", "fault test", "watchdog test"};
    LOG_WARN(boot, "reset on request: %s in %u ms", names[how], 100u);
    reset_due = how + 1;
    reset_asked_ms = mono_ms();
    out[0] = LINK_RESET_OK;
}

/* From the loop: a reset that is due, once its reply is out. */
static void service_reset(void)
{
    if (!reset_due || mono_ms() - reset_asked_ms < 100)
        return;
    switch (reset_due - 1) {
    case LINK_RESET_FAULT:
        write_crash();
        restart(0x40); /* the handler ends in a soft reset */
        break;
    case LINK_RESET_WATCHDOG:
        sleep(1); /* the device: up to 8 s */
        restart(0x04);
        break;
    default:
        restart(0x40);
    }
}

/* The keep block in a file, mapped shared; NULL if that fails. */
static struct diag_keep *map_keep(const char *path)
{
    int fd = open(path, O_RDWR | O_CREAT, 0600);
    if (fd < 0 || ftruncate(fd, sizeof(struct diag_keep)) != 0)
        return NULL;
    void *p = mmap(NULL, sizeof(struct diag_keep), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    return p == MAP_FAILED ? NULL : p;
}

/* --flash: the configuration's slots and the panel's state as NOR flash in a file */
#define FLASH_AREA (STATESTORE_END - CFGSTORE_FLASH_AT)

static uint8_t *map_flash(const char *path)
{
    int fd = open(path, O_RDWR | O_CREAT, 0600);
    if (fd < 0 || ftruncate(fd, FLASH_AREA) != 0)
        return NULL;
    void *p = mmap(NULL, FLASH_AREA, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    return p == MAP_FAILED ? NULL : p;
}

static bool flash_in(uint32_t addr, uint32_t len)
{
    return addr >= CFGSTORE_FLASH_AT && len <= FLASH_AREA && addr - CFGSTORE_FLASH_AT <= FLASH_AREA - len;
}

static bool fl_read(void *ctx, uint32_t addr, uint8_t *buf, uint32_t len)
{
    (void)ctx;
    if (!flash_in(addr, len))
        return false;
    memcpy(buf, flash + (addr - CFGSTORE_FLASH_AT), len);
    return true;
}

static bool fl_erase(void *ctx, uint32_t addr)
{
    (void)ctx;
    if (addr % CFGSTORE_SECTOR || !flash_in(addr, CFGSTORE_SECTOR))
        return false;
    memset(flash + (addr - CFGSTORE_FLASH_AT), 0xFF, CFGSTORE_SECTOR);
    return true;
}

static bool fl_program(void *ctx, uint32_t addr, const uint8_t *buf, uint32_t len)
{
    (void)ctx;
    if (!len || addr / CFGSTORE_PAGE != (addr + len - 1) / CFGSTORE_PAGE || !flash_in(addr, len))
        return false;
    uint8_t *p = flash + (addr - CFGSTORE_FLASH_AT);
    for (uint32_t i = 0; i < len; i++)
        p[i] &= buf[i];
    return true;
}

static const struct cfgstore_flash flash_ops = {0, fl_read, fl_erase, fl_program};

/* --boot: the whole flash as NOR, for STOCK */
#define BOOT_FLASH 0x80000u
static uint8_t *bootfl;

static uint8_t *map_boot(const char *path)
{
    int fd = open(path, O_RDWR);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0 || st.st_size != BOOT_FLASH) {
        if (fd >= 0)
            close(fd);
        errno = EINVAL;
        return NULL;
    }
    void *p = mmap(NULL, BOOT_FLASH, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    return p == MAP_FAILED ? NULL : p;
}

static bool bt_read(void *ctx, uint32_t addr, uint8_t *buf, uint32_t len)
{
    (void)ctx;
    if (addr >= BOOT_FLASH || len > BOOT_FLASH - addr)
        return false;
    memcpy(buf, bootfl + addr, len);
    return true;
}

static bool bt_erase(void *ctx, uint32_t addr)
{
    (void)ctx;
    if (addr != 0) /* sector 0 alone, as flash_br25_boot_ops */
        return false;
    memset(bootfl, 0xFF, BOOTSEL_SECTOR);
    return true;
}

static bool bt_program(void *ctx, uint32_t addr, const uint8_t *buf, uint32_t len)
{
    (void)ctx;
    if (!len || addr + len > BOOTSEL_SECTOR || addr / BOOTSEL_PAGE != (addr + len - 1) / BOOTSEL_PAGE)
        return false;
    for (uint32_t i = 0; i < len; i++)
        bootfl[addr + i] &= buf[i];
    return true;
}

static const struct cfgstore_flash boot_ops = {0, bt_read, bt_erase, bt_program};
static uint8_t boot_sector[BOOTSEL_SECTOR];

static void do_reset(void *ctx, uint8_t how, uint8_t out[1]);

/* STOCK, as the controller does it (app/controller.c, do_stock) */
static void do_stock(void *ctx, uint8_t what, uint8_t out[LINK_STOCK_REPLY])
{
    struct bootsel_info i;
    memset(&i, 0, sizeof i);
    int st;
    if (flash && cfgstore_busy(&store)) {
        st = BOOTSEL_BUSY;
    } else if (what == LINK_STOCK_CHECK) {
        st = bootsel_check(&boot_ops, boot_sector, &i);
        LOG_INFO(boot, "going back to M-Vave's firmware, the check: %s (sector 0 points at 0x%x, its area 0x%x bytes)",
                 bootsel_name(st), (unsigned)i.pointer, (unsigned)i.stock_size);
    } else {
        LOG_WARN(boot, "going back to M-Vave's firmware: sector 0 is written");
        st = bootsel_to_stock(&boot_ops, boot_sector, &i);
        LOG_WARN(boot, "going back to M-Vave's firmware: %s, sector 0 points at 0x%x, %u tries", bootsel_name(st),
                 (unsigned)i.pointer, (unsigned)i.tries);
        if (st == BOOTSEL_OK) {
            uint8_t r;
            do_reset(ctx, LINK_RESET_SOFT, &r);
        }
    }
    out[0] = (uint8_t)st;
    for (unsigned k = 0; k < 4; k++)
        out[1 + k] = (uint8_t)(i.pointer >> (8 * k));
    out[5] = i.tries;
}

static void do_inject(void *ctx, uint32_t t_us, const uint8_t *p, size_t len, uint8_t out[3])
{
    (void)ctx;
    struct inject_reply r = inject_command(&inj, t_us, p, len);
    out[0] = r.status;
    out[1] = r.accepted;
    out[2] = r.refused;
}

static void do_setup(void *ctx, const uint8_t *p, size_t len, uint8_t out[2])
{
    struct core_settings s;
    (void)ctx;
    out[1] = 0;
    if (!inj.on) {
        out[0] = LINK_SETUP_DISABLED;
        return;
    }
    core_settings_init(&s, CORE_CONFIG_FACTORY);
    unsigned bad = core_settings_parse(&s, (const char *)p, len);
    if (bad) {
        out[0] = LINK_SETUP_BAD;
        out[1] = (uint8_t)(bad > 255 ? 255 : bad);
        return;
    }
    if (ignore_settings)
        s.count = 0;
    out[0] = ctl_request_setup(&ctl, &s) ? LINK_SETUP_OK : LINK_SETUP_BUSY;
}

/* The configuration protocol on the control path, as in the controller. */
static size_t cfg_get(void *ctx, uint8_t *out, size_t cap, struct cfgbin_error *err)
{
    (void)ctx;
    return ctl_encode_config(&ctl, out, cap, err);
}

static bool cfg_apply(void *ctx, const uint8_t *blob, uint32_t len)
{
    (void)ctx;
    return ctl_request_config(&ctl, blob, len);
}

static bool cfg_pending(void *ctx)
{
    (void)ctx;
    return ctl_change_pending(&ctl);
}

static void cfg_active(void *ctx, uint8_t *source, struct cfgbin_meta *meta)
{
    (void)ctx;
    if (ctl_change_pending(&ctl))
        return;
    *source = ctl.source;
    *meta = ctl.meta;
}

static uint32_t cfg_touched(void *ctx)
{
    (void)ctx;
    return ctl.last_touch_us;
}

static uint32_t cfg_state(void *ctx)
{
    (void)ctx;
    return ctl_state(&ctl);
}

static bool cfg_edit(void *ctx, unsigned preset, const uint8_t *rec, uint32_t len, uint32_t revision)
{
    (void)ctx;
    return ctl_request_edit(&ctl, preset, rec, len, revision);
}

static bool cfg_revert(void *ctx, const uint8_t *blob, uint32_t len)
{
    (void)ctx;
    return ctl_request_revert(&ctl, blob, len);
}

static bool cfg_autosave(void *ctx)
{
    (void)ctx;
    return ctl.cfg->global.save == CORE_SAVE_AUTO;
}

_Static_assert(CTL_SOURCE_RECEIVED == CFGLINK_SOURCE_RECEIVED, "the source cfglink saves");
_Static_assert(CTL_SOURCE_FACTORY == CFGLINK_SOURCE_FACTORY && CTL_SOURCE_STORED == CFGLINK_SOURCE_STORED,
               "the sources cfglink knows");

static void cfg_panel(void *ctx, uint8_t out[CFGLINK_PANEL_BYTES])
{
    (void)ctx;
    ctl_panel_bytes(ctl_panel(&ctl), out);
}

static bool cfg_select(void *ctx, int preset, int page)
{
    (void)ctx;
    if (!ctl_request_select(&ctl, preset, page))
        return false;
    return true;
}

static unsigned cfg_sent(void *ctx, uint32_t from, uint8_t (*out)[CFGLINK_SENT_LEN], unsigned max,
                         uint32_t *first)
{
    (void)ctx;
    return ctl_sent_read(&ctl, from, out, max, first);
}

_Static_assert(CTL_PANEL_BYTES == CFGLINK_PANEL_BYTES, "the panel's part of the state");
_Static_assert(CTL_SENT_LEN == CFGLINK_SENT_LEN && CTL_SENT_NOW == CFGLINK_SENT_NOW, "a sent entry");

static const struct cfglink_ops cfg_ops = {.get = cfg_get,
                                           .apply = cfg_apply,
                                           .pending = cfg_pending,
                                           .active = cfg_active,
                                           .touched_at = cfg_touched,
                                           .state = cfg_state,
                                           .edit = cfg_edit,
                                           .revert = cfg_revert,
                                           .autosave = cfg_autosave,
                                           .panel = cfg_panel,
                                           .select = cfg_select,
                                           .sent = cfg_sent};

static struct link_ops ops = { /* .stock with --boot */
    .variant = 0, .version = APP_VERSION, .metrics = metrics, .boot_log = boot_log, .reset = do_reset, .inject = do_inject,
    .setup = do_setup, .config = cfglink_command, .cfg = &cfgl, .cfg_poll = cfglink_poll};
static const struct ctl_ops no_leds = {0};

static uint64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* stdout shares the master's non-blocking open file with stdin: wait for room, up to a second. */
static int write_all(const uint8_t *p, size_t n)
{
    uint32_t start = us_clock();
    while (n) {
        ssize_t r = write(1, p, n);
        if (r > 0) {
            p += r;
            n -= (size_t)r;
            continue;
        }
        if ((r < 0 && errno != EAGAIN) || us_clock() - start > 1000000u)
            return 0;
        struct pollfd pfd = {.fd = 1, .events = POLLOUT};
        poll(&pfd, 1, 100);
    }
    return 1;
}

int main(int argc, char **argv)
{
    uint8_t in[256], out[1024];
    size_t in_len = 0, in_pos = 0;

    int bad = argc < 2;
    for (int i = 2; i < argc; i += 2) {
        const char *opt = argv[i], *val = i + 1 < argc ? argv[i + 1] : NULL;
        if (val && !strcmp(opt, "--fault") && !strcmp(val, "ignore-settings")) {
            ignore_settings = 1;
        } else if (val && !strcmp(opt, "--fault") && !strcmp(val, "drop-midi")) {
            drop_midi = 1;
        } else if (val && !strcmp(opt, "--midi")) {
            midi_path = val;
        } else if (val && !strcmp(opt, "--sysex-fd")) {
            sysex_fd = atoi(val);
            if (fcntl(sysex_fd, F_SETFL, fcntl(sysex_fd, F_GETFL) | O_NONBLOCK) < 0) {
                perror("--sysex-fd");
                return 1;
            }
        } else if (val && !strcmp(opt, "--reset-cause")) {
            reset_cause = (uint32_t)strtoul(val, NULL, 0);
        } else if (val && !strcmp(opt, "--flash")) {
            flash = map_flash(val);
            if (!flash) {
                perror(val);
                return 1;
            }
        } else if (val && !strcmp(opt, "--boot")) {
            bootfl = map_boot(val);
            if (!bootfl) {
                perror(val);
                return 1;
            }
            ops.stock = do_stock;
        } else if (val && !strcmp(opt, "--keep")) {
            keep = map_keep(val);
            if (!keep) {
                perror(val);
                return 1;
            }
        } else {
            bad = 1;
        }
    }
    if (bad) {
        fprintf(stderr, "usage: devhost SECONDS [--fault ignore-settings|drop-midi] [--keep FILE] "
                        "[--reset-cause RAW] [--midi PATH] [--flash FILE] [--boot FILE] [--sysex-fd N]\n");
        return 2;
    }
    if (bootfl) { /* the uboot follows sector 0: once it points at M-Vave's area, ours does not start */
        struct bootsel_info bi;
        int st = bootsel_check(&boot_ops, boot_sector, &bi);
        if (st == BOOTSEL_NOT_OURS) {
            fprintf(stderr, "devhost: sector 0 points at 0x%x: M-Vave's firmware starts, not this one\n", (unsigned)bi.pointer);
            return 0;
        }
    }
    uint64_t run_ms = (uint64_t)(atof(argv[1]) * 1000), start = mono_ms();
    started_ms = start;
    clock_start_us = mono_us();
    args = argv;

    smclog_host_set_clock(us_clock);
    smclog_reset();
    diag_boot_begin(&boot, keep, smclog_port_time_us(), reset_cause,
                    reset_cause ? "host restart" : "host start", "devhost");
    signal(SIGUSR1, crash);
    signal(SIGPIPE, SIG_IGN); /* --midi's reader gone: write() fails instead */
    smclog_init();
    metrics_state_init(&mstate, smclog_port_time_us());
    uint32_t next_heartbeat = smclog_port_time_us() + HEARTBEAT_US;
    LOG_INFO(boot, "devhost: the controller's portable code on the host%s",
             ignore_settings ? " (fault: settings ignored)" : "");
    struct cfgbin_meta meta;
    if (flash)
        cfgstore_init(&store, &flash_ops);
    cfglink_init(&cfgl, &cfg_ops, cfg_buf, sizeof cfg_buf, flash ? &store : NULL);
    bool stored = cfglink_load(&cfgl, &config, &meta); /* the factory configuration if none */
    evq_init(&queue);
    midiq_init(&midi_out);
    inject_init(&inj, &queue);
    ctl_init(&ctl, &config, &queue, &midi_out, &no_leds);
    bleq_init(&ble_out);
    ctl_set_ble(&ctl, &ble_out); /* Bluetooth on, as the controller with BLE=1 */
    if (stored)
        ctl_set_stored(&ctl, &meta);
    if (flash) { /* the panel's state as it was left, as the controller */
        uint32_t state;
        statestore_init(&state_store, &flash_ops);
        if (statestore_load(&state_store, &state))
            ctl_restore_state(&ctl, state);
        cfglink_set_state(&cfgl, &state_store);
    }
    diag_boot_mark(&boot, "core", smclog_port_time_us());
    link_init(&lk, &ops);
    link_init_sysex(&sx, &ops);
    diag_boot_mark(&boot, "link", smclog_port_time_us());
    diag_boot_done(&boot, smclog_port_time_us());
    fcntl(0, F_SETFL, fcntl(0, F_GETFL) | O_NONBLOCK);

    while (mono_ms() - start < run_ms) {
        struct pollfd pfd[2] = {{.fd = 0, .events = POLLIN}, {.fd = sysex_fd, .events = POLLIN}};
        if (poll(pfd, sysex_fd >= 0 ? 2 : 1, 10) > 0 && (pfd[0].revents & (POLLHUP | POLLERR)) &&
            !(pfd[0].revents & POLLIN))
            break;
        if (!sysex_service())
            break;
        for (;;) { /* the USB task: bytes in, the link's replies and log out */
            if (in_pos == in_len) {
                ssize_t r = read(0, in, sizeof in);
                if (r <= 0)
                    break;
                in_len = (size_t)r;
                in_pos = 0;
            }
            size_t used = link_rx(&lk, in + in_pos, in_len - in_pos);
            in_pos += used;
            size_t n = link_tx(&lk, out, sizeof out);
            if (n && !write_all(out, n))
                return 1;
            if (!used && !n)
                break;
        }
        do {
            ctl_run(&ctl, smclog_port_time_us()); /* the control task */
            cfglink_poll(&cfgl, smclog_port_time_us()); /* the USB task's autosave */
            while (midiq_peek(&midi_out)) {        /* the USB task's MIDI */
                uint32_t t;
                if (midiq_peek_time(&midi_out, &t)) {
                    int32_t lat = (int32_t)(smclog_port_time_us() - t);
                    lat_hist_add(&usb_lat, lat > 0 ? (uint32_t)lat : 0u);
                }
                midi_write(midiq_peek(&midi_out));
                midiq_pop(&midi_out);
                packets++;
            }
            (void)midiq_room_due(&midi_out); /* the control task runs again at once here */
            bleq_drop_all(&ble_out);             /* the BLE sender with no central */
            (void)bleq_room_due(&ble_out);
        } while (evq_depth(&queue)); /* a round held for the MIDI queue: the rest now */
        if ((int32_t)(smclog_port_time_us() - next_heartbeat) >= 0) {
            next_heartbeat += HEARTBEAT_US;
            heartbeat();
        }
        size_t n = link_tx(&lk, out, sizeof out);
        if (n && !write_all(out, n))
            return 1;
        service_reset();
    }
    return 0;
}
