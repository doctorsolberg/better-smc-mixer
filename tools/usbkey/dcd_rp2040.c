/*
 * TinyUSB device controller driver for the RP2040, polled (no interrupts) and without the
 * Raspberry Pi SDK: single-buffered endpoints, full speed. Enough for one CDC interface.
 *
 * Hardware model (RP2040 datasheet, USB): each endpoint direction has a buffer control word in
 * the controller's memory. Software fills it (length, DATA0/1, FULL for IN) and then sets
 * AVAILABLE; the controller does one packet and sets the endpoint's bit in BUFF_STATUS. Endpoint 0
 * uses the fixed buffer at 0x100 for both directions.
 */
#include <string.h>

#include "rp2040.h"
#include "tusb.h"
#include "device/dcd.h"

#define EP_MAX 16

struct ep {
    uint8_t *buf;     /* the transfer's buffer (NULL for a zero-length one) */
    uint16_t len;     /* bytes asked for */
    uint16_t done;    /* bytes moved so far */
    uint16_t mps;     /* max packet size */
    uint16_t offset;  /* data buffer in the controller memory */
    uint8_t data1;    /* next packet is DATA1 */
    bool active;
};

static struct ep eps[EP_MAX][2];  /* [number][direction: 0 out, 1 in] */
static uint16_t next_offset;

static volatile uint8_t *dpram(uint16_t offset)
{
    return (volatile uint8_t *)(uintptr_t)(USB_DPRAM + offset);
}

/* Sets the buffer control word: everything first, AVAILABLE after (the datasheet asks for a
 * few cycles in between so the controller sees a consistent word). */
static void buf_ctrl_write(uint8_t num, uint8_t in, uint32_t v)
{
    REG(USB_BUF_CTRL(num, in)) = v;
    __asm__ volatile("nop\n nop\n nop\n nop\n nop\n nop");
    REG(USB_BUF_CTRL(num, in)) = v | BUF_CTRL_AVAILABLE;
}

/* Starts the next packet of the endpoint's transfer. */
static void arm(uint8_t num, uint8_t in)
{
    struct ep *e = &eps[num][in];
    uint16_t n = (uint16_t)(e->len - e->done);
    if (n > e->mps)
        n = e->mps;
    uint32_t v = n | (e->data1 ? BUF_CTRL_DATA1 : 0);
    if (in) {
        for (uint16_t i = 0; i < n; i++)
            dpram(e->offset)[i] = e->buf[e->done + i];
        v |= BUF_CTRL_FULL;
    }
    e->data1 ^= 1;
    buf_ctrl_write(num, in, v);
}

/* A packet of the endpoint finished: count it, start the next one or report the transfer. */
static void packet_done(uint8_t num, uint8_t in)
{
    struct ep *e = &eps[num][in];
    if (!e->active)
        return;
    uint16_t n = (uint16_t)(REG(USB_BUF_CTRL(num, in)) & BUF_CTRL_LEN_MASK);
    if (!in) {
        if (n > e->len - e->done)
            n = (uint16_t)(e->len - e->done);
        for (uint16_t i = 0; i < n; i++)
            e->buf[e->done + i] = dpram(e->offset)[i];
    }
    e->done = (uint16_t)(e->done + n);
    if (n == e->mps && e->done < e->len) {
        arm(num, in);
        return;
    }
    e->active = false;
    dcd_event_xfer_complete(0, (uint8_t)(num | (in ? TUSB_DIR_IN_MASK : 0)), e->done, XFER_RESULT_SUCCESS, false);
}

static void reset_endpoints(void)
{
    for (uint8_t n = 1; n < EP_MAX; n++) {
        for (uint8_t d = 0; d < 2; d++) {
            REG(USB_EP_CTRL(n, d)) = 0;
            REG(USB_BUF_CTRL(n, d)) = 0;
            eps[n][d] = (struct ep){ 0 };
        }
    }
    for (uint8_t d = 0; d < 2; d++)
        eps[0][d] = (struct ep){ .mps = CFG_TUD_ENDPOINT0_SIZE, .offset = USB_EP0_BUF };
    next_offset = USB_DATA_BUF_START;
}

bool dcd_init(uint8_t rhport, const tusb_rhport_init_t *rh_init)
{
    (void)rh_init;
    REG_SET(RESETS_RESET) = RESET_USBCTRL;
    REG_CLR(RESETS_RESET) = RESET_USBCTRL;
    while (!(REG(RESETS_RESET_DONE) & RESET_USBCTRL))
        ;
    for (uint32_t i = 0; i < USB_DPRAM_SIZE; i += 4)
        REG(USB_DPRAM + i) = 0;
    reset_endpoints();
    REG(USB_MUXING) = USB_MUXING_TO_PHY | USB_MUXING_SOFTCON;
    /* No VBUS pin on the bus side: the device is powered by the host it talks to. */
    REG(USB_PWR) = USB_PWR_VBUS_DETECT | USB_PWR_VBUS_DETECT_OVERRIDE_EN;
    REG(USB_MAIN_CTRL) = USB_MAIN_CTRL_CONTROLLER_EN;
    REG(USB_SIE_CTRL) = USB_SIE_CTRL_EP0_INT_1BUF;
    REG(USB_SIE_STATUS) = 0xffffffffu;
    dcd_connect(rhport);
    return true;
}

/* Called from the main loop (the firmware enables no interrupts). */
void dcd_int_handler(uint8_t rhport)
{
    (void)rhport;
    /* Finished packets first, so a control transfer's status stage is seen before the next
     * SETUP. */
    uint32_t bs = REG(USB_BUFF_STATUS);
    if (bs) {
        REG(USB_BUFF_STATUS) = bs;
        for (uint8_t i = 0; i < 2 * EP_MAX; i++) {
            if (bs & (1u << i))
                packet_done((uint8_t)(i >> 1), (uint8_t)!(i & 1));
        }
    }
    uint32_t st = REG(USB_SIE_STATUS);
    if (st & USB_SIE_STATUS_SETUP_REC) {
        REG(USB_SIE_STATUS) = USB_SIE_STATUS_SETUP_REC;
        /* A SETUP ends whatever endpoint 0 was doing (a stall included); data and status stages
         * start with DATA1. */
        for (uint8_t d = 0; d < 2; d++) {
            REG(USB_BUF_CTRL(0, d)) = 0;
            eps[0][d].active = false;
            eps[0][d].data1 = 1;
        }
        uint8_t setup[8];
        for (int i = 0; i < 8; i++)
            setup[i] = dpram(0)[i];
        dcd_event_setup_received(0, setup, false);
    }
    if (st & USB_SIE_STATUS_BUS_RESET) {
        REG(USB_SIE_STATUS) = USB_SIE_STATUS_BUS_RESET;
        REG(USB_ADDR_ENDP) = 0;
        reset_endpoints();
        dcd_event_bus_reset(0, TUSB_SPEED_FULL, false);
    }
}

void dcd_int_enable(uint8_t rhport) { (void)rhport; }
void dcd_int_disable(uint8_t rhport) { (void)rhport; }

/* The address takes effect after the status stage (dcd_edpt0_status_complete). */
void dcd_set_address(uint8_t rhport, uint8_t dev_addr)
{
    (void)dev_addr;
    dcd_edpt_xfer(rhport, TUSB_DIR_IN_MASK, NULL, 0, false);
}

void dcd_edpt0_status_complete(uint8_t rhport, const tusb_control_request_t *request)
{
    (void)rhport;
    if (request->bmRequestType_bit.recipient == TUSB_REQ_RCPT_DEVICE &&
        request->bmRequestType_bit.type == TUSB_REQ_TYPE_STANDARD &&
        request->bRequest == TUSB_REQ_SET_ADDRESS)
        REG(USB_ADDR_ENDP) = request->wValue & 0x7fu;
}

void dcd_remote_wakeup(uint8_t rhport) { (void)rhport; }

void dcd_connect(uint8_t rhport)
{
    (void)rhport;
    REG_SET(USB_SIE_CTRL) = USB_SIE_CTRL_PULLUP_EN;
}

void dcd_disconnect(uint8_t rhport)
{
    (void)rhport;
    REG_CLR(USB_SIE_CTRL) = USB_SIE_CTRL_PULLUP_EN;
}

void dcd_sof_enable(uint8_t rhport, bool en)
{
    (void)rhport;
    (void)en;
}

bool dcd_edpt_open(uint8_t rhport, const tusb_desc_endpoint_t *desc_ep)
{
    (void)rhport;
    uint8_t num = tu_edpt_number(desc_ep->bEndpointAddress);
    uint8_t in = tu_edpt_dir(desc_ep->bEndpointAddress) == TUSB_DIR_IN;
    uint16_t mps = tu_edpt_packet_size(desc_ep);
    uint16_t size = (uint16_t)((mps + 63u) & ~63u);
    if (num == 0 || num >= EP_MAX || mps > 64 || next_offset + size > USB_DPRAM_SIZE)
        return false;
    eps[num][in] = (struct ep){ .mps = mps, .offset = next_offset };
    REG(USB_EP_CTRL(num, in)) = EP_CTRL_ENABLE | EP_CTRL_INT_PER_BUFF |
        ((uint32_t)desc_ep->bmAttributes.xfer << EP_CTRL_TYPE_SHIFT) | next_offset;
    next_offset = (uint16_t)(next_offset + size);
    return true;
}

void dcd_edpt_close_all(uint8_t rhport)
{
    (void)rhport;
    struct ep ep0[2] = { eps[0][0], eps[0][1] };
    reset_endpoints();
    eps[0][0] = ep0[0];
    eps[0][1] = ep0[1];
}

bool dcd_edpt_xfer(uint8_t rhport, uint8_t ep_addr, uint8_t *buffer, uint16_t total_bytes, bool is_isr)
{
    (void)rhport;
    (void)is_isr;
    uint8_t num = tu_edpt_number(ep_addr);
    uint8_t in = tu_edpt_dir(ep_addr) == TUSB_DIR_IN;
    struct ep *e = &eps[num][in];
    e->buf = buffer;
    e->len = total_bytes;
    e->done = 0;
    e->active = true;
    arm(num, in);
    return true;
}

void dcd_edpt_stall(uint8_t rhport, uint8_t ep_addr)
{
    (void)rhport;
    uint8_t num = tu_edpt_number(ep_addr);
    uint8_t in = tu_edpt_dir(ep_addr) == TUSB_DIR_IN;
    if (num == 0)
        REG_SET(USB_EP_STALL_ARM) = in ? 1u : 2u;
    eps[num][in].active = false;
    buf_ctrl_write(num, in, BUF_CTRL_STALL);
}

void dcd_edpt_clear_stall(uint8_t rhport, uint8_t ep_addr)
{
    (void)rhport;
    uint8_t num = tu_edpt_number(ep_addr);
    uint8_t in = tu_edpt_dir(ep_addr) == TUSB_DIR_IN;
    REG(USB_BUF_CTRL(num, in)) = 0;
    eps[num][in].data1 = 0;
}
