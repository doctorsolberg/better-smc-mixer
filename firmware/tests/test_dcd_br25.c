/*
 * Host tests of the BR25 USB controller driver (firmware/platform/br25/usb/dcd_br25.c) against a
 * model of the controller. The model does what docs/usb.md says the hardware does: MUSB registers
 * reached one byte at a time through CON1, interrupt registers that clear on read, DMA from the
 * addresses in EP0_ADR / EPn_TADR / EPn_RADR (a packet that lands without the address having been
 * rewritten since the last one is an error: the real pointer has moved on), and an RX count that
 * can read 0 for a while in a run of full packets. The test plays the host and the stack: it
 * sends SETUP/OUT packets, takes IN packets, and answers the driver's events as usbd would.
 *
 * What a model cannot show: the real register semantics, timing, and the controller's quirks
 * beyond those listed. That needs the device (docs/usb.md, "Not checked without hardware").
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "check.h"
#include "smclog.h"
#include "smclog_host.h"
#include "tusb.h"
#include "device/dcd.h"

#include "br25_usb_regs.h"
#include "dcd_br25.h"

int check_failed;

/* --- the model ------------------------------------------------------------------------------- */

struct rxq {
    uint8_t data[64];
    uint16_t len;
};

static struct {
    uint32_t con0, con1, io, ep_cnt[BR25_USB_EPS];
    uint8_t common[0x10];
    uint8_t maxp[BR25_USB_EPS][2];
    bool no_ack;
    unsigned con1_ops;
    /* DMA: the address as last written, and whether it was written since the last packet */
    uint8_t *ep0_adr, *tadr[BR25_USB_EPS], *radr[BR25_USB_EPS];
    bool ep0_fresh, tadr_fresh[BR25_USB_EPS], radr_fresh[BR25_USB_EPS];
    /* endpoint 0 */
    bool rx_rdy0, tx_rdy0, sent_stall0, setup_end0, data_end, stall0;
    uint16_t count0;
    /* other endpoints */
    uint8_t txcsr[BR25_USB_EPS], rxcsr[BR25_USB_EPS];
    uint16_t rxcount[BR25_USB_EPS];
    unsigned zero_reads[BR25_USB_EPS]; /* RXCOUNT1 reads that return 0 first */
    struct rxq queued[BR25_USB_EPS];   /* the second packet of the double-buffered FIFO */
    bool has_queued[BR25_USB_EPS];
    unsigned toggles_cleared[BR25_USB_EPS][2];
    unsigned irq_sets, irq_on;
} m;

static uint8_t idx(void) { return m.common[BR25_MUSB_INDEX]; }

static uint8_t csr0_value(void)
{
    return (uint8_t)((m.rx_rdy0 ? BR25_CSR0_RXPKTRDY : 0) | (m.tx_rdy0 ? BR25_CSR0_TXPKTRDY : 0) |
                     (m.sent_stall0 ? BR25_CSR0_SENTSTALL : 0) |
                     (m.setup_end0 ? BR25_CSR0_SETUPEND : 0) | (m.stall0 ? BR25_CSR0_SENDSTALL : 0));
}

static uint32_t musb_read(uint8_t reg)
{
    uint8_t n = idx();
    uint8_t v;
    switch (reg) {
    case BR25_MUSB_INTRUSB:
    case BR25_MUSB_INTRTX1:
    case BR25_MUSB_INTRRX1:
        v = m.common[reg];
        m.common[reg] = 0; /* clear on read */
        return v;
    case BR25_MUSB_CSR0: /* = TXCSR1 */
        return n == 0 ? csr0_value() : m.txcsr[n];
    case BR25_MUSB_COUNT0: /* = RXCOUNT1 */
        if (n == 0)
            return m.count0;
        if (m.zero_reads[n]) {
            m.zero_reads[n]--;
            return 0;
        }
        return m.rxcount[n] & 0xFF;
    case BR25_MUSB_RXCOUNT2:
        return n == 0 ? 0 : (m.zero_reads[n] ? 0 : m.rxcount[n] >> 8);
    case BR25_MUSB_RXCSR1:
        return m.rxcsr[n];
    default:
        return reg < 0x10 ? m.common[reg] : 0;
    }
}

static void rx_load(uint8_t n, const uint8_t *data, uint16_t len, unsigned zero_reads)
{
    CHECK(m.radr_fresh[n]); /* the driver rewound RADR after the previous packet */
    if (m.radr_fresh[n] && m.radr[n])
        memcpy(m.radr[n], data, len);
    m.radr_fresh[n] = false;
    m.rxcount[n] = len;
    m.zero_reads[n] = zero_reads;
    m.rxcsr[n] |= BR25_RXCSR_RXPKTRDY;
    if (len == 64 || zero_reads)
        m.rxcsr[n] |= BR25_RXCSR_FIFOFULL;
    else
        m.rxcsr[n] &= (uint8_t)~BR25_RXCSR_FIFOFULL;
    m.common[BR25_MUSB_INTRRX1] |= (uint8_t)(1u << n);
}

static void musb_write(uint8_t reg, uint8_t v)
{
    uint8_t n = idx();
    if (reg < 0x10) {
        m.common[reg] = v;
        return;
    }
    switch (reg) {
    case BR25_MUSB_TXMAXP:
        m.maxp[n][1] = v;
        break;
    case BR25_MUSB_RXMAXP:
        m.maxp[n][0] = v;
        break;
    case BR25_MUSB_CSR0: /* = TXCSR1 */
        if (n == 0) {
            if (v & BR25_CSR0_SVDRXPKTRDY)
                m.rx_rdy0 = false;
            if (v & BR25_CSR0_SVDSETUPEND)
                m.setup_end0 = false;
            if (!(v & BR25_CSR0_SENTSTALL))
                m.sent_stall0 = false;
            if (v & BR25_CSR0_SENDSTALL)
                m.stall0 = true;
            if (v & BR25_CSR0_DATAEND)
                m.data_end = true;
            if (v & BR25_CSR0_TXPKTRDY) {
                CHECK(!m.rx_rdy0); /* a SETUP must be serviced before data goes out */
                CHECK(m.ep0_fresh);
                m.tx_rdy0 = true;
            }
        } else {
            if (v & BR25_TXCSR_TXPKTRDY) {
                CHECK(m.tadr_fresh[n]);
                CHECK(!(m.txcsr[n] & BR25_TXCSR_TXPKTRDY)); /* one packet in flight */
            }
            if (v & BR25_TXCSR_CLRDATATOG)
                m.toggles_cleared[n][1]++;
            m.txcsr[n] = (uint8_t)((m.txcsr[n] & BR25_TXCSR_TXPKTRDY) |
                                   (v & (BR25_TXCSR_TXPKTRDY | BR25_TXCSR_SENDSTALL)) |
                                   (m.txcsr[n] & v & (BR25_TXCSR_SENTSTALL | BR25_TXCSR_UNDERRUN)));
            if (v & BR25_TXCSR_FLUSHFIFO)
                m.txcsr[n] &= (uint8_t)~BR25_TXCSR_TXPKTRDY;
        }
        break;
    case BR25_MUSB_TXCSR2:
        break;
    case BR25_MUSB_RXCSR1:
        if (v & BR25_RXCSR_CLRDATATOG)
            m.toggles_cleared[n][0]++;
        m.rxcsr[n] = (uint8_t)((m.rxcsr[n] & (BR25_RXCSR_RXPKTRDY | BR25_RXCSR_FIFOFULL)) |
                               (v & BR25_RXCSR_SENDSTALL) | (m.rxcsr[n] & v & BR25_RXCSR_SENTSTALL));
        if ((v & BR25_RXCSR_FLUSHFIFO) && !(v & BR25_RXCSR_RXPKTRDY)) {
            m.rxcsr[n] &= (uint8_t)~(BR25_RXCSR_RXPKTRDY | BR25_RXCSR_FIFOFULL);
            m.rxcount[n] = 0;
            if (m.has_queued[n]) { /* the second packet moves up */
                m.has_queued[n] = false;
                rx_load(n, m.queued[n].data, m.queued[n].len, 0);
            }
        }
        break;
    default:
        break;
    }
}

uint32_t br25_model_rd(uint32_t reg)
{
    if (reg == BR25_USB_CON0)
        return m.con0;
    if (reg == BR25_USB_CON1)
        return m.con1;
    if (reg == BR25_USB_IO_CON0)
        return m.io;
    CHECK(0); /* nothing else is readable */
    return 0;
}

void br25_model_wr(uint32_t reg, uint32_t v)
{
    if (reg == BR25_USB_CON0) {
        m.con0 = v;
    } else if (reg == BR25_USB_IO_CON0) {
        m.io = v;
    } else if (reg == BR25_USB_CON1) {
        m.con1_ops++;
        if (!(m.con0 & BR25_CON0_NRST) && v) {
            CHECK(0); /* the core is in reset: CON1 does nothing */
            return;
        }
        uint8_t r = (uint8_t)((v >> 8) & 0x3F);
        if (v & BR25_CON1_RNW)
            m.con1 = musb_read(r);
        else {
            if (v)
                musb_write(r, (uint8_t)v);
            m.con1 = v & 0xFF;
        }
        if (!m.no_ack && v)
            m.con1 |= BR25_CON1_ACK;
    } else {
        for (unsigned n = 0; n < BR25_USB_EPS; n++)
            if (reg == BR25_USB_EP_CNT(n)) {
                m.ep_cnt[n] = v;
                return;
            }
        CHECK(0);
    }
}

void br25_model_dma(uint32_t reg, void *buf)
{
    if (reg == BR25_USB_EP0_ADR) {
        m.ep0_adr = buf;
        m.ep0_fresh = true;
        return;
    }
    for (unsigned n = 1; n < BR25_USB_EPS; n++) {
        if (reg == BR25_USB_EP_TADR(n)) {
            m.tadr[n] = buf;
            m.tadr_fresh[n] = true;
            return;
        }
        if (reg == BR25_USB_EP_RADR(n)) {
            m.radr[n] = buf;
            m.radr_fresh[n] = true;
            return;
        }
    }
    CHECK(0);
}

void br25_usb_irq_set(bool enabled)
{
    m.irq_sets++;
    m.irq_on = enabled;
}

/* --- the stack's side: events ---------------------------------------------------------------- */

#define MAX_EVENTS 32
static dcd_event_t events[MAX_EVENTS];
static unsigned n_events;

void dcd_event_handler(dcd_event_t const *event, bool in_isr)
{
    (void)in_isr;
    if (n_events < MAX_EVENTS)
        events[n_events] = *event;
    n_events++;
}

static dcd_event_t *take(uint8_t id)
{
    static dcd_event_t none;
    for (unsigned i = 0; i < n_events && i < MAX_EVENTS; i++)
        if (events[i].event_id == id) {
            static dcd_event_t e;
            e = events[i];
            memmove(&events[i], &events[i + 1], (n_events - i - 1) * sizeof events[0]);
            n_events--;
            return &e;
        }
    fprintf(stderr, "  no event %u among %u\n", id, n_events);
    check_failed++;
    memset(&none, 0, sizeof none);
    none.event_id = 0xFF;
    return &none;
}

static void expect_xfer(uint8_t ep, uint32_t len)
{
    dcd_event_t *e = take(DCD_EVENT_XFER_COMPLETE);
    CHECK_EQ(e->xfer_complete.ep_addr, ep);
    CHECK_EQ(e->xfer_complete.len, len);
}

/* --- the host's side ------------------------------------------------------------------------- */

static void irq(void)
{
    dcd_int_handler(0);
}

static void host_reset(void)
{
    m.common[BR25_MUSB_INTRUSB] |= BR25_INTR_RESET;
    m.common[BR25_MUSB_FADDR] = 0;
    irq();
}

static void host_setup(uint8_t type, uint8_t req, uint16_t value, uint16_t index, uint16_t len)
{
    uint8_t s[8] = {type, req, (uint8_t)value, (uint8_t)(value >> 8), (uint8_t)index,
                    (uint8_t)(index >> 8), (uint8_t)len, (uint8_t)(len >> 8)};
    CHECK(!m.rx_rdy0);
    CHECK(m.ep0_fresh);
    memcpy(m.ep0_adr, s, 8);
    m.ep0_fresh = false;
    m.count0 = 8;
    m.rx_rdy0 = true;
    m.stall0 = false;
    m.common[BR25_MUSB_INTRTX1] |= 1;
    irq();
}

/* The host takes the IN packet on endpoint 0; merge: the status OUT completes in the same
 * interrupt (no separate one). */
static uint16_t host_in0(uint8_t *out, bool merge)
{
    CHECK(m.tx_rdy0);
    uint16_t n = (uint16_t)m.ep_cnt[0];
    memcpy(out, m.ep0_adr, n);
    m.ep0_fresh = false;
    m.tx_rdy0 = false;
    if (m.data_end && merge)
        m.data_end = false;
    m.common[BR25_MUSB_INTRTX1] |= 1;
    irq();
    return n;
}

static bool host_out0(const uint8_t *data, uint16_t n)
{
    if (m.rx_rdy0)
        return false; /* NAKed */
    CHECK(m.ep0_fresh);
    memcpy(m.ep0_adr, data, n);
    m.ep0_fresh = false;
    m.count0 = n;
    m.rx_rdy0 = true;
    m.common[BR25_MUSB_INTRTX1] |= 1;
    irq();
    return true;
}

/* The status stage the controller runs after DataEnd. */
static void host_status(void)
{
    CHECK(m.data_end);
    m.data_end = false;
    m.common[BR25_MUSB_INTRTX1] |= 1;
    irq();
}

static uint16_t host_in(uint8_t n, uint8_t *out)
{
    CHECK(m.txcsr[n] & BR25_TXCSR_TXPKTRDY);
    uint16_t k = (uint16_t)m.ep_cnt[n];
    memcpy(out, m.tadr[n], k);
    m.tadr_fresh[n] = false;
    m.txcsr[n] &= (uint8_t)~BR25_TXCSR_TXPKTRDY;
    m.common[BR25_MUSB_INTRTX1] |= (uint8_t)(1u << n);
    irq();
    return k;
}

static void host_out(uint8_t n, const uint8_t *data, uint16_t len, unsigned zero_reads)
{
    if (m.rxcsr[n] & BR25_RXCSR_RXPKTRDY) {
        CHECK(!m.has_queued[n]);
        memcpy(m.queued[n].data, data, len);
        m.queued[n].len = len;
        m.has_queued[n] = true;
        return;
    }
    rx_load(n, data, len, zero_reads);
    irq();
}

/* --- tests ----------------------------------------------------------------------------------- */

static uint32_t clock_us;
static uint32_t now(void) { return clock_us++; }

static void setup_test(void)
{
    memset(&m, 0, sizeof m);
    n_events = 0;
    const tusb_rhport_init_t init = {.role = TUSB_ROLE_DEVICE, .speed = TUSB_SPEED_FULL};
    CHECK(dcd_init(0, &init));
    dcd_int_enable(0);
}

static void enumerate_to_address(void)
{
    host_reset();
    CHECK_EQ(take(DCD_EVENT_BUS_RESET)->bus_reset.speed, TUSB_SPEED_FULL);
}

static void test_init(void)
{
    setup_test();
    CHECK(m.con0 & BR25_CON0_PHY_ON);
    CHECK((m.con0 & (BR25_CON0_NRST | BR25_CON0_TM1 | BR25_CON0_CID | BR25_CON0_VBUS)) ==
          (BR25_CON0_NRST | BR25_CON0_TM1 | BR25_CON0_CID | BR25_CON0_VBUS));
    CHECK(m.con0 & BR25_CON0_SIEIE);
    CHECK(m.io & BR25_IO_DPDIE);
    CHECK(m.io & BR25_IO_DMDIE);
    CHECK(m.io & BR25_IO_SR);
    CHECK(!(m.io & BR25_IO_IO_MODE));
    CHECK(m.io & BR25_IO_DPPU);
    CHECK(!(m.io & (BR25_IO_DMPU | BR25_IO_DPPD | BR25_IO_DMPD)));
    CHECK_EQ(m.common[BR25_MUSB_POWER], BR25_POWER_SOFTCONN);
    CHECK_EQ(m.common[BR25_MUSB_INTRUSBE], BR25_INTR_RESET | BR25_INTR_SUSPEND | BR25_INTR_RESUME);
    CHECK_EQ(m.common[BR25_MUSB_INTRTX1E], 1);
    CHECK(m.ep0_adr != NULL);
    CHECK(m.irq_on);
    dcd_disconnect(0);
    CHECK(!(m.io & BR25_IO_DPPU));
    CHECK_EQ(m.common[BR25_MUSB_POWER], 0);
}

static void test_get_descriptor(void)
{
    setup_test();
    enumerate_to_address();
    /* GET_DESCRIPTOR(device), wLength 64, 18 bytes answered */
    host_setup(0x80, 6, 0x0100, 0, 64);
    dcd_event_t *e = take(DCD_EVENT_SETUP_RECEIVED);
    CHECK_EQ(e->setup_received.bRequest, 6);
    CHECK_EQ(e->setup_received.wLength, 64);
    CHECK(m.rx_rdy0); /* not serviced until the stack answers */
    uint8_t desc[18];
    for (unsigned i = 0; i < sizeof desc; i++)
        desc[i] = (uint8_t)(i + 1);
    CHECK(dcd_edpt_xfer(0, 0x80, desc, 18, false));
    CHECK(!m.rx_rdy0);
    CHECK(m.data_end); /* a short packet ends the data stage */
    uint8_t got[64];
    CHECK_EQ(host_in0(got, true), 18);
    CHECK(memcmp(got, desc, 18) == 0);
    expect_xfer(0x80, 18);
    CHECK(dcd_edpt_xfer(0, 0x00, NULL, 0, false)); /* the stack's status OUT */
    expect_xfer(0x00, 0);
    CHECK_EQ(n_events, 0);
}

static void test_long_in_and_zlp(void)
{
    setup_test();
    enumerate_to_address();
    uint8_t data[100], got[64];
    for (unsigned i = 0; i < sizeof data; i++)
        data[i] = (uint8_t)(0xA0 + i);
    /* 100 bytes for wLength 255: the stack sends 64 then 36 */
    host_setup(0x80, 6, 0x0200, 0, 255);
    take(DCD_EVENT_SETUP_RECEIVED);
    dcd_edpt_xfer(0, 0x80, data, 64, false);
    CHECK(!m.data_end);
    CHECK_EQ(host_in0(got, false), 64);
    expect_xfer(0x80, 64);
    dcd_edpt_xfer(0, 0x80, data + 64, 36, false);
    CHECK(m.data_end);
    CHECK_EQ(host_in0(got, false), 36);
    CHECK(memcmp(got, data + 64, 36) == 0);
    expect_xfer(0x80, 36);
    /* The status interrupt comes on its own here; the stack arms it after or before. */
    host_status();
    CHECK_EQ(n_events, 0);
    dcd_edpt_xfer(0, 0x00, NULL, 0, false);
    expect_xfer(0x00, 0);

    /* Exactly 64 bytes for wLength 255: a zero-length packet ends the data stage. */
    host_setup(0x80, 6, 0x0300, 0, 255);
    take(DCD_EVENT_SETUP_RECEIVED);
    dcd_edpt_xfer(0, 0x80, data, 64, false);
    CHECK(!m.data_end);
    host_in0(got, false);
    expect_xfer(0x80, 64);
    dcd_edpt_xfer(0, 0x80, NULL, 0, false);
    CHECK(m.data_end);
    CHECK_EQ(host_in0(got, true), 0);
    expect_xfer(0x80, 0);
    dcd_edpt_xfer(0, 0x00, NULL, 0, false);
    expect_xfer(0x00, 0);

    /* Exactly wLength (64 of 64): DataEnd with the full packet, no ZLP. */
    host_setup(0x80, 6, 0x0300, 0, 64);
    take(DCD_EVENT_SETUP_RECEIVED);
    dcd_edpt_xfer(0, 0x80, data, 64, false);
    CHECK(m.data_end);
    host_in0(got, true);
    expect_xfer(0x80, 64);
    dcd_edpt_xfer(0, 0x00, NULL, 0, false);
    expect_xfer(0x00, 0);
    CHECK_EQ(n_events, 0);
}

static void test_set_address(void)
{
    setup_test();
    enumerate_to_address();
    host_setup(0x00, 5, 7, 0, 0);
    take(DCD_EVENT_SETUP_RECEIVED);
    dcd_set_address(0, 7);
    CHECK(!m.rx_rdy0);
    CHECK(m.data_end);
    CHECK_EQ(m.common[BR25_MUSB_FADDR], 0); /* the status IN still goes to address 0 */
    host_status();
    CHECK_EQ(m.common[BR25_MUSB_FADDR], 7);
    CHECK_EQ(n_events, 0); /* the driver answers SET_ADDRESS itself: no event */

    /* SET_CONFIGURATION: no data, the stack arms the status IN */
    host_setup(0x00, 9, 1, 0, 0);
    take(DCD_EVENT_SETUP_RECEIVED);
    CHECK(m.rx_rdy0);
    dcd_edpt_xfer(0, 0x80, NULL, 0, false);
    CHECK(!m.rx_rdy0);
    CHECK(m.data_end);
    CHECK_EQ(n_events, 0);
    host_status();
    expect_xfer(0x80, 0);
}

static void test_out_data(void)
{
    setup_test();
    enumerate_to_address();
    uint8_t line[7] = {0x00, 0xC2, 0x01, 0x00, 0x00, 0x00, 0x08}, buf[64];
    /* SET_LINE_CODING-like: 7 bytes OUT */
    host_setup(0x21, 0x20, 0, 0, 7);
    take(DCD_EVENT_SETUP_RECEIVED);
    CHECK(!host_out0(line, 7)); /* NAKed until the stack takes the SETUP */
    dcd_edpt_xfer(0, 0x00, buf, 7, false);
    CHECK(host_out0(line, 7));
    expect_xfer(0x00, 7);
    CHECK(memcmp(buf, line, 7) == 0);
    CHECK(m.data_end);
    host_status(); /* before the stack arms the status IN */
    CHECK_EQ(n_events, 0);
    dcd_edpt_xfer(0, 0x80, NULL, 0, false);
    expect_xfer(0x80, 0);

    /* 100 bytes OUT in two parts; the second packet arrives before the stack asks for it */
    uint8_t big[100];
    for (unsigned i = 0; i < sizeof big; i++)
        big[i] = (uint8_t)(3 * i);
    host_setup(0x21, 0x01, 0, 0, 100);
    take(DCD_EVENT_SETUP_RECEIVED);
    dcd_edpt_xfer(0, 0x00, buf, 64, false);
    CHECK(host_out0(big, 64));
    expect_xfer(0x00, 64);
    CHECK(!m.data_end);
    CHECK(memcmp(buf, big, 64) == 0);
    CHECK(host_out0(big + 64, 36)); /* waits in the FIFO */
    CHECK_EQ(n_events, 0);
    dcd_edpt_xfer(0, 0x00, buf, 36, false);
    expect_xfer(0x00, 36);
    CHECK(memcmp(buf, big + 64, 36) == 0);
    CHECK(m.data_end);
    dcd_edpt_xfer(0, 0x80, NULL, 0, false);
    CHECK_EQ(n_events, 0);
    host_status();
    expect_xfer(0x80, 0);
}

static void test_stall_and_abort(void)
{
    setup_test();
    enumerate_to_address();
    host_setup(0xC0, 0x55, 0, 0, 4); /* unknown vendor request */
    take(DCD_EVENT_SETUP_RECEIVED);
    dcd_edpt_stall(0, 0x00);
    dcd_edpt_stall(0, 0x80);
    CHECK(m.stall0);
    CHECK(!m.rx_rdy0);
    m.sent_stall0 = true;
    m.common[BR25_MUSB_INTRTX1] |= 1;
    irq();
    CHECK(!m.sent_stall0);
    CHECK_EQ(br25_usb_stats()->stalls_sent, 1);

    /* The host gives up on a control read halfway: SetupEnd, then a new SETUP. */
    uint8_t data[64] = {0}, got[64];
    host_setup(0x80, 6, 0x0200, 0, 128);
    take(DCD_EVENT_SETUP_RECEIVED);
    dcd_edpt_xfer(0, 0x80, data, 64, false);
    host_in0(got, false);
    expect_xfer(0x80, 64);
    m.setup_end0 = true;
    m.common[BR25_MUSB_INTRTX1] |= 1;
    irq();
    CHECK(!m.setup_end0);
    dcd_edpt_xfer(0, 0x80, data, 64, false); /* the stack's next part: dropped */
    CHECK(!m.tx_rdy0);
    host_setup(0x80, 0, 0, 0, 2); /* GET_STATUS works */
    take(DCD_EVENT_SETUP_RECEIVED);
    dcd_edpt_xfer(0, 0x80, data, 2, false);
    CHECK_EQ(host_in0(got, true), 2);
    expect_xfer(0x80, 2);
    CHECK_EQ(n_events, 0);
}

/* The host's next SETUP can come before the stack has armed the last transfer's status stage
 * (an IN data stage ends on the bus with DataEnd; the stack arms the status OUT from its task).
 * The SETUP must wait, or the late status would be taken as the new request's. */
static void test_setup_before_status_armed(void)
{
    setup_test();
    enumerate_to_address();
    uint8_t data[18] = {18, 1}, got[64];
    host_setup(0x80, 6, 0x0100, 0, 18);
    take(DCD_EVENT_SETUP_RECEIVED);
    dcd_edpt_xfer(0, 0x80, data, 18, false);
    host_in0(got, true);
    expect_xfer(0x80, 18);
    host_setup(0x80, 6, 0x0200, 0, 9); /* before the stack arms the status OUT */
    CHECK_EQ(n_events, 0);
    CHECK(m.rx_rdy0); /* still in the FIFO */
    dcd_edpt_xfer(0, 0x00, NULL, 0, false); /* the old status: completes, then the SETUP comes */
    CHECK_EQ(n_events, 2);
    CHECK_EQ(events[0].event_id, DCD_EVENT_XFER_COMPLETE);
    CHECK_EQ(events[0].xfer_complete.ep_addr, 0x00);
    CHECK_EQ(events[1].event_id, DCD_EVENT_SETUP_RECEIVED);
    CHECK_EQ(events[1].setup_received.wValue, 0x0200);
    n_events = 0;
    dcd_edpt_xfer(0, 0x80, data, 9, false); /* the new request's data goes out */
    CHECK_EQ(host_in0(got, true), 9);
    expect_xfer(0x80, 9);

    /* The stack refuses the old transfer after its data (stall pair): the stall is late for the
     * bus, so it is dropped, and the parked SETUP goes on. */
    host_setup(0x80, 6, 0x0300, 0, 4);
    CHECK_EQ(n_events, 0); /* the 9-byte transfer's status is not armed: parked */
    dcd_edpt_stall(0, 0x00);
    dcd_edpt_stall(0, 0x80);
    CHECK(!m.stall0);
    CHECK_EQ(n_events, 1);
    CHECK_EQ(events[0].event_id, DCD_EVENT_SETUP_RECEIVED);
    CHECK_EQ(events[0].setup_received.wValue, 0x0300);
    n_events = 0;
    dcd_edpt_stall(0, 0x00); /* a real stall of the new request still works */
    dcd_edpt_stall(0, 0x80);
    CHECK(m.stall0);
}

static const tusb_desc_endpoint_t bulk_out = {
    .bLength = 7, .bDescriptorType = TUSB_DESC_ENDPOINT, .bEndpointAddress = 0x02,
    .bmAttributes = {.xfer = TUSB_XFER_BULK}, .wMaxPacketSize = 64};
static const tusb_desc_endpoint_t bulk_in = {
    .bLength = 7, .bDescriptorType = TUSB_DESC_ENDPOINT, .bEndpointAddress = 0x83,
    .bmAttributes = {.xfer = TUSB_XFER_BULK}, .wMaxPacketSize = 64};

static void test_bulk_out(void)
{
    setup_test();
    enumerate_to_address();
    CHECK(dcd_edpt_open(0, &bulk_out));
    CHECK_EQ(m.maxp[2][0], 0xFF); /* MaxP as 0xFF */
    CHECK_EQ(m.common[BR25_MUSB_INTRRX1E], 1u << 2);
    uint8_t buf[128], pkt[64];
    for (unsigned i = 0; i < 64; i++)
        pkt[i] = (uint8_t)i;
    dcd_edpt_xfer(0, 0x02, buf, 128, false);
    host_out(2, pkt, 64, 0);
    CHECK_EQ(n_events, 0);
    pkt[0] = 0xEE;
    host_out(2, pkt, 64, 5); /* count reads 0 five times: the driver waits */
    expect_xfer(0x02, 128);
    CHECK_EQ(buf[0], 0);
    CHECK_EQ(buf[64], 0xEE);
    CHECK_EQ(br25_usb_stats()->rx_waits, 1);
    CHECK_EQ(br25_usb_stats()->rx_wait_max, 6);

    /* short packet ends the transfer; a zero-length packet too */
    dcd_edpt_xfer(0, 0x02, buf, 128, false);
    host_out(2, pkt, 10, 0);
    expect_xfer(0x02, 10);
    dcd_edpt_xfer(0, 0x02, buf, 128, false);
    host_out(2, pkt, 0, 0);
    expect_xfer(0x02, 0);

    /* packets before the transfer is armed wait (NAKed), two fit in the FIFO */
    host_out(2, pkt, 64, 0);
    pkt[0] = 0x11;
    host_out(2, pkt, 20, 0);
    CHECK_EQ(n_events, 0);
    dcd_edpt_xfer(0, 0x02, buf, 128, false);
    expect_xfer(0x02, 84);
    CHECK_EQ(buf[64], 0x11);

    /* a count that stays 0 is given up on and taken as zero-length */
    dcd_edpt_xfer(0, 0x02, buf, 128, false);
    host_out(2, pkt, 64, 5000); /* more reads than RX_WAIT */
    expect_xfer(0x02, 0);
    CHECK_EQ(br25_usb_stats()->rx_wait_fails, 1);
}

static void test_bulk_in(void)
{
    setup_test();
    enumerate_to_address();
    CHECK(dcd_edpt_open(0, &bulk_in));
    CHECK_EQ(m.maxp[3][1], 0xFF);
    CHECK_EQ(m.common[BR25_MUSB_INTRTX1E], 1u | 1u << 3);
    uint8_t data[150], got[64];
    for (unsigned i = 0; i < sizeof data; i++)
        data[i] = (uint8_t)(7 * i);
    dcd_edpt_xfer(0, 0x83, data, 150, false);
    CHECK_EQ(host_in(3, got), 64);
    CHECK(memcmp(got, data, 64) == 0);
    CHECK_EQ(host_in(3, got), 64);
    CHECK(memcmp(got, data + 64, 64) == 0);
    CHECK_EQ(n_events, 0);
    CHECK_EQ(host_in(3, got), 22);
    CHECK(memcmp(got, data + 128, 22) == 0);
    expect_xfer(0x83, 150);
    dcd_edpt_xfer(0, 0x83, NULL, 0, false);
    CHECK_EQ(host_in(3, got), 0);
    expect_xfer(0x83, 0);

    dcd_edpt_stall(0, 0x83);
    CHECK(m.txcsr[3] & BR25_TXCSR_SENDSTALL);
    dcd_edpt_clear_stall(0, 0x83);
    CHECK(!(m.txcsr[3] & BR25_TXCSR_SENDSTALL));
    CHECK(m.toggles_cleared[3][1] >= 2); /* at open and at clear */

    tusb_desc_endpoint_t iso = bulk_in;
    iso.bmAttributes.xfer = TUSB_XFER_ISOCHRONOUS;
    CHECK(!dcd_edpt_open(0, &iso));
    tusb_desc_endpoint_t big = bulk_in;
    big.wMaxPacketSize = 512;
    CHECK(!dcd_edpt_open(0, &big));
    tusb_desc_endpoint_t five = bulk_in;
    five.bEndpointAddress = 0x85;
    CHECK(!dcd_edpt_open(0, &five));

    dcd_edpt_close_all(0);
    CHECK_EQ(m.common[BR25_MUSB_INTRTX1E], 1);
    CHECK(!dcd_edpt_xfer(0, 0x83, data, 10, false));
}

/* USB MIDI in the application: OUT 0x04 and IN 0x84 on the same endpoint number, each direction
 * with its own buffer, both busy at once (docs/usb.md). */
static void test_shared_endpoint(void)
{
    static const tusb_desc_endpoint_t midi_out = {
        .bLength = 7, .bDescriptorType = TUSB_DESC_ENDPOINT, .bEndpointAddress = 0x04,
        .bmAttributes = {.xfer = TUSB_XFER_BULK}, .wMaxPacketSize = 64};
    static const tusb_desc_endpoint_t midi_in = {
        .bLength = 7, .bDescriptorType = TUSB_DESC_ENDPOINT, .bEndpointAddress = 0x84,
        .bmAttributes = {.xfer = TUSB_XFER_BULK}, .wMaxPacketSize = 64};
    setup_test();
    enumerate_to_address();
    CHECK(dcd_edpt_open(0, &midi_out));
    CHECK(dcd_edpt_open(0, &midi_in));
    CHECK_EQ(m.maxp[4][0], 0xFF);
    CHECK_EQ(m.maxp[4][1], 0xFF);
    CHECK_EQ(m.common[BR25_MUSB_INTRRX1E], 1u << 4);
    CHECK_EQ(m.common[BR25_MUSB_INTRTX1E], 1u | 1u << 4);

    uint8_t rx[64], got[64];
    const uint8_t note_on[8] = {0x09, 0x90, 0x48, 0x7F, 0x0B, 0xB0, 0x14, 0x64};
    const uint8_t from_host[4] = {0x09, 0x90, 0x3C, 0x40};
    memset(rx, 0, sizeof rx);
    CHECK(dcd_edpt_xfer(0, 0x04, rx, 64, false)); /* the host may send */
    CHECK(dcd_edpt_xfer(0, 0x84, (uint8_t *)note_on, 8, false));
    CHECK(m.tadr[4] != m.radr[4]);

    host_out(4, from_host, 4, 0); /* OUT arrives while the IN packet waits */
    expect_xfer(0x04, 4);
    CHECK(memcmp(rx, from_host, 4) == 0);
    CHECK(m.txcsr[4] & BR25_TXCSR_TXPKTRDY); /* the IN packet is still there */
    CHECK_EQ(n_events, 0);

    CHECK_EQ(host_in(4, got), 8);
    CHECK(memcmp(got, note_on, 8) == 0);
    expect_xfer(0x84, 8);
    CHECK_EQ(n_events, 0);

    /* the other way round: IN first, then OUT, then the IN completes */
    CHECK(dcd_edpt_xfer(0, 0x04, rx, 64, false));
    CHECK(dcd_edpt_xfer(0, 0x84, (uint8_t *)note_on, 4, false));
    CHECK_EQ(host_in(4, got), 4);
    expect_xfer(0x84, 4);
    host_out(4, note_on + 4, 4, 0);
    expect_xfer(0x04, 4);
    CHECK(memcmp(rx, note_on + 4, 4) == 0);

    /* a stall in one direction leaves the other alone */
    dcd_edpt_stall(0, 0x84);
    CHECK(m.txcsr[4] & BR25_TXCSR_SENDSTALL);
    CHECK(!(m.rxcsr[4] & BR25_RXCSR_SENDSTALL));
    dcd_edpt_clear_stall(0, 0x84);
    CHECK(dcd_edpt_xfer(0, 0x04, rx, 64, false));
    host_out(4, from_host, 4, 0);
    expect_xfer(0x04, 4);
}

static void test_bus_events_and_locking(void)
{
    setup_test();
    enumerate_to_address();
    CHECK(dcd_edpt_open(0, &bulk_in));
    unsigned before = m.irq_sets;
    uint8_t data[4] = {1, 2, 3, 4}, got[64];
    dcd_edpt_xfer(0, 0x83, data, 4, false);
    CHECK_EQ(m.irq_sets, before + 2); /* masked and unmasked around the task-side access */
    CHECK(m.irq_on);
    m.common[BR25_MUSB_INTRUSB] = BR25_INTR_SUSPEND;
    irq();
    take(DCD_EVENT_SUSPEND);
    m.common[BR25_MUSB_INTRUSB] = BR25_INTR_RESUME;
    irq();
    take(DCD_EVENT_RESUME);
    /* a bus reset drops the endpoints and the transfer in flight */
    host_reset();
    take(DCD_EVENT_BUS_RESET);
    CHECK_EQ(m.common[BR25_MUSB_INTRTX1E], 1);
    host_in(3, got);
    CHECK_EQ(n_events, 0);
    dcd_int_disable(0);
    before = m.irq_sets;
    dcd_sof_enable(0, true);
    CHECK_EQ(m.irq_sets, before); /* already masked: left alone */
    CHECK(m.common[BR25_MUSB_INTRUSBE] & BR25_INTR_SOF);
}

static void test_register_timeout(void)
{
    setup_test();
    m.no_ack = true;
    uint32_t before = br25_usb_stats()->reg_timeouts;
    irq();
    CHECK(br25_usb_stats()->reg_timeouts > before);
    m.no_ack = false;
}

int main(void)
{
    smclog_host_set_clock(now);
    smclog_reset();
    smclog_init();
    smclog_set_level(SMCLOG_MOD_usb, SMCLOG_TRACE);
    RUN(test_init);
    RUN(test_get_descriptor);
    RUN(test_long_in_and_zlp);
    RUN(test_set_address);
    RUN(test_out_data);
    RUN(test_stall_and_abort);
    RUN(test_setup_before_status_armed);
    RUN(test_bulk_out);
    RUN(test_bulk_in);
    RUN(test_shared_endpoint);
    RUN(test_bus_events_and_locking);
    RUN(test_register_timeout);
    printf("test_dcd_br25: %s\n", check_failed ? "FAILED" : "all ok");
    return check_failed ? 1 : 0;
}
