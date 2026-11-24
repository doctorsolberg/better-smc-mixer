/*
 * BLE-MIDI on the SDK's BLE stack (SPEC FR-IO-03; docs/ble.md). Built only with BLE=1
 * (firmware/app/Makefile); without it ble_stub.c stands in.
 *
 * What it does: starts the SDK's BLE controller and host, serves a GATT database with the
 * BLE-MIDI service (Apple's specification: service 03B80E5A-EDE8-4B33-A751-6CE34EC4C700, one
 * characteristic 7772E5DB-3868-4112-A1A9-F2669D106BF3, read / write without response / notify),
 * advertises it under the stock firmware's name, asks for a 7.5..15 ms connection interval and
 * logs the link's events. MIDI (docs/ble.md, "Design"):
 *
 * - Out: the control task fills the BLE queue (midi/bleq.h, with the merge) and wakes the sender,
 *   our task "smc_ble". The sender packs whatever waits into notifications (midi/blemidi.c, as many
 *   messages as the ATT MTU takes, the send time as their timestamp) while the stack's send buffer
 *   has room for a whole one; when it has none, the stack's GATT_COMM_EVENT_CAN_SEND_NOW wakes it
 *   again. ⚠️ ble_comm_att_send_data() is called from our task, as the SDK's examples call it from
 *   theirs (trans_data: the UART's and a timer's). With no connection, or notifications off, what
 *   waits is dropped and counted, so a host that subscribes gets no burst of stale values.
 * - In: a write to the characteristic is decoded in the stack's task; channel messages go to the
 *   variant (the input queue, as CORE_FROM_BLE); the rest is counted.
 * - The command link over SysEx (docs/link.md, "Over SysEx"): SysEx the stack received goes into
 *   a pipe (link/sxpipe.h) to the USB task, which runs the link; the reply comes back through the
 *   pipe and the sender puts it into notifications ahead of the controls' messages (a SysEx
 *   once begun is finished before any other message, BLE-MIDI's rule). A connection made or lost
 *   starts the link again.
 * - Metrics: input event → its message in a notification the stack took (wide buckets, LOG-06).
 *
 * The start, as the SDK's examples do it (apps/spp_and_le/examples/trans_data): the BLE address
 * from the Bluetooth MAC, ble_comm_init() with our GATT callbacks, btstack_init(); the stack then
 * posts BT_STATUS_INIT_OK as a system event, and the handler sets the name, the profile and the
 * advertising and enables the module. GATT callbacks and events run in the BT stack's task.
 */
#include "system/includes.h"
#include "app_config.h"
#include "btstack/btstack_task.h"
#include "btstack/bluetooth.h"
#include "btstack/avctp_user.h"
#include "btcontroller_modules.h"
#include "user_cfg.h"
#include "le_common.h"
#include "gatt_common/le_gatt_common.h"

#include "app_tasks.h"
#include "ble_br25.h"
#include "blemidi.h"
#include "irq_br25.h"
#include "metrics_app.h"
#include "smclog.h"
#include "smclog_port.h"
#include "sxpipe.h"

/* Our address against M-Vave's (ble_br25_start); a build may choose another (1-15) for a test. */
#ifndef BLE_ADDR_XOR
#define BLE_ADDR_XOR 5
#endif

/* Not the stock name ("SMC-Mixer", ✅ its EXIF block), so a host's list tells the two firmwares
 * apart (owner, 2026-11-12, SPEC Q-25). The vendor's BLE→DIN adapter does not pick by name (✅
 * docs/ble.md, "The adapter and the name"). BLE_NAME=... as a make argument gives another. */
#ifndef BLE_NAME
#define BLE_NAME "Better-SMC-Mixer"
#endif

/* MTU 104: a BLE-MIDI packet of up to 101 bytes in one notification (docs/ble.md, "RAM"). */
#define BLE_MTU 104

/* GATT handles (the database below) */
#define H_GAP_NAME 0x0003 /* the device name's value */
#define H_MIDI_IO 0x000a  /* the BLE-MIDI characteristic's value */
#define H_MIDI_CCC 0x000b /* its client configuration */

/* The ATT database in the SDK's binary form (each entry: size, flags, handle, UUID, value; flags
 * 0x0100 dynamic, 0x0200 128-bit UUID, low byte the properties), as the SDK's profile generator
 * writes it. Little-endian throughout. */
#define MIDI_SVC_UUID \
    0x00, 0xc7, 0xc4, 0x4e, 0xe3, 0x6c, 0x51, 0xa7, 0x33, 0x4b, 0xe8, 0xed, 0x5a, 0x0e, 0xb8, 0x03
#define MIDI_IO_UUID \
    0xf3, 0x6b, 0x10, 0x9d, 0x66, 0xf2, 0xa9, 0xa1, 0x12, 0x41, 0x68, 0x38, 0xdb, 0xe5, 0x72, 0x77
static const uint8_t ble_profile[] = {
    /* 0x0001 primary service 1800 (GAP) */
    0x0a, 0x00, 0x02, 0x00, 0x01, 0x00, 0x00, 0x28, 0x00, 0x18,
    /* 0x0002 characteristic 2a00 (device name) READ | DYNAMIC; 0x0003 its value */
    0x0d, 0x00, 0x02, 0x00, 0x02, 0x00, 0x03, 0x28, 0x02, 0x03, 0x00, 0x00, 0x2a,
    0x08, 0x00, 0x02, 0x01, 0x03, 0x00, 0x00, 0x2a,
    /* 0x0004 primary service 1801 (GATT) */
    0x0a, 0x00, 0x02, 0x00, 0x04, 0x00, 0x00, 0x28, 0x01, 0x18,
    /* 0x0005 characteristic 2a05 (service changed) INDICATE; 0x0006 its value; 0x0007 its CCC */
    0x0d, 0x00, 0x02, 0x00, 0x05, 0x00, 0x03, 0x28, 0x20, 0x06, 0x00, 0x05, 0x2a,
    0x08, 0x00, 0x20, 0x00, 0x06, 0x00, 0x05, 0x2a,
    0x0a, 0x00, 0x0a, 0x01, 0x07, 0x00, 0x02, 0x29, 0x00, 0x00,
    /* 0x0008 primary service 03B80E5A-EDE8-4B33-A751-6CE34EC4C700 (BLE-MIDI) */
    0x18, 0x00, 0x02, 0x00, 0x08, 0x00, 0x00, 0x28, MIDI_SVC_UUID,
    /* 0x0009 characteristic 7772E5DB-3868-4112-A1A9-F2669D106BF3 READ | WRITE_WITHOUT_RESPONSE |
     * NOTIFY | DYNAMIC; 0x000a its value; 0x000b its CCC */
    0x1b, 0x00, 0x02, 0x00, 0x09, 0x00, 0x03, 0x28, 0x16, 0x0a, 0x00, MIDI_IO_UUID,
    0x16, 0x00, 0x16, 0x03, 0x0a, 0x00, MIDI_IO_UUID,
    0x0a, 0x00, 0x0a, 0x01, 0x0b, 0x00, 0x02, 0x29, 0x00, 0x00,
    /* end */
    0x00, 0x00,
};

/* 7.5..15 ms, no latency, 4 s supervision (FR-IO-03); the central picks one or refuses. */
static const struct conn_update_param_t ble_conn_params[] = {
    {6, 12, 0, 400},
    {9, 12, 0, 400},
    {12, 12, 0, 400},
};

static volatile uint16_t ble_con; /* 0 = no connection */
static volatile uint8_t ble_want = 1; /* Bluetooth on: ble_br25_enable() */
static uint8_t ble_applied;           /* what the module was last told (the sender's task) */
static volatile uint8_t ble_up;       /* the stack is up and set (its start applied ble_want) */
static volatile uint8_t ble_notify_on; /* the central turned MIDI notifications on */
static volatile uint16_t ble_payload = 20; /* a notification's bytes: the ATT MTU less 3 */
static uint32_t ble_rx_writes, ble_rx_bytes;

/* MIDI */
static const struct ble_br25_midi *midi;
static OS_SEM ble_sem;
static struct blemidi_rx rx;     /* the stack's task */
static struct blemidi_tx tx;     /* the sender's */
static struct sxpipe link_pipe;  /* the command link's SysEx, to and from the USB task */
static void (*link_wake)(void);  /* wakes the USB task */
static size_t sx_sent;           /* the sender's: bytes of the waiting reply already sent */
static uint32_t sx_epoch;        /* the sender's: the pipe's epoch it last saw */
static uint32_t sx_replies;      /* the sender's: replies sent whole */
static uint32_t sx_refused;      /* the sender's: replies the encoder refused (never: a bug) */
static struct {
    uint32_t out, notify, dropped, send_fail; /* the sender's */
    uint32_t in, ignored;                     /* the stack's task's: messages passed on, others */
    /* the others by kind: clock F8, start/continue/stop FA-FC, active sensing FE, reset FF, System
     * Common (F1-F3, F6), SysEx (docs/ble.md, "The vendor's adapter on the mixer") */
    uint32_t clock, transport, sensing, reset, common, sysex;
} st;
static struct { /* the heartbeat's window, under the interrupt lock */
    struct lat_hist lat; /* wide */
} win;
static struct {
    uint32_t out, notify, dropped, in, bad;
} win_from;
static uint8_t ble_adv_data[ADV_RSP_PACKET_MAX];
static uint8_t ble_rsp_data[ADV_RSP_PACKET_MAX];
static adv_cfg_t ble_adv;

/* --- in: the stack's task ---------------------------------------------------------------------- */

static void rx_msg(void *ctx, uint16_t ts, const uint8_t *m, unsigned len)
{
    (void)ctx;
    (void)ts;
    if (m[0] >= 0x80 && m[0] < 0xF0 && len >= 2 && midi && midi->in) {
        const uint8_t msg[3] = {m[0], m[1], len > 2 ? m[2] : 0};
        st.in++;
        midi->in(msg);
    } else {
        st.ignored++; /* System Common, real-time: not used yet */
        switch (len ? m[0] : 0) {
        case 0xF8: st.clock++; break;
        case 0xFA: case 0xFB: case 0xFC: st.transport++; break;
        case 0xFE: st.sensing++; break;
        case 0xFF: st.reset++; break;
        default: st.common++; break;
        }
    }
}

static void rx_sysex(void *ctx, const uint8_t *bytes, unsigned len)
{
    (void)ctx;
    st.sysex += len ? 1u : 0u;
    if (!link_wake) {
        st.ignored += len ? 1u : 0u;
        return;
    }
    sxpipe_put_in(&link_pipe, bytes, len); /* what does not fit is counted there */
    link_wake();
}

static const struct blemidi_sink rx_sink = {.msg = rx_msg, .sysex = rx_sysex};

/* --- out: the sender's task ------------------------------------------------------------------- */

/* The command link's reply that waits, or the rest of it, into the packet begun. */
static void add_reply(uint16_t ts)
{
    size_t len;
    const uint8_t *r = sxpipe_peek_out(&link_pipe, &len);
    if (!r)
        return;
    unsigned k = blemidi_tx_sysex(&tx, ts, r + sx_sent, (unsigned)(len - sx_sent));
    sx_sent += k;
    if (sx_sent == len || (!k && !blemidi_tx_len(&tx))) { /* sent, or not a SysEx (never) */
        sx_replies += sx_sent == len;
        if (sx_sent != len) {
            /* Refused: end the SysEx the encoder may have begun, or it refuses every reply
             * after this one (the receiver drops the unfinished message). */
            sx_refused++;
            tx.sysex = false;
        }
        sx_sent = 0;
        sxpipe_done_out(&link_pipe);
        if (link_wake)
            link_wake(); /* the slot is free for the next reply */
    }
}

/* One notification: whatever waits, as much as fits. False when nothing was sent. */
static bool send_one(uint16_t con, uint16_t cap)
{
    static uint8_t buf[BLE_MTU - 3];
    size_t sx_len;
    const struct bleq_msg *e = bleq_peek(midi->out);
    if (!e && !sxpipe_peek_out(&link_pipe, &sx_len))
        return false;
    uint32_t now = smclog_port_time_us();
    uint16_t ts = (uint16_t)((now / 1000u) & BLEMIDI_TS_MASK);
    unsigned n = 0;
    blemidi_tx_begin(&tx, buf, cap);
    add_reply(ts);
    for (e = bleq_peek(midi->out); e != NULL && !tx.sysex; e = bleq_peek(midi->out)) {
        if (!blemidi_tx_add(&tx, ts, e->m, bleq_msg_len(e->m[0])))
            break; /* full: the rest goes in the next one */
        if (e->timed) {
            int32_t lat = (int32_t)(now - e->t_us);
            uint32_t l = irq_br25_save();
            lat_hist_add(&win.lat, lat > 0 ? (uint32_t)lat : 0u);
            irq_br25_restore(l);
        }
        bleq_pop(midi->out);
        n++;
    }
    if (!blemidi_tx_len(&tx))
        return false;
    int r = ble_comm_att_send_data(con, H_MIDI_IO, buf, (uint16_t)blemidi_tx_len(&tx), ATT_OP_NOTIFY);
    if (r) {
        if (st.send_fail++ == 0)
            LOG_WARN(midi_ble, "notification of %u messages refused: %d (counting the rest)", n, r);
    } else {
        st.notify++;
        st.out += n;
    }
    return true;
}

/* The sender's task: Bluetooth switched on or off since the module was last told. ⚠️ The SDK's
 * examples switch the module from their own task (trans_data: the application's), as this does
 * from ours; the stack's commands are queued for its task. */
static void apply_enable(void)
{
    uint8_t want = ble_want;
    if (!ble_up || want == ble_applied)
        return;
    ble_applied = want;
    LOG_INFO(ble, "bluetooth %s: advertising %s", want ? "on" : "off", want ? "again" : "stopped");
    ble_module_enable(want);
}

static void service(void)
{
    apply_enable();
    if (!midi || !midi->out)
        return;
    uint16_t con = ble_con;
    size_t sx_len;
    if (link_pipe.epoch != sx_epoch || !con || !ble_notify_on) {
        /* no one to send to, or another central: the reply for the last one is dropped */
        sx_epoch = link_pipe.epoch;
        if (sx_sent || tx.sysex)
            blemidi_tx_init(&tx);
        sx_sent = 0;
        if (sxpipe_peek_out(&link_pipe, &sx_len))
            sxpipe_done_out(&link_pipe);
    }
    if (!con || !ble_notify_on) {
        st.dropped += bleq_drop_all(midi->out);
    } else {
        /* room for a whole notification, its ATT header counted (⚠️ whether the stack's buffer
         * counts it is not known; this errs on the safe side) */
        uint16_t cap = ble_payload;
        while (ble_comm_att_check_send(con, (uint16_t)(cap + ATT_PACKET_HEAD_SIZE)) && send_one(con, cap))
            ;
    }
    if (midi->room && bleq_room_due(midi->out))
        midi->room();
}

static void ble_task(void *priv)
{
    (void)priv;
    metrics_app_task_start(APP_TASK_BLE, APP_TASK_BLE_STACK);
    for (;;) {
        os_sem_pend(&ble_sem, 1); /* 10 ms at most */
        service();
    }
}

void ble_br25_set_midi(const struct ble_br25_midi *m)
{
    midi = m;
}

struct sxpipe *ble_br25_link_pipe(void (*wake)(void))
{
    link_wake = wake;
    return &link_pipe;
}

void ble_br25_wake(void)
{
    os_sem_post(&ble_sem);
}

void ble_br25_enable(bool on)
{
    ble_want = on;
    ble_br25_wake();
}

void ble_br25_take_window(struct metrics_window *w)
{
    uint32_t l = irq_br25_save();
    w->lat_ble = win.lat;
    lat_hist_clear_wide(&win.lat);
    irq_br25_restore(l);
    w->have_ble = true;
    uint32_t out = st.out, notify = st.notify, dropped = st.dropped, in = st.in, bad = rx.bad;
    w->ble_out = out - win_from.out;
    w->ble_notify = notify - win_from.notify;
    w->ble_dropped = dropped - win_from.dropped;
    w->ble_in = in - win_from.in;
    w->ble_in_bad = bad - win_from.bad;
    win_from.out = out;
    win_from.notify = notify;
    win_from.dropped = dropped;
    win_from.in = in;
    win_from.bad = bad;
}

void ble_br25_log_stats(void)
{
    LOG_INFO(midi_ble, "BLE MIDI out: %u messages in %u notifications, %u dropped (no connection), "
             "%u notifications refused by the stack", st.out, st.notify, st.dropped, st.send_fail);
    LOG_INFO(midi_ble, "BLE MIDI in: %u writes, %u bytes: %u messages, %u others, %u faults",
             ble_rx_writes, ble_rx_bytes, st.in, st.ignored, rx.bad);
    LOG_INFO(midi_ble, "BLE MIDI SysEx: %u messages in, %u bytes did not fit the link's pipe; %u replies out, "
             "%u refused", st.sysex, link_pipe.in_dropped, sx_replies, sx_refused);
    if (st.ignored)
        LOG_INFO(midi_ble, "BLE MIDI in, the others: %u clock, %u start/continue/stop, %u active sensing, "
                 "%u reset, %u System Common, %u SysEx",
                 st.clock, st.transport, st.sensing, st.reset, st.common, st.sysex);
}

static uint16_t ble_att_read(hci_con_handle_t con, uint16_t handle, uint16_t offset, uint8_t *buf,
                             uint16_t size)
{
    uint16_t n = 0;
    switch (handle) {
    case H_GAP_NAME: {
        const char *name = BLE_NAME;
        uint16_t len = (uint16_t)strlen(name);
        if (offset >= len) {
            return 0;
        }
        n = (uint16_t)(len - offset);
        if (buf) {
            if (n > size) {
                n = size;
            }
            memcpy(buf, name + offset, n);
        }
        return n;
    }
    case H_MIDI_IO:
        return 0; /* Apple's specification: a read returns no payload */
    case H_MIDI_CCC:
        if (buf) {
            buf[0] = (uint8_t)ble_gatt_server_characteristic_ccc_get(con, handle);
            buf[1] = 0;
        }
        return 2;
    default:
        return 0;
    }
}

static int ble_att_write(hci_con_handle_t con, uint16_t handle, uint16_t mode, uint16_t offset,
                         uint8_t *buf, uint16_t size)
{
    switch (handle) {
    case H_MIDI_CCC:
        ble_gatt_server_characteristic_ccc_set(con, handle, buf[0]);
        ble_notify_on = buf[0] != 0;
        LOG_INFO(ble, "MIDI notifications %s", buf[0] ? "on" : "off");
        ble_br25_wake();
        ble_gatt_server_connetion_update_request(
            con, ble_conn_params, sizeof ble_conn_params / sizeof ble_conn_params[0]);
        break;
    case H_MIDI_IO:
        ble_rx_writes++;
        ble_rx_bytes += size;
        LOG_TRACE(midi_ble, "write of %u bytes, header %02x", size, size ? buf[0] : 0);
        blemidi_rx_packet(&rx, buf, size, &rx_sink);
        break;
    default:
        break;
    }
    return 0;
}

static int ble_event(int event, uint8_t *packet, uint16_t size, uint8_t *ext)
{
    switch (event) {
    case GATT_COMM_EVENT_CAN_SEND_NOW:
        ble_br25_wake();
        break;
    case GATT_COMM_EVENT_CONNECTION_COMPLETE:
        ble_notify_on = 0;
        ble_payload = 20;
        blemidi_rx_init(&rx);
        link_pipe.epoch++; /* the USB task starts the link again */
        ble_con = little_endian_read_16(packet, 0);
        LOG_INFO(ble, "connected: handle %04x, interval %u x 1.25 ms, latency %u, timeout %u x 10 ms",
                 ble_con, little_endian_read_16(ext, 14), little_endian_read_16(ext, 16),
                 little_endian_read_16(ext, 18));
        if (midi && midi->link)
            midi->link(true);
        break;
    case GATT_COMM_EVENT_DISCONNECT_COMPLETE:
        LOG_INFO(ble, "disconnected: handle %04x, reason %02x; %u writes, %u bytes received",
                 little_endian_read_16(packet, 0), packet[2], ble_rx_writes, ble_rx_bytes);
        if (ble_con == little_endian_read_16(packet, 0)) {
            ble_con = 0;
            ble_notify_on = 0;
            link_pipe.epoch++;
            ble_br25_wake(); /* drops what waits */
            if (midi && midi->link)
                midi->link(false);
        }
        break;
    case GATT_COMM_EVENT_CONNECTION_UPDATE_COMPLETE:
        LOG_INFO(ble, "connection parameters: interval %u x 1.25 ms, latency %u, timeout %u x 10 ms",
                 little_endian_read_16(ext, 6), little_endian_read_16(ext, 8),
                 little_endian_read_16(ext, 10));
        break;
    case GATT_COMM_EVENT_MTU_EXCHANGE_COMPLETE: {
        uint16_t mtu = little_endian_read_16(packet, 2);
        if (mtu > BLE_MTU)
            mtu = BLE_MTU;
        ble_payload = mtu > 23 ? (uint16_t)(mtu - 3) : 20;
        LOG_INFO(ble, "ATT MTU %u: notifications of %u bytes", little_endian_read_16(packet, 2),
                 ble_payload);
        break;
    }
    case GATT_COMM_EVENT_ENCRYPTION_CHANGE:
        LOG_INFO(ble, "encryption: state %u, process %u", packet[2], packet[3]);
        break;
    default:
        break;
    }
    return 0;
}

static const gatt_server_cfg_t ble_server_cfg = {
    .att_read_cb = ble_att_read,
    .att_write_cb = ble_att_write,
    .event_packet_handler = ble_event,
};

#if CONFIG_BT_SM_SUPPORT_ENABLE
static const sm_cfg_t ble_sm_cfg = {
    .slave_security_auto_req = 0,
    .slave_set_wait_security = 0,
    .io_capabilities = IO_CAPABILITY_NO_INPUT_NO_OUTPUT,
    /* Just Works when a central asks (the vendor's adapter does), no keys stored, so the stack
     * never writes flash (SPEC Q-24, the owner, 2026-11-13; docs/ble.md) */
    .authentication_req_flags = SM_AUTHREQ_NO_BONDING,
    .min_key_size = 7,
    .max_key_size = 16,
    .sm_cb_packet_handler = NULL,
};
#endif

static gatt_ctrl_t ble_ctrl = {
    .mtu_size = BLE_MTU,
    .cbuffer_size = 2 * (ATT_PACKET_HEAD_SIZE + BLE_MTU),
    .multi_dev_flag = 0,
    .server_config = (gatt_server_cfg_t *)&ble_server_cfg,
    .client_config = NULL,
#if CONFIG_BT_SM_SUPPORT_ENABLE
    .sm_config = (sm_cfg_t *)&ble_sm_cfg,
#else
    .sm_config = NULL,
#endif
    .hci_cb_packet_handler = NULL,
};

static void ble_adv_setup(void)
{
    static const uint8_t midi_uuid[16] = {MIDI_SVC_UUID};
    uint8_t n = 0;
    n += make_eir_packet_val(&ble_adv_data[n], n, HCI_EIR_DATATYPE_FLAGS,
                             FLAGS_GENERAL_DISCOVERABLE_MODE | FLAGS_EDR_NOT_SUPPORTED, 1);
    n += make_eir_packet_data(&ble_adv_data[n], n, HCI_EIR_DATATYPE_COMPLETE_128BIT_SERVICE_UUIDS,
                              (void *)midi_uuid, sizeof midi_uuid);
    ble_adv.adv_data = ble_adv_data;
    ble_adv.adv_data_len = n;
    n = 0;
    n += make_eir_packet_data(&ble_rsp_data[n], n, HCI_EIR_DATATYPE_COMPLETE_LOCAL_NAME,
                              (void *)BLE_NAME, sizeof BLE_NAME - 1);
    ble_adv.rsp_data = ble_rsp_data;
    ble_adv.rsp_data_len = n;
    ble_adv.adv_interval = 48; /* 30 ms (0.625 ms units) */
    ble_adv.adv_auto_do = 1;
    ble_adv.adv_type = ADV_IND;
    ble_adv.adv_channel = ADV_CHANNEL_ALL;
    ble_gatt_server_set_adv_config(&ble_adv);
}

/* The SDK calls this from its start-up paths (update code); the module's switch. */
void ble_module_enable(u8 en)
{
    ble_comm_module_enable(en);
}

static void ble_sys_event(struct sys_event *e)
{
    if ((u32)e->arg != SYS_BT_EVENT_TYPE_CON_STATUS || e->u.bt.event != BT_STATUS_INIT_OK) {
        return;
    }
    LOG_INFO(ble, "stack up: advertising as \"%s\"; pairing %s", BLE_NAME,
             ble_sm_cfg.authentication_req_flags & SM_AUTHREQ_BONDING ? "with bonding" : "without bonding");
    ble_comm_set_config_name(BLE_NAME, 0);
    ble_gatt_server_set_profile(ble_profile, sizeof ble_profile);
    ble_adv_setup();
    ble_applied = ble_want;
    ble_module_enable(ble_applied);
    ble_up = 1;
    if (!ble_applied)
        LOG_INFO(ble, "bluetooth off: no advertising");
}
SYS_EVENT_HANDLER(SYS_BT_EVENT, ble_sys_event, 0);

void ble_br25_start(void)
{
    uint8_t addr[6];
    blemidi_rx_init(&rx);
    blemidi_tx_init(&tx);
    lat_hist_clear_wide(&win.lat);
    os_sem_create(&ble_sem, 0);
    int e = task_create(ble_task, NULL, APP_TASK_BLE);
    if (e)
        LOG_ERROR(ble, "BLE task not created: error %d", e);
    /* Our BLE address: the SDK's rule over the Bluetooth MAC (BTIF), as M-Vave's firmware makes
     * its own, then the low bits of one byte flipped, so ours never takes M-Vave's address and a
     * phone or a computer keeps one entry and one pairing for each firmware (the owner,
     * 2026-11-14; docs/ble.md, "Our own address"). Stable per mixer: the same MAC, the same
     * address. The vendor's adapter takes any address (✅ R-11, BLE_ADDR_XOR=5, controller 26). */
    lib_make_ble_address(addr, (void *)bt_get_mac_addr());
    addr[0] ^= (uint8_t)(BLE_ADDR_XOR & 0x0F); /* the low bits only: the address type stays */
    LOG_INFO(ble, "BLE address: M-Vave's rule, then xor %02x", BLE_ADDR_XOR & 0x0F);
    le_controller_set_mac(addr);
    bt_pll_para(TCFG_CLOCK_OSC_HZ, clk_get("sys"), 0, 0);
    ble_comm_init(&ble_ctrl);
    int r = btstack_init();
    LOG_INFO(ble, "btstack_init: %d", r);
}
