/*
 * USB key firmware for an RP2040 board (Raspberry Pi Pico or RP2040-Zero): sends the JieLi USB
 * key so that the SMC-Mixer's chip starts in its ROM download mode (docs/recovery.md).
 *
 * Wiring: GP2 -[270 R]- D+, GP3 -[270 R]- D-, GND - GND of the mixer's USB cable.
 *
 * The board's own USB port is a serial port: a time-stamped log (kept from power-up, so nothing
 * is lost before a terminal opens) and one-letter commands:
 *   r  start an attempt (or start over)    x  abort the attempt
 *   d  replug: both lines low for 300 ms   s  status    h  help
 * An attempt also starts at power-up and with the BOOTSEL button.
 * LED (GP25 on the Pico; the RP2040-Zero has only an RGB LED, not used): 2 Hz while waiting for
 * the bus to be empty, 10 Hz while keying, on once the chip answered, a short blink every 2 s
 * when idle.
 *
 * Bare metal without the Pico SDK: the 12 MHz crystal, the USB PLL at 48 MHz for the USB
 * controller and the processor, a 1 us timer; no interrupts, everything is polled.
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>

#include "key.h"
#include "rp2040.h"
#include "tusb.h"

#define PIN_DP   2
#define PIN_DM   3
#define PIN_LED  25

#define USBKEY_VERSION "0.1"

/* MARK: clocks and pins */

static void unreset(uint32_t mask)
{
    REG_CLR(RESETS_RESET) = mask;
    while ((REG(RESETS_RESET_DONE) & mask) != mask)
        ;
}

static void clocks_init(void)
{
    REG(XOSC_STARTUP) = 47;  /* about 1 ms at 12 MHz, in units of 256 cycles */
    REG(XOSC_CTRL) = XOSC_CTRL_1_15MHZ | XOSC_CTRL_ENABLE;
    while (!(REG(XOSC_STATUS) & XOSC_STATUS_STABLE))
        ;
    REG(CLK_REF_CTRL) = CLK_REF_SRC_XOSC;
    while (!(REG(CLK_REF_SELECTED) & (1u << CLK_REF_SRC_XOSC)))
        ;
    /* The timer counts watchdog ticks: one per 12 cycles of the 12 MHz reference = 1 us. */
    REG(WATCHDOG_TICK) = WATCHDOG_TICK_ENABLE | 12u;

    /* USB PLL: 12 MHz x 120 = 1440 MHz VCO, / 6 / 5 = 48 MHz. */
    REG_SET(RESETS_RESET) = RESET_PLL_USB;
    unreset(RESET_PLL_USB);
    REG(PLL_USB_CS) = 1;
    REG(PLL_USB_FBDIV_INT) = 120;
    REG_CLR(PLL_USB_PWR) = PLL_PWR_PD | PLL_PWR_VCOPD;
    while (!(REG(PLL_USB_CS) & PLL_CS_LOCK))
        ;
    REG(PLL_USB_PRIM) = (6u << 16) | (5u << 12);
    REG_CLR(PLL_USB_PWR) = PLL_PWR_POSTDIVPD;

    REG(CLK_USB_CTRL) = 0;
    REG(CLK_USB_DIV) = 1u << 8;
    REG(CLK_USB_CTRL) = CLK_USB_ENABLE | CLK_USB_AUXSRC_PLL_USB;

    /* The processor too: back to the reference first (glitch-free), then to the PLL. */
    REG(CLK_SYS_CTRL) = 0;
    while (!(REG(CLK_SYS_SELECTED) & 1u))
        ;
    REG(CLK_SYS_CTRL) = CLK_SYS_AUXSRC_PLL_USB;
    REG(CLK_SYS_CTRL) = CLK_SYS_AUXSRC_PLL_USB | CLK_SYS_SRC_AUX;
    while (!(REG(CLK_SYS_SELECTED) & 2u))
        ;
}

static void pin_init(unsigned pin)
{
    REG(PADS_BANK0_GPIO(pin)) = PAD_IE | PAD_SCHMITT | PAD_DRIVE_4MA;  /* no pulls */
    REG(IO_BANK0_CTRL(pin)) = GPIO_FUNC_SIO;
    REG(SIO_GPIO_OE_CLR) = 1u << pin;
    REG(SIO_GPIO_OUT_CLR) = 1u << pin;
}

static uint32_t now_us(void)
{
    return REG(TIMER_TIMERAWL);
}

static void delay_us(uint32_t us)
{
    uint32_t t = now_us();
    while (now_us() - t <= us)
        ;
}

static const unsigned line_pin[2] = { [KEY_DP] = PIN_DP, [KEY_DM] = PIN_DM };

static void line_drive(enum key_line l, int level)
{
    uint32_t m = 1u << line_pin[l];
    REG(level ? SIO_GPIO_OUT_SET : SIO_GPIO_OUT_CLR) = m;  /* the level before the driver */
    REG(SIO_GPIO_OE_SET) = m;
}

static void line_release(enum key_line l)
{
    REG(SIO_GPIO_OE_CLR) = 1u << line_pin[l];
}

static int line_read(enum key_line l)
{
    return (int)((REG(SIO_GPIO_IN) >> line_pin[l]) & 1u);
}

static const struct key_io io = { line_drive, line_release, line_read, delay_us, now_us };

/* BOOTSEL is on the flash chip-select line: reading it stops the flash, so this runs from RAM
 * (nothing else runs: no interrupts, the second core sleeps). */
__attribute__((section(".ram_func"), noinline, long_call))
static bool bootsel_pressed(void)
{
    uint32_t ctrl = REG(IO_QSPI_SS_CTRL);
    REG(IO_QSPI_SS_CTRL) = (ctrl & ~QSPI_SS_OEOVER_MASK) | QSPI_SS_OEOVER_DISABLE;
    for (volatile int i = 0; i < 1000; i++)
        ;
    bool pressed = !(REG(SIO_GPIO_HI_IN) & (1u << 1));
    REG(IO_QSPI_SS_CTRL) = ctrl;
    return pressed;
}

/* MARK: log */

static char logbuf[8192];
static uint32_t log_head, log_tail;  /* running counts: written, sent */

static void log_putc(char c)
{
    logbuf[log_head++ % sizeof(logbuf)] = c;
    if (log_head - log_tail > sizeof(logbuf))
        log_tail = log_head - sizeof(logbuf);  /* the oldest lines go */
}

static void log_puts(const char *s)
{
    while (*s)
        log_putc(*s++);
}

static void log_number(uint32_t v, unsigned base, unsigned width)
{
    char d[10];
    unsigned n = 0;
    do {
        d[n++] = "0123456789abcdef"[v % base];
        v /= base;
    } while (v);
    while (width > n) {
        log_putc(base == 10 ? ' ' : '0');
        width--;
    }
    while (n)
        log_putc(d[--n]);
}

/* One line: "[seconds.milliseconds] text". Formats: %u %x (unsigned), %s, %c. */
__attribute__((format(printf, 1, 2)))
static void log_line(const char *fmt, ...)
{
    uint32_t ms = now_us() / 1000u;
    log_putc('[');
    log_number(ms / 1000u, 10, 6);
    log_putc('.');
    log_number(ms % 1000u / 100u, 10, 0);
    log_number(ms % 100u / 10u, 10, 0);
    log_number(ms % 10u, 10, 0);
    log_puts("] ");
    va_list ap;
    va_start(ap, fmt);
    for (const char *p = fmt; *p; p++) {
        if (*p != '%' || !p[1]) {
            log_putc(*p);
            continue;
        }
        switch (*++p) {
        case 'u': log_number(va_arg(ap, unsigned), 10, 0); break;
        case 'x': log_number(va_arg(ap, unsigned), 16, 0); break;
        case 's': log_puts(va_arg(ap, const char *)); break;
        case 'c': log_putc((char)va_arg(ap, int)); break;
        default: log_putc(*p); break;
        }
    }
    va_end(ap);
    log_puts("\r\n");
}

/* Sends what the terminal has not seen yet, once it is open (DTR set). */
static void log_drain(void)
{
    if (!tud_cdc_connected())
        return;
    while (log_tail != log_head) {
        uint32_t room = tud_cdc_write_available();
        if (!room)
            break;
        uint32_t start = log_tail % sizeof(logbuf);
        uint32_t n = log_head - log_tail;
        if (n > sizeof(logbuf) - start)
            n = sizeof(logbuf) - start;
        if (n > room)
            n = room;
        log_tail += tud_cdc_write(&logbuf[start], n);
    }
    tud_cdc_write_flush();
}

/* MARK: attempts */

static struct key_attempt attempt;
static unsigned attempts;
static bool answered;

static void usb_service(void)
{
    dcd_int_handler(0);
    tud_task();
    log_drain();
}

static void start_attempt(const char *why)
{
    attempts++;
    answered = false;
    key_start(&attempt, &io);
    log_line("attempt %u (%s): waiting until D+ stays low for %u ms - power the mixer on now",
         attempts, why, (unsigned)(KEY_ARM_LOW_US / 1000u));
}

static void report(enum key_event e)
{
    switch (e) {
    case KEY_EV_NONE:
        break;
    case KEY_EV_ARMED:
        log_line("D+ low: sending key 0x%x", (unsigned)KEY_VALUE);
        break;
    case KEY_EV_ANSWERED:
        answered = true;
        log_line("the chip answered after %u keys, %u ms: lines released; watching D+ for %u s",
             (unsigned)attempt.keys, (unsigned)(attempt.answer_us / 1000u), (unsigned)(KEY_WATCH_US / 1000000u));
        break;
    case KEY_EV_TIMEOUT:
        log_line("no answer in %u s (%u keys): check the wiring and the order of power-up; r tries again",
             (unsigned)(KEY_TIMEOUT_US / 1000000u), (unsigned)attempt.keys);
        break;
    case KEY_EV_WATCHED:
        if (key_dropped(&attempt)) {
            answered = false;
            log_line("the chip left the bus (D+ low for %u ms, %u low pulses): its clock calibration"
                 " probably failed - use a USB port of the computer itself, no hub",
                 (unsigned)(attempt.longest_low_us / 1000u), (unsigned)attempt.low_pulses);
        } else {
            log_line("the chip stays on the bus (%u low pulses, longest %u ms): it should be a UBOOT"
                 " mass-storage device now (jlrom list)",
                 (unsigned)attempt.low_pulses, (unsigned)(attempt.longest_low_us / 1000u));
        }
        break;
    }
}

static const char *const state_name[] = {
    [KEY_IDLE] = "idle", [KEY_ARMING] = "waiting for an empty bus", [KEY_KEYING] = "keying",
    [KEY_WATCH] = "watching after the answer",
};

static void command(char c)
{
    switch (c) {
    case 'r':
        start_attempt("r");
        break;
    case 'x':
        if (attempt.state != KEY_IDLE) {
            key_abort(&attempt, &io);
            log_line("attempt %u aborted, lines released", attempts);
        }
        break;
    case 'd':
        if (attempt.state != KEY_IDLE) {
            log_line("d: abort the attempt first (x)");
            break;
        }
        log_line("replug: both lines low for 300 ms");
        line_drive(KEY_DP, 0);
        line_drive(KEY_DM, 0);
        for (uint32_t t = now_us(); now_us() - t < 300000u;)
            usb_service();
        line_release(KEY_DP);
        line_release(KEY_DM);
        log_line("replug: lines released");
        break;
    case 's':
        log_line("usbkey %s: %s; attempt %u, %u keys; D+ %u, D- %u", USBKEY_VERSION,
             state_name[attempt.state], attempts, (unsigned)attempt.keys,
             (unsigned)line_read(KEY_DP), (unsigned)line_read(KEY_DM));
        break;
    case 'h':
    case '?':
        log_line("commands: r start an attempt, x abort, d replug (lines low 300 ms), s status, h help");
        break;
    default:
        break;
    }
}

static void led_update(void)
{
    uint32_t ms = now_us() / 1000u;
    bool on;
    switch (attempt.state) {
    case KEY_ARMING: on = ms % 500u < 250u; break;
    case KEY_KEYING: on = ms % 100u < 50u; break;
    default: on = answered || ms % 2000u < 50u; break;
    }
    REG(on ? SIO_GPIO_OUT_SET : SIO_GPIO_OUT_CLR) = 1u << PIN_LED;
}

uint32_t tusb_time_millis_api(void)
{
    return now_us() / 1000u;
}

int main(void)
{
    clocks_init();
    unreset(RESET_IO_BANK0 | RESET_PADS_BANK0 | RESET_TIMER);
    pin_init(PIN_DP);
    pin_init(PIN_DM);
    pin_init(PIN_LED);
    REG(SIO_GPIO_OE_SET) = 1u << PIN_LED;

    log_line("usbkey %s: JieLi USB key on GP%u (D+, clock) and GP%u (D-, data); h for help",
         USBKEY_VERSION, (unsigned)PIN_DP, (unsigned)PIN_DM);
    const tusb_rhport_init_t dev = { .role = TUSB_ROLE_DEVICE, .speed = TUSB_SPEED_FULL };
    tusb_init(0, &dev);
    start_attempt("power-up");

    bool was_connected = false, was_pressed = false;
    uint32_t last_button = 0;
    for (;;) {
        usb_service();
        bool connected = tud_cdc_connected();
        if (connected && !was_connected)
            log_line("terminal connected");
        was_connected = connected;
        while (tud_cdc_available())
            command((char)tud_cdc_read_char());

        report(key_poll(&attempt, &io));

        if (now_us() - last_button >= 50000u) {
            last_button = now_us();
            bool pressed = bootsel_pressed();
            if (pressed && !was_pressed)
                start_attempt("BOOTSEL");
            was_pressed = pressed;
        }
        led_update();
    }
}
