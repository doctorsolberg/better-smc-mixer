/*
 * USB in the application (usb_app.h, docs/link.md, docs/usb.md).
 *
 * - TinyUSB, no OS layer, on our BR25 controller driver (platform/br25/usb). The SDK's own USB
 *   stack is not built (TCFG_PC_ENABLE off, no OTG detection), so the controller is ours.
 * - The controller's interrupt (IRQ 9, priority 3 like the SDK's USB device) runs
 *   dcd_int_handler() and wakes the USB task. It is installed and masked before tusb_init(), which
 *   attaches and then unmasks it (docs/usb.md, item 6).
 * - The task "smc_usb" runs tud_task() and the command link: the bytes the host sent go to
 *   link_rx(), what link_tx() has goes into the CDC FIFO, whole frames that fit. It wakes on every
 *   USB interrupt and at least every OS tick (10 ms): log records reach the host within ~10 ms.
 * - The port closed (DTR dropped), a bus reset or a lost configuration resets the link: the log
 *   stream stops until the host subscribes again.
 * - USB MIDI, in the same task: packets from the host are sorted (usbmidi.h); channel messages on
 *   the performance port go to the variant, the rest is counted. Packets for the host come from
 *   the variant's queue (midiq.h, filled by its control task, which wakes this task); while no
 *   host has the MIDI function configured, while the bus is suspended (a pulled cable looks like
 *   that: no bus reset, no SOF) and while the host has taken nothing for MIDI_STALL_US, they are
 *   thrown away and counted, so that a host never gets a burst of stale messages when it connects
 *   and the control task, which waits for room in the queue, never waits for a host that is gone.
 * - The command link over SysEx (docs/link.md, "Over SysEx"), in the same task, a link per
 *   transport: SysEx on the config port (cable 1) goes to its own link, whose replies go back on
 *   cable 1, a packet at a time as TinyUSB takes them; with BLE=1 the BLE task hands its SysEx
 *   through a pipe (sxpipe.h) to a third link here, so every link and the configuration protocol
 *   run in this one task. A link stops taking bytes while its reply waits: the config port's
 *   packets then wait in TinyUSB's FIFO, the performance port's with them.
 * - A reset the host asked for (the link's RESET) is done here, about 100 ms after its reply went
 *   into the FIFO (diag_br25.h).
 * - Metrics (LOG-06): a packet that carries its input event's time (midiq.h) adds input → USB
 *   latency when TinyUSB takes it (into its FIFO; the host's polling comes after); the task's run
 *   time per wake-up adds to its CPU share. usb_app_take_window() hands them to the heartbeat.
 *
 * Not verified on hardware (docs/usb.md, "Not checked without hardware").
 */
/* The SDK first: it typedefs bool, which <stdbool.h> (through the headers below) then redefines. */
#include "system/includes.h"

#include <string.h>

#include "tusb.h"
#include "device/dcd.h"

#include "app_tasks.h"
#include "dcd_br25.h"
#include "diag_br25.h"
#include "irq_br25.h"
#include "link.h"
#include "metrics_app.h"
#include "midiq.h"
#include "smclog.h"
#include "smclog_port.h"
#include "sxpipe.h"
#include "usb_app.h"
#include "usbmidi.h"

#define USB_IRQ_PRIORITY 3 /* the SDK's USB device */
#define USB_WAIT_TICKS 1   /* 10 ms */
#define ROUNDS_MAX 8       /* link rounds per wake-up */
#define MIDI_STALL_US 250000u /* packets wait and the host took none this long: it is not reading */

static OS_SEM usb_sem;
static struct link lk;
static const struct link_ops *link_ops;
static const struct usb_app_midi *midi;
static uint8_t in_buf[CFG_TUD_CDC_EP_BUFSIZE]; /* read from the CDC FIFO, not yet taken */
static uint16_t in_len, in_pos;
static uint8_t out_buf[512];
/* the link on the config port (cable 1): bytes of a packet not yet taken, the reply being sent */
static struct link mx;
static uint8_t mx_in[3], mx_in_len, mx_in_pos;
static uint8_t mx_out[LINK_SYSEX_LEN(LINK_REPLY_MAX)];
static uint16_t mx_out_len, mx_out_pos;
/* the link over BLE MIDI (usb_app_set_ble_link) */
static struct link bx;
static struct sxpipe *ble_pipe;
static void (*ble_wake)(void);
static uint32_t ble_epoch;
static uint8_t bx_in[64];
static uint16_t bx_in_len, bx_in_pos;
static uint8_t bx_out[LINK_SYSEX_LEN(LINK_REPLY_MAX)];
static bool was_up, was_dtr, was_suspended, midi_was_up;

static struct {
    uint32_t in, config, sysex, system, bad; /* packets from the host */
    uint32_t out;                            /* packets handed to TinyUSB */
    uint32_t stalls;                         /* runs of the host not reading (MIDI_STALL_US) */
} midi_stats;
static uint32_t midi_moved_us; /* the last packet handed over, or nothing waiting */
static bool midi_stalled;

static struct { /* the heartbeat's window, under the interrupt lock */
    struct lat_hist lat; /* input event → its packet handed to TinyUSB */
    uint32_t busy_us;
    uint32_t wakes, wake_max_us; /* the task's runs and the longest */
} win;
static struct { /* the USB interrupt's window, written by it, taken under the lock */
    uint32_t n, sum_us, max_us;
} isr_win;
static struct {
    uint32_t out, in, dropped, tx_packets;
} win_from;

void br25_usb_irq_set(bool enabled)
{
    irq_br25_enable(IRQ_USB_CTRL_IDX, enabled);
}

___interrupt static void usb_isr(void)
{
#ifdef SMC_STALL_CHECK
    diag_br25_usb_irqs++;
#endif
    uint32_t t0 = smclog_port_time_us();
    dcd_int_handler(0);
    os_sem_post(&usb_sem);
    uint32_t d = smclog_port_time_us() - t0;
    isr_win.n++;
    isr_win.sum_us += d;
    if (d > isr_win.max_us)
        isr_win.max_us = d;
}

static void link_down(const char *why)
{
    link_reset(&lk);
    in_len = in_pos = 0;
    tud_cdc_write_clear();
    LOG_INFO(usb, "command link reset: %s", why);
}

/* One wake-up's worth of USB MIDI: everything the host sent, then what waits for it. */
/* The config port's link: leftover bytes of the last packet offered again; false while its reply
 * still waits (no packet may be read then). */
static bool mx_feed(void)
{
    mx_in_pos += (uint8_t)link_rx(&mx, mx_in + mx_in_pos, mx_in_len - mx_in_pos);
    return mx_in_pos == mx_in_len;
}

/* The config port's reply, a packet at a time while TinyUSB takes them. */
static void mx_send(void)
{
    uint8_t pkt[4];

    if (mx_out_pos == mx_out_len) {
        mx_out_len = (uint16_t)link_tx(&mx, mx_out, sizeof mx_out);
        mx_out_pos = 0;
    }
    while (mx_out_pos < mx_out_len) {
        unsigned k = usbmidi_sysex_pack(USBMIDI_CABLE_CONFIG, mx_out + mx_out_pos,
                                        mx_out_len - mx_out_pos, pkt);
        if (!k || !tud_midi_packet_write(pkt))
            break; /* k is never 0 for a whole reply */
        mx_out_pos += (uint16_t)k;
    }
}

static void mx_down(void)
{
    link_reset(&mx);
    mx_in_len = mx_in_pos = 0;
    mx_out_len = mx_out_pos = 0;
}

static void midi_move(bool up)
{
    uint8_t pkt[4], msg[3];
    const uint8_t *p;

    up = up && tud_midi_mounted();
    if (up != midi_was_up)
        LOG_INFO(midi_usb, "USB MIDI %s", up ? "configured by the host" : "gone");
    midi_was_up = up;
    if (!up) {
        mx_down(); /* the config port's link starts again with the next host */
        if (midi && midi->out)
            midiq_drop_all(midi->out);
        midi_moved_us = smclog_port_time_us(); /* the stall is timed from when it is up */
        return;
    }
    mx_send();
    while (mx_feed() && tud_midi_packet_read(pkt)) {
        enum usbmidi_kind k = usbmidi_unpack(pkt, msg);
        if (k == USBMIDI_BAD) {
            if (midi_stats.bad++ == 0)
                LOG_WARN(midi_usb, "bad packet %02x %02x %02x %02x, counting the rest", pkt[0],
                         pkt[1], pkt[2], pkt[3]);
        } else if (USBMIDI_CABLE(pkt) == USBMIDI_CABLE_CONFIG) {
            midi_stats.config++; /* the command link over SysEx */
            if (k != USBMIDI_REALTIME) {
                mx_in_len = (uint8_t)usbmidi_packet_bytes(pkt, mx_in);
                mx_in_pos = 0;
                if (!mx_feed())
                    mx_send(); /* a command: its reply out, then the bytes after it */
            }
        } else if (USBMIDI_CABLE(pkt) != USBMIDI_CABLE_PERFORMANCE) {
            midi_stats.bad++; /* a port the device does not have */
        } else if (k == USBMIDI_CHANNEL) {
            midi_stats.in++;
            if (midi && midi->in)
                midi->in(msg);
        } else if (k == USBMIDI_SYSEX) {
            midi_stats.sysex++;
        } else {
            midi_stats.system++; /* clock, start, stop...: not used yet */
        }
    }
    mx_send(); /* a reply the last packet's bytes made */
    bool moved = false;
    while (midi && midi->out && (p = midiq_peek(midi->out)) != NULL && tud_midi_packet_write(p)) {
        moved = true;
        uint32_t t;
        if (midiq_peek_time(midi->out, &t)) {
            int32_t lat = (int32_t)(smclog_port_time_us() - t);
            uint32_t l = irq_br25_save();
            lat_hist_add(&win.lat, lat > 0 ? (uint32_t)lat : 0u);
            irq_br25_restore(l);
        }
        midiq_pop(midi->out);
        midi_stats.out++;
    }
    /* A host that is there but takes nothing (a cable pulled while the bus looks configured, an
     * endpoint nobody polls) must not hold the controls: the control task waits for room in the
     * queue (backpressure), so what waits is thrown away, as with no host. */
    uint32_t now = smclog_port_time_us();
    if (moved || !midi || !midi->out || !midiq_peek(midi->out)) {
        midi_moved_us = now;
        if (midi_stalled && moved)
            LOG_INFO(midi_usb, "USB MIDI: the host takes packets again");
        midi_stalled = midi_stalled && !moved;
    } else if (now - midi_moved_us >= MIDI_STALL_US) {
        if (!midi_stalled) {
            midi_stats.stalls++;
            LOG_WARN(midi_usb, "USB MIDI: the host took nothing for %u ms; what waits is dropped",
                     (unsigned)(MIDI_STALL_US / 1000u));
        }
        midi_stalled = true;
        midiq_drop_all(midi->out);
    }
}

/* midi_move(), then the producer woken if it waits for room (its backpressure, midiq.h). */
static void midi_service(bool up)
{
    midi_move(up);
    if (midi && midi->out && midi->room && midiq_room_due(midi->out))
        midi->room();
}

/* The link over BLE MIDI: SysEx from the BLE task in, its replies out through the pipe. Runs
 * with or without USB (the mixer may be on a power adapter). */
static void ble_service(void)
{
    struct sxpipe *p = ble_pipe;

    if (!p)
        return;
    if (p->epoch != ble_epoch) { /* a central came or went */
        ble_epoch = p->epoch;
        link_reset(&bx);
        sxpipe_drop_in(p);
        bx_in_len = bx_in_pos = 0;
    }
    for (unsigned round = 0; round < ROUNDS_MAX; round++) {
        if (bx_in_pos == bx_in_len) {
            bx_in_len = (uint16_t)sxpipe_get_in(p, bx_in, sizeof bx_in);
            bx_in_pos = 0;
        }
        size_t used = link_rx(&bx, bx_in + bx_in_pos, bx_in_len - bx_in_pos);
        bx_in_pos += (uint16_t)used;
        size_t n = 0;
        if (!p->out_len) { /* the slot is empty: the next reply, if there is one */
            n = link_tx(&bx, bx_out, sizeof bx_out);
            if (n) {
                sxpipe_put_out(p, bx_out, n);
                if (ble_wake)
                    ble_wake();
            }
        }
        if (!used && !n)
            break;
    }
}

/* One wake-up's worth of the command link and USB MIDI. */
static void service(void)
{
    bool up = tud_mounted(), dtr = up && tud_cdc_connected();
    bool suspended = up && tud_suspended(); /* no host awake: a cable pulled looks like this */

    if (was_up && !up)
        link_down("not configured (bus reset or detached)");
    else if (was_dtr && !dtr)
        link_down("the host closed the port");
    if (up && !was_up)
        LOG_INFO(usb, "configured by the host");
    was_up = up;
    was_dtr = dtr;
    if (suspended != was_suspended)
        LOG_INFO(usb, "%s", suspended ? "suspended: USB MIDI as with no host" : "awake again");
    was_suspended = suspended;
    midi_service(up && !suspended);
    ble_service();
    if (!up)
        return;

    for (unsigned round = 0; round < ROUNDS_MAX; round++) {
        if (in_pos == in_len) {
            in_len = (uint16_t)tud_cdc_read(in_buf, sizeof in_buf);
            in_pos = 0;
        }
        size_t used = link_rx(&lk, in_buf + in_pos, in_len - in_pos);
        in_pos += (uint16_t)used;
        uint32_t room = tud_cdc_write_available();
        size_t n = link_tx(&lk, out_buf, room < sizeof out_buf ? room : sizeof out_buf);
        if (n && tud_cdc_write(out_buf, (uint32_t)n) != n)
            LOG_ERROR(usb, "CDC FIFO took less than its free room"); /* would cut a frame */
        if (!used && !n)
            break;
    }
    tud_cdc_write_flush();
}

static void usb_task(void *priv)
{
    const tusb_rhport_init_t rh = {.role = TUSB_ROLE_DEVICE, .speed = TUSB_SPEED_FULL};

    (void)priv;
    metrics_app_task_start(APP_TASK_USB, APP_TASK_USB_STACK);
    request_irq(IRQ_USB_CTRL_IDX, USB_IRQ_PRIORITY, usb_isr, 0);
    br25_usb_irq_set(false); /* tusb_init() unmasks it once the stack is ready */
    if (!tusb_init(0, &rh)) {
        LOG_ERROR(usb, "USB init failed: no command link");
        for (;;)
            os_time_dly(100);
    }
    LOG_INFO(usb, "USB started: CDC command link and USB MIDI, product \"%s\"", usb_app_product);
    for (;;) {
        os_sem_pend(&usb_sem, USB_WAIT_TICKS);
        uint32_t t0 = smclog_port_time_us();
        tud_task();
        service();
        if (link_ops && link_ops->cfg_poll) /* the configuration's autosave, host or none */
            link_ops->cfg_poll(link_ops->cfg, smclog_port_time_us());
        diag_br25_service(); /* a reset the host asked for, once its reply is out */
        uint32_t took = smclog_port_time_us() - t0;
        uint32_t l = irq_br25_save();
        win.busy_us += took;
        win.wakes++;
        if (took > win.wake_max_us)
            win.wake_max_us = took;
        irq_br25_restore(l);
    }
}

void usb_app_wake(void)
{
    os_sem_post(&usb_sem);
}

void usb_app_start(const struct link_ops *ops, const char *product, const char *serial, const struct usb_app_midi *m)
{
    link_ops = ops;
    midi = m;
    usb_app_product = product;
    if (serial)
        usb_app_serial = serial;
    link_init(&lk, link_ops);
    link_init_sysex(&mx, link_ops);
    link_init_sysex(&bx, link_ops);
    os_sem_create(&usb_sem, 0);
    int err = task_create(usb_task, NULL, APP_TASK_USB);
    if (err)
        LOG_ERROR(usb, "USB task not created: error %d", err);
}

void usb_app_set_ble_link(struct sxpipe *p, void (*wake)(void))
{
    ble_pipe = p;
    ble_wake = wake;
}

static void log_link(const char *name, const struct link_stats *k)
{
    LOG_INFO(link, "%s: %u frames (%u bad, %u too long, %u not commands, %u foreign SysEx)", name,
             k->frames, k->bad_frames, k->overflows, k->ignored, k->foreign);
    LOG_INFO(link, "%s: %u commands, %u retries, %u replies", name, k->commands, k->retries,
             k->replies);
}

void usb_app_log_stats(void)
{
    const struct br25_usb_stats *u = br25_usb_stats();
    const struct link_stats *k = &lk.stats;

    LOG_INFO(usb, "driver: %u resets, %u suspends, %u resumes, %u SETUPs, %u stalls, %u SetupEnds",
             u->resets, u->suspends, u->resumes, u->setups, u->stalls_sent, u->setup_ends);
    LOG_INFO(usb, "driver: %u packets in, %u out; RX waits %u (max %u reads), %u failed; "
             "%u bytes cut, %u register timeouts",
             u->tx_packets, u->rx_packets, u->rx_waits, u->rx_wait_max, u->rx_wait_fails,
             u->rx_truncated, u->reg_timeouts);
    log_link("CDC", k);
    log_link("USB MIDI SysEx", &mx.stats);
    if (ble_pipe) {
        log_link("BLE MIDI SysEx", &bx.stats);
        LOG_INFO(link, "BLE MIDI SysEx: %u bytes did not fit the pipe", ble_pipe->in_dropped);
    }
    LOG_INFO(midi_usb, "USB MIDI: in %u channel, %u SysEx, %u system, %u config port, %u bad; "
             "out %u, %u dropped (no host or not reading)", midi_stats.in, midi_stats.sysex,
             midi_stats.system, midi_stats.config, midi_stats.bad, midi_stats.out,
             midi && midi->out ? midi->out->dropped : 0u);
    LOG_INFO(midi_usb, "USB MIDI: %u times the host stopped reading", midi_stats.stalls);
}

void usb_app_take_window(struct metrics_window *w)
{
    uint32_t l = irq_br25_save();
    w->lat_usb = win.lat;
    w->busy_usb_us = win.busy_us;
    uint32_t wakes = win.wakes, wake_max = win.wake_max_us, busy = win.busy_us;
    uint32_t isr_n = isr_win.n, isr_sum = isr_win.sum_us, isr_max = isr_win.max_us;
    memset(&win, 0, sizeof win);
    memset(&isr_win, 0, sizeof isr_win);
    irq_br25_restore(l);
    uint32_t tx = br25_usb_stats()->tx_packets; /* the USB task's: one word */
    LOG_INFO(usb, "USB task: %u runs, avg %u max %u us; interrupt: %u, avg %u max %u us; %u packets "
             "to the host", wakes, wakes ? busy / wakes : 0u, wake_max, isr_n,
             isr_n ? isr_sum / isr_n : 0u, isr_max, tx - win_from.tx_packets);
    win_from.tx_packets = tx;
    uint32_t out = midi_stats.out, in = midi_stats.in, /* the USB task's: one word each */
        dropped = midi && midi->out ? midi->out->dropped : 0u;
    w->midi_out = out - win_from.out;
    w->midi_in = in - win_from.in;
    w->midi_dropped = dropped - win_from.dropped;
    win_from.out = out;
    win_from.in = in;
    win_from.dropped = dropped;
}

/* TinyUSB callbacks (usb task) */

void tud_suspend_cb(bool remote_wakeup_en)
{
    (void)remote_wakeup_en;
    LOG_INFO(usb, "suspended");
}

void tud_resume_cb(void)
{
    LOG_INFO(usb, "resumed");
}

void tud_cdc_line_state_cb(uint8_t itf, bool dtr, bool rts)
{
    (void)itf;
    LOG_DEBUG(usb, "line state: DTR %u RTS %u", dtr, rts);
}
