/*
 * TinyUSB device controller driver (dcd_*) for the BR25 full-speed USB controller (docs/usb.md).
 *
 * Our own code, written from the SDK's register map and access sequences and from what
 * open-smc-pad measured on the same chip (docs/usb.md lists each fact and where it comes from).
 * It runs polled (the bootloader calls dcd_int_handler() from its loop) or from the controller's
 * interrupt (the application). Built for the device with the JieLi toolchain, and on the host
 * against a model of the controller (BR25_USB_MODEL; firmware/tests/test_dcd_br25.c).
 *
 * Endpoint 0 follows the MUSB peripheral protocol: a SETUP stays unserviced (the host is NAKed)
 * until the stack answers it; DataEnd goes with the last data packet, and the controller runs
 * the status stage by itself. Other endpoints: one 64-byte DMA buffer per direction, one packet
 * in flight; bulk and interrupt only.
 */
#include <string.h>

#include "tusb.h"
#include "device/dcd.h"
#include "smclog.h"

#include "br25_usb_regs.h"
#include "dcd_br25.h"

#ifdef BR25_USB_MODEL
uint32_t br25_model_rd(uint32_t reg);
void br25_model_wr(uint32_t reg, uint32_t value);
void br25_model_dma(uint32_t reg, void *buf);
#define RD(reg) br25_model_rd(reg)
#define WR(reg, v) br25_model_wr((reg), (v))
#define DMA(reg, buf) br25_model_dma((reg), (buf))
#define CSYNC() ((void)0)
#else
#define RD(reg) (*(volatile uint32_t *)(uintptr_t)(reg))
#define WR(reg, v) (*(volatile uint32_t *)(uintptr_t)(reg) = (uint32_t)(v))
#define DMA(reg, buf) WR((reg), (uint32_t)(uintptr_t)(buf))
#define CSYNC() __asm__ volatile("csync")
#endif

/* Interrupt context may not use the interrupted code's cause (smclog.h): the driver logs with
 * cause 0 everywhere. */
#define ULOG(level, ...) LOGC_##level(0, usb, __VA_ARGS__)

/* Endpoints the driver serves: the hardware's 0..4, or fewer when the build asks for fewer (the
 * bootloader uses endpoint 0 only, and saves the other buffers). */
#if TUP_DCD_ENDPOINT_MAX < BR25_USB_EPS
#define EPS TUP_DCD_ENDPOINT_MAX
#else
#define EPS BR25_USB_EPS
#endif

#define MPS 64u
#define REG_WAIT 20000u /* CON1 ACK polls, as the SDK */
#define RX_WAIT 1600u   /* count reads while a packet streams in (64 bytes take ~43 µs): room up to 192 MHz */
#define RX_AGAIN 3u     /* packets taken per interrupt: the RX FIFO holds two */

enum ep0_stage {
    EP0_IDLE,   /* waiting for a SETUP */
    EP0_SETUP,  /* SETUP received, not serviced: the stack decides what comes next */
    EP0_IN,     /* data stage, device to host */
    EP0_OUT,    /* data stage, host to device */
    EP0_STATUS, /* DataEnd written: the controller runs the status stage */
};

struct xfer {
    uint8_t *buf;
    uint16_t len, done;
    uint16_t last; /* size of the IN packet in flight */
    uint16_t mps;
    bool busy, open;
};

static struct {
    struct xfer ep[EPS][2]; /* [number][direction], TUSB_DIR_IN = 1 */
    uint8_t stage;
    bool req_in;            /* the SETUP's direction */
    uint16_t wlength, total; /* the data stage: its length and the bytes so far */
    bool in_end;            /* the IN packet in flight carries DataEnd */
    uint8_t status_ep;      /* the status stage's endpoint address, once the stack arms it */
    bool status_armed, status_done, status_internal, stalled;
    bool setup_parked;      /* a SETUP waits in the FIFO until the stack retires the last transfer */
    bool stall_skip;        /* swallow the second half of an ignored stall pair */
    uint8_t addr;
    bool addr_pending;
    bool irq_enabled, in_isr;
    uint8_t tx_ie, rx_ie;   /* INTRTX1E / INTRRX1E as written */
    struct br25_usb_stats stats;
} d;

static uint8_t ep0_buf[MPS] __attribute__((aligned(4)));
static uint8_t ep_buf[EPS > 1 ? EPS - 1 : 1][2][MPS] __attribute__((aligned(4)));

/* --- register access ---------------------------------------------------------------------- */

static void reg_timeout(uint32_t reg)
{
    if (d.stats.reg_timeouts++ == 0)
        ULOG(ERROR, "register %02x: no ACK from the controller", reg);
}

static uint32_t mu_rd(uint32_t reg)
{
    WR(BR25_USB_CON1, (reg << 8) | BR25_CON1_RNW);
    CSYNC();
    for (uint32_t n = REG_WAIT; n; n--) {
        uint32_t v = RD(BR25_USB_CON1);
        if (v & BR25_CON1_ACK)
            return v & 0xFFu;
    }
    reg_timeout(reg);
    return 0;
}

static void mu_wr(uint32_t reg, uint32_t value)
{
    WR(BR25_USB_CON1, (reg << 8) | (value & 0xFFu));
    CSYNC();
    for (uint32_t n = REG_WAIT; n; n--)
        if (RD(BR25_USB_CON1) & BR25_CON1_ACK)
            return;
    reg_timeout(reg);
}

static void mu_index(unsigned ep)
{
    mu_wr(BR25_MUSB_INDEX, ep);
}

static uint32_t csr0(void)
{
    mu_index(0);
    return mu_rd(BR25_MUSB_CSR0);
}

static void csr0_wr(uint32_t value)
{
    mu_index(0);
    mu_wr(BR25_MUSB_CSR0, value);
}

/* Task-side calls mask the controller's interrupt: INDEX is shared with the handler. */
static bool lock(void)
{
    if (d.in_isr || !d.irq_enabled)
        return false;
    br25_usb_irq_set(false);
    return true;
}

static void unlock(bool locked)
{
    if (locked)
        br25_usb_irq_set(true);
}

static void complete(uint8_t ep_addr, uint16_t len)
{
    dcd_event_xfer_complete(0, ep_addr, len, XFER_RESULT_SUCCESS, d.in_isr);
}

const struct br25_usb_stats *br25_usb_stats(void)
{
    return &d.stats;
}

/* --- controller ---------------------------------------------------------------------------- */

static void ep0_reset(void)
{
    memset(d.ep, 0, sizeof d.ep);
    d.stage = EP0_IDLE;
    d.status_armed = d.status_done = d.status_internal = d.stalled = d.setup_parked = false;
    d.stall_skip = false;
    d.addr_pending = false;
    DMA(BR25_USB_EP0_ADR, ep0_buf);
    d.tx_ie = 1u; /* endpoint 0's interrupt is bit 0 of the TX register */
    d.rx_ie = 0;
    mu_wr(BR25_MUSB_INTRTX1E, d.tx_ie);
    mu_wr(BR25_MUSB_INTRRX1E, d.rx_ie);
}

bool dcd_init(uint8_t rhport, const tusb_rhport_init_t *rh_init)
{
    (void)rh_init;
    struct br25_usb_stats keep = d.stats;
    memset(&d, 0, sizeof d);
    d.stats = keep;

    /* Pads: USB mode, digital inputs on, slew rate; no pulls yet (attached in dcd_connect). */
    uint32_t io = RD(BR25_USB_IO_CON0);
    io = (io | BR25_IO_DPDIE | BR25_IO_DMDIE | BR25_IO_SR) & ~BR25_IO_IO_MODE;
    io &= ~(BR25_IO_DPPD | BR25_IO_DMPD | BR25_IO_DPPU | BR25_IO_DMPU);
    WR(BR25_USB_IO_CON0, io);

    /* The SDK's order: core in reset, CON1 cleared, PHY on, then out of reset. */
    WR(BR25_USB_CON0, RD(BR25_USB_CON0) &
                          ~(BR25_CON0_NRST | BR25_CON0_CID | BR25_CON0_VBUS | BR25_CON0_SIE_PND));
    WR(BR25_USB_CON1, 0);
    WR(BR25_USB_CON0, RD(BR25_USB_CON0) | BR25_CON0_PHY_ON);
    WR(BR25_USB_CON0, RD(BR25_USB_CON0) | BR25_CON0_NRST | BR25_CON0_TM1 | BR25_CON0_CID |
                          BR25_CON0_VBUS);

    mu_wr(BR25_MUSB_INTRUSBE, 0);
    mu_wr(BR25_MUSB_INTRTX2E, 0);
    mu_wr(BR25_MUSB_INTRRX2E, 0);
    ep0_reset();
    (void)mu_rd(BR25_MUSB_INTRUSB); /* drop anything pending: these registers clear on read */
    (void)mu_rd(BR25_MUSB_INTRTX1);
    (void)mu_rd(BR25_MUSB_INTRRX1);
    mu_wr(BR25_MUSB_INTRUSBE, BR25_INTR_RESET | BR25_INTR_SUSPEND | BR25_INTR_RESUME);
    WR(BR25_USB_CON0, RD(BR25_USB_CON0) | BR25_CON0_SIEIE);

    dcd_connect(rhport);
    ULOG(INFO, "controller on, attached");
    return true;
}

void dcd_connect(uint8_t rhport)
{
    (void)rhport;
    mu_wr(BR25_MUSB_POWER, BR25_POWER_SOFTCONN);
    WR(BR25_USB_IO_CON0, RD(BR25_USB_IO_CON0) | BR25_IO_DPPU);
}

void dcd_disconnect(uint8_t rhport)
{
    (void)rhport;
    WR(BR25_USB_IO_CON0, RD(BR25_USB_IO_CON0) & ~BR25_IO_DPPU);
    mu_wr(BR25_MUSB_POWER, 0);
    ULOG(INFO, "detached");
}

void dcd_int_enable(uint8_t rhport)
{
    (void)rhport;
    d.irq_enabled = true;
    br25_usb_irq_set(true);
}

void dcd_int_disable(uint8_t rhport)
{
    (void)rhport;
    d.irq_enabled = false;
    br25_usb_irq_set(false);
}

void dcd_remote_wakeup(uint8_t rhport)
{
    (void)rhport;
    /* Needs RESUME held for 1-15 ms; our descriptors do not offer remote wakeup. */
    ULOG(WARN, "remote wakeup is not supported");
}

void dcd_sof_enable(uint8_t rhport, bool en)
{
    (void)rhport;
    bool l = lock();
    mu_wr(BR25_MUSB_INTRUSBE, BR25_INTR_RESET | BR25_INTR_SUSPEND | BR25_INTR_RESUME |
                                  (en ? BR25_INTR_SOF : 0));
    unlock(l);
}

/* --- endpoint 0 ---------------------------------------------------------------------------- */

static void ep0_send(void)
{
    struct xfer *x = &d.ep[0][TUSB_DIR_IN];
    uint16_t n = (uint16_t)tu_min16(x->len - x->done, MPS);
    if (n)
        memcpy(ep0_buf, x->buf + x->done, n);
    x->last = n;
    d.in_end = n < MPS || d.total + n >= d.wlength;
    DMA(BR25_USB_EP0_ADR, ep0_buf);
    WR(BR25_USB_EP_CNT(0), n);
    csr0_wr(BR25_CSR0_TXPKTRDY | (d.in_end ? BR25_CSR0_DATAEND : 0));
    ULOG(TRACE, "ep 80: %u bytes%s", n, d.in_end ? ", end" : "");
}

/* The status stage is over (or, after an IN data stage, left to the controller). */
static void ep0_status_over(void)
{
    if (d.addr_pending) {
        mu_wr(BR25_MUSB_FADDR, d.addr);
        d.addr_pending = false;
        ULOG(DEBUG, "address %u", d.addr);
    }
    if (d.status_internal)
        d.status_internal = false;
    else if (d.status_armed) {
        d.status_armed = false;
        complete(d.status_ep, 0);
    } else
        d.status_done = true;
    d.stage = EP0_IDLE;
}

/* Takes the SETUP from the buffer into the driver's state; the caller raises the event, last:
 * from task context, TinyUSB's queue unmasks the interrupt (docs/usb.md, "Locking"). */
static bool ep0_setup_take(uint8_t setup[8])
{
    mu_index(0);
    uint32_t n = mu_rd(BR25_MUSB_COUNT0);
    if (n != 8) {
        ULOG(WARN, "ep 0: %u bytes where a SETUP was expected; dropped", n);
        DMA(BR25_USB_EP0_ADR, ep0_buf);
        csr0_wr(BR25_CSR0_SVDRXPKTRDY);
        return false;
    }
    memcpy(setup, ep0_buf, 8);
    DMA(BR25_USB_EP0_ADR, ep0_buf);
    d.stage = EP0_SETUP;
    d.req_in = (setup[0] & 0x80u) != 0;
    d.wlength = (uint16_t)(setup[6] | setup[7] << 8);
    d.total = 0;
    d.status_armed = d.status_done = d.stalled = false;
    d.ep[0][0].busy = d.ep[0][1].busy = false;
    d.stats.setups++;
    ULOG(DEBUG, "setup %02x %02x %02x%02x %02x%02x %u", setup[0], setup[1], setup[3], setup[2],
         setup[5], setup[4], d.wlength);
    return true;
}

static void ep0_setup(void)
{
    uint8_t setup[8];
    if (ep0_setup_take(setup))
        dcd_event_setup_received(0, setup, d.in_isr);
}

/* One OUT data packet into the armed transfer. */
static void ep0_rx(void)
{
    struct xfer *x = &d.ep[0][TUSB_DIR_OUT];
    mu_index(0);
    uint16_t n = (uint16_t)mu_rd(BR25_MUSB_COUNT0);
    uint16_t room = (uint16_t)(x->len - x->done);
    uint16_t k = n < room ? n : room;
    if (n > room)
        d.stats.rx_truncated += n - room;
    memcpy(x->buf + x->done, ep0_buf, k);
    DMA(BR25_USB_EP0_ADR, ep0_buf);
    x->done = (uint16_t)(x->done + k);
    d.total = (uint16_t)(d.total + n);
    bool end = n < MPS || d.total >= d.wlength;
    csr0_wr(BR25_CSR0_SVDRXPKTRDY | (end ? BR25_CSR0_DATAEND : 0));
    ULOG(TRACE, "ep 00: %u bytes%s", n, end ? ", end" : "");
    if (end)
        d.stage = EP0_STATUS;
    if (end || x->done >= x->len) {
        x->busy = false;
        complete(0x00, x->done);
    }
}

static void ep0_isr(void)
{
    uint32_t c = csr0();
    if (c & BR25_CSR0_SENTSTALL) {
        csr0_wr(0); /* clears SentStall; the stall ends with the next SETUP */
        DMA(BR25_USB_EP0_ADR, ep0_buf);
        d.stage = EP0_IDLE;
        d.stats.stalls_sent++;
    }
    if (c & BR25_CSR0_SETUPEND) {
        csr0_wr(BR25_CSR0_SVDSETUPEND);
        DMA(BR25_USB_EP0_ADR, ep0_buf); /* the packet in flight may have moved the pointer */
        d.stats.setup_ends++;
        ULOG(DEBUG, "control transfer ended early by the host (stage %u)", d.stage);
        d.stage = EP0_IDLE;
        d.status_armed = d.status_internal = false;
        d.ep[0][0].busy = d.ep[0][1].busy = false;
    }
    if (d.stage == EP0_STATUS)
        ep0_status_over();
    if (d.stage == EP0_IN && d.ep[0][TUSB_DIR_IN].busy && !(c & BR25_CSR0_TXPKTRDY)) {
        struct xfer *x = &d.ep[0][TUSB_DIR_IN];
        x->done = (uint16_t)(x->done + x->last);
        d.total = (uint16_t)(d.total + x->last);
        DMA(BR25_USB_EP0_ADR, ep0_buf); /* the pointer moved with the packet: rewind for the next one */
        if (x->done < x->len && !d.in_end) {
            ep0_send();
        } else {
            x->busy = false;
            /* After the last IN packet the controller completes the status OUT by itself, possibly
             * in this very interrupt: take it as done. */
            if (d.in_end)
                ep0_status_over();
            complete(0x80, x->done);
        }
    }
    if (c & BR25_CSR0_RXPKTRDY) {
        if (d.stage == EP0_OUT) {
            if (d.ep[0][TUSB_DIR_OUT].busy)
                ep0_rx(); /* otherwise it waits (NAKed) until the stack arms the next part */
        } else if (d.stage == EP0_IDLE) {
            if (d.status_done) {
                /* The last transfer's status stage is over on the bus, but the stack has not armed
                 * it yet: it would take the new SETUP's place. The SETUP waits (docs/usb.md). */
                d.setup_parked = true;
                ULOG(DEBUG, "setup parked until the last status is armed");
            } else {
                ep0_setup();
            }
        }
    }
}

static bool ep0_xfer(uint8_t ep_addr, uint8_t *buf, uint16_t len)
{
    bool in = tu_edpt_dir(ep_addr) == TUSB_DIR_IN;
    bool status = d.wlength == 0 ? in : in != d.req_in;
    d.stall_skip = false;

    if (status) {
        d.status_ep = ep_addr;
        if (d.stage == EP0_SETUP) {
            /* No data stage: service the SETUP with DataEnd; the controller answers the status IN. */
            csr0_wr(BR25_CSR0_SVDRXPKTRDY | BR25_CSR0_DATAEND);
            d.stage = EP0_STATUS;
            d.status_armed = true;
        } else if (d.status_done) {
            d.status_done = false;
            uint8_t setup[8];
            bool next = d.setup_parked && ep0_setup_take(setup);
            d.setup_parked = false;
            complete(ep_addr, 0);
            if (next)
                dcd_event_setup_received(0, setup, d.in_isr);
        } else if (d.stage == EP0_STATUS) {
            d.status_armed = true;
        }
        /* Otherwise the transfer was aborted (SetupEnd, bus reset): nothing will come. */
        return true;
    }

    if (d.stage != EP0_SETUP && d.stage != (in ? EP0_IN : EP0_OUT)) {
        /* The control transfer is gone (SetupEnd, stall, bus reset): a new SETUP follows. */
        ULOG(DEBUG, "ep %02x: data for an ended control transfer dropped", ep_addr);
        return true;
    }
    struct xfer *x = &d.ep[0][in];
    x->buf = buf;
    x->len = len;
    x->done = 0;
    x->busy = true;
    if (d.stage == EP0_SETUP) {
        csr0_wr(BR25_CSR0_SVDRXPKTRDY);
        d.stage = in ? EP0_IN : EP0_OUT;
    }
    if (in) {
        ep0_send();
    } else if (csr0() & BR25_CSR0_RXPKTRDY) {
        ep0_rx(); /* the packet came before the stack asked for it */
    }
    return true;
}

void dcd_set_address(uint8_t rhport, uint8_t dev_addr)
{
    (void)rhport;
    bool l = lock();
    /* The status IN goes out with the old address; the new one is set once it is over. */
    d.addr = dev_addr;
    d.addr_pending = true;
    d.status_internal = true;
    csr0_wr(BR25_CSR0_SVDRXPKTRDY | BR25_CSR0_DATAEND);
    d.stage = EP0_STATUS;
    unlock(l);
}

/* --- other endpoints ----------------------------------------------------------------------- */

static void tx_packet(uint8_t n)
{
    struct xfer *x = &d.ep[n][TUSB_DIR_IN];
    uint8_t *buf = ep_buf[n - 1][TUSB_DIR_IN];
    uint16_t k = (uint16_t)tu_min16(x->len - x->done, x->mps);
    if (k)
        memcpy(buf, x->buf + x->done, k);
    x->last = k;
    DMA(BR25_USB_EP_TADR(n), buf); /* the DMA pointer moves on with every packet */
    WR(BR25_USB_EP_CNT(n), k);
    mu_index(n);
    uint32_t c = mu_rd(BR25_MUSB_TXCSR1);
    mu_wr(BR25_MUSB_TXCSR1, (c & BR25_TXCSR_SENDSTALL) | BR25_TXCSR_TXPKTRDY);
}

static void tx_isr(uint8_t n)
{
    struct xfer *x = &d.ep[n][TUSB_DIR_IN];
    mu_index(n);
    uint32_t c = mu_rd(BR25_MUSB_TXCSR1);
    if (c & (BR25_TXCSR_SENTSTALL | BR25_TXCSR_UNDERRUN))
        mu_wr(BR25_MUSB_TXCSR1, c & BR25_TXCSR_SENDSTALL); /* clears them; TxPktRdy stays clear */
    if (!x->busy || (c & BR25_TXCSR_TXPKTRDY))
        return;
    x->done = (uint16_t)(x->done + x->last);
    d.stats.tx_packets++;
    if (x->done < x->len) {
        tx_packet(n);
    } else {
        x->busy = false;
        complete((uint8_t)(n | TUSB_DIR_IN_MASK), x->done);
    }
}

/* Size of the packet in the RX FIFO. In a run of full packets RxPktRdy can come up while the
 * packet is still streaming in, and the count reads 0; a real zero-length packet comes without
 * FIFOFull (docs/usb.md, item 2). */
static uint16_t rx_count(uint32_t c)
{
    uint16_t n = (uint16_t)(mu_rd(BR25_MUSB_RXCOUNT1) | mu_rd(BR25_MUSB_RXCOUNT2) << 8);
    if (n || !(c & BR25_RXCSR_FIFOFULL))
        return n;
    d.stats.rx_waits++;
    uint32_t k;
    for (k = 1; k <= RX_WAIT && !n; k++)
        n = (uint16_t)(mu_rd(BR25_MUSB_RXCOUNT1) | mu_rd(BR25_MUSB_RXCOUNT2) << 8);
    if (k > d.stats.rx_wait_max)
        d.stats.rx_wait_max = k;
    if (!n) {
        d.stats.rx_wait_fails++;
        ULOG(WARN, "rx: the packet length stayed 0 for %u reads", RX_WAIT);
    }
    return n;
}

static void rx_take(uint8_t n)
{
    struct xfer *x = &d.ep[n][TUSB_DIR_OUT];
    uint8_t *buf = ep_buf[n - 1][TUSB_DIR_OUT];
    for (unsigned again = 0; again < RX_AGAIN && x->busy; again++) {
        mu_index(n);
        uint32_t c = mu_rd(BR25_MUSB_RXCSR1);
        if (c & BR25_RXCSR_SENTSTALL)
            mu_wr(BR25_MUSB_RXCSR1, c & BR25_RXCSR_SENDSTALL);
        if (!(c & BR25_RXCSR_RXPKTRDY))
            return;
        uint16_t count = rx_count(c);
        uint16_t room = (uint16_t)(x->len - x->done);
        uint16_t k = count < room ? count : room;
        if (count > room) {
            d.stats.rx_truncated += count - room;
            ULOG(WARN, "ep %02x: %u bytes for %u of room", n, count, room);
        }
        memcpy(x->buf + x->done, buf, k);
        DMA(BR25_USB_EP_RADR(n), buf); /* rewind, then pop the packet */
        mu_wr(BR25_MUSB_RXCSR1, (c & ~BR25_RXCSR_POP_CLEAR & 0xFFu) | BR25_RXCSR_FLUSHFIFO);
        x->done = (uint16_t)(x->done + k);
        d.stats.rx_packets++;
        if (x->done >= x->len || count < x->mps) {
            x->busy = false;
            complete(n, x->done);
        }
    }
}

bool dcd_edpt_open(uint8_t rhport, const tusb_desc_endpoint_t *desc)
{
    (void)rhport;
    uint8_t n = tu_edpt_number(desc->bEndpointAddress);
    uint8_t dir = tu_edpt_dir(desc->bEndpointAddress);
    uint16_t mps = tu_edpt_packet_size(desc);
    uint8_t type = desc->bmAttributes.xfer;
    if (n == 0 || n >= EPS || mps == 0 || mps > MPS || type == TUSB_XFER_ISOCHRONOUS) {
        ULOG(ERROR, "ep %02x: cannot open (type %u, %u bytes)", desc->bEndpointAddress, type, mps);
        return false;
    }
    bool l = lock();
    struct xfer *x = &d.ep[n][dir];
    memset(x, 0, sizeof *x);
    x->mps = mps;
    x->open = true;
    mu_index(n);
    if (dir == TUSB_DIR_IN) {
        DMA(BR25_USB_EP_TADR(n), ep_buf[n - 1][TUSB_DIR_IN]);
        mu_wr(BR25_MUSB_TXMAXP, BR25_MUSB_MAXP_ANY);
        mu_wr(BR25_MUSB_TXCSR1, BR25_TXCSR_CLRDATATOG | BR25_TXCSR_FLUSHFIFO);
        mu_wr(BR25_MUSB_TXCSR2, 0);
        d.tx_ie |= (uint8_t)(1u << n);
        mu_wr(BR25_MUSB_INTRTX1E, d.tx_ie);
    } else {
        DMA(BR25_USB_EP_RADR(n), ep_buf[n - 1][TUSB_DIR_OUT]);
        mu_wr(BR25_MUSB_RXMAXP, BR25_MUSB_MAXP_ANY);
        mu_wr(BR25_MUSB_RXCSR1, BR25_RXCSR_CLRDATATOG | BR25_RXCSR_FLUSHFIFO);
        mu_wr(BR25_MUSB_RXCSR2, 0);
        d.rx_ie |= (uint8_t)(1u << n);
        mu_wr(BR25_MUSB_INTRRX1E, d.rx_ie);
    }
    unlock(l);
    ULOG(DEBUG, "ep %02x open: type %u, %u bytes", desc->bEndpointAddress, type, mps);
    return true;
}

void dcd_edpt_close_all(uint8_t rhport)
{
    (void)rhport;
    bool l = lock();
    for (uint8_t n = 1; n < EPS; n++) {
        if (d.ep[n][TUSB_DIR_IN].open || d.ep[n][TUSB_DIR_OUT].open) {
            mu_index(n);
            mu_wr(BR25_MUSB_TXCSR1, BR25_TXCSR_FLUSHFIFO);
            mu_wr(BR25_MUSB_RXCSR1, BR25_RXCSR_FLUSHFIFO);
        }
        memset(d.ep[n], 0, sizeof d.ep[n]);
    }
    d.tx_ie = 1u;
    d.rx_ie = 0;
    mu_wr(BR25_MUSB_INTRTX1E, d.tx_ie);
    mu_wr(BR25_MUSB_INTRRX1E, d.rx_ie);
    unlock(l);
}

bool dcd_edpt_xfer(uint8_t rhport, uint8_t ep_addr, uint8_t *buffer, uint16_t total_bytes,
                   bool is_isr)
{
    (void)rhport;
    (void)is_isr; /* the driver knows: d.in_isr */
    uint8_t n = tu_edpt_number(ep_addr);
    uint8_t dir = tu_edpt_dir(ep_addr);
    if (n >= EPS || (n && !d.ep[n][dir].open))
        return false;
    bool l = lock();
    bool ok = true;
    if (n == 0) {
        ok = ep0_xfer(ep_addr, buffer, total_bytes);
    } else {
        struct xfer *x = &d.ep[n][dir];
        x->buf = buffer;
        x->len = total_bytes;
        x->done = 0;
        x->busy = true;
        if (dir == TUSB_DIR_IN)
            tx_packet(n);
        else
            rx_take(n); /* a packet may already be waiting */
    }
    unlock(l);
    return ok;
}

void dcd_edpt_stall(uint8_t rhport, uint8_t ep_addr)
{
    (void)rhport;
    uint8_t n = tu_edpt_number(ep_addr);
    if (n >= EPS)
        return;
    bool l = lock();
    if (n == 0) {
        if (d.stall_skip) {
            d.stall_skip = false; /* the other direction of the pair ignored below */
        } else if (d.status_done) {
            /* The stack refuses a transfer whose status stage is already over (an IN data stage
             * ends with DataEnd): nothing to stall on the bus any more. A waiting SETUP goes on. */
            ULOG(WARN, "ep 0: stall after the status stage, ignored");
            d.status_done = false;
            d.stall_skip = true;
            uint8_t setup[8];
            bool next = d.setup_parked && ep0_setup_take(setup);
            d.setup_parked = false;
            if (next)
                dcd_event_setup_received(0, setup, d.in_isr);
        } else if (!d.stalled) { /* the stack stalls both directions: one is enough */
            csr0_wr(BR25_CSR0_SENDSTALL | (d.stage == EP0_SETUP ? BR25_CSR0_SVDRXPKTRDY : 0));
            d.stalled = true;
            d.stage = EP0_IDLE;
            d.ep[0][0].busy = d.ep[0][1].busy = false;
            ULOG(DEBUG, "ep 0 stalled");
        }
    } else {
        d.ep[n][tu_edpt_dir(ep_addr)].busy = false;
        mu_index(n);
        if (tu_edpt_dir(ep_addr) == TUSB_DIR_IN)
            mu_wr(BR25_MUSB_TXCSR1, BR25_TXCSR_SENDSTALL);
        else
            mu_wr(BR25_MUSB_RXCSR1, BR25_RXCSR_SENDSTALL);
        ULOG(DEBUG, "ep %02x stalled", ep_addr);
    }
    unlock(l);
}

void dcd_edpt_clear_stall(uint8_t rhport, uint8_t ep_addr)
{
    (void)rhport;
    uint8_t n = tu_edpt_number(ep_addr);
    if (n == 0 || n >= EPS)
        return;
    bool l = lock();
    mu_index(n);
    /* Writing the register without SendStall/SentStall clears them; the toggle restarts at DATA0. */
    if (tu_edpt_dir(ep_addr) == TUSB_DIR_IN)
        mu_wr(BR25_MUSB_TXCSR1, BR25_TXCSR_CLRDATATOG);
    else
        mu_wr(BR25_MUSB_RXCSR1, BR25_RXCSR_CLRDATATOG);
    unlock(l);
    ULOG(DEBUG, "ep %02x stall cleared", ep_addr);
}

/* --- interrupt ----------------------------------------------------------------------------- */

void dcd_int_handler(uint8_t rhport)
{
    (void)rhport;
    d.in_isr = true;
    (void)RD(BR25_USB_CON0); /* reading CON0 acknowledges the controller's interrupt */
    uint32_t usb = mu_rd(BR25_MUSB_INTRUSB);
    uint32_t tx = mu_rd(BR25_MUSB_INTRTX1);
    uint32_t rx = mu_rd(BR25_MUSB_INTRRX1);

    if (usb & BR25_INTR_RESET) {
        d.stats.resets++;
        ep0_reset();
        ULOG(INFO, "bus reset");
        dcd_event_bus_reset(0, TUSB_SPEED_FULL, true);
        tx &= 1u; /* endpoint state is gone; only a SETUP can follow */
        rx = 0;
    }
    if (usb & BR25_INTR_SUSPEND) {
        d.stats.suspends++;
        ULOG(DEBUG, "suspend");
        dcd_event_bus_signal(0, DCD_EVENT_SUSPEND, true);
    }
    if (usb & BR25_INTR_RESUME) {
        d.stats.resumes++;
        ULOG(DEBUG, "resume");
        dcd_event_bus_signal(0, DCD_EVENT_RESUME, true);
    }
    if (usb & BR25_INTR_SOF) {
        uint32_t frame = mu_rd(BR25_MUSB_FRAME1) | (mu_rd(BR25_MUSB_FRAME2) & 0x7u) << 8;
        dcd_event_sof(0, frame, true);
    }
    if (tx & 1u)
        ep0_isr();
    for (uint8_t n = 1; n < EPS; n++) {
        if (tx & (1u << n))
            tx_isr(n);
        if (rx & (1u << n))
            rx_take(n);
    }
    d.in_isr = false;
}
