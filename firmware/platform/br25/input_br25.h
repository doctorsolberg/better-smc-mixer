/*
 * The input hardware of the SMC-Mixer on BR25 (docs/hwtest.md): the scan timer, SPI2 to the four
 * 74HC595s, the latch, the input rows, the LED column pins and the fader ADC, all driven from the
 * scan interrupt (priority 7, in RAM: it runs while the SDK writes flash). It drives the portable
 * scan layer (input/scan.h) and provides its lock (input/evq.h; irq_br25.h).
 */
#ifndef INPUT_BR25_H
#define INPUT_BR25_H

#include <stdint.h>

#include "metrics.h"
#include "scan.h"

/* The ADC inputs sampled as faders 0-7, provisional (docs/hwtest.md): the eight ADC-capable pins
 * the matrix, the LEDs and USB leave free. The hardware test's raw records tell which pin follows
 * which fader. The scan interrupt converts them in turn and hands each value to the scan. */
#define INPUT_BR25_FADER_CHANNELS 8

struct input_br25_stats {
    uint32_t steps;          /* scan interrupts */
    uint32_t spi_busy;       /* steps skipped: the last word was still being shifted */
    uint32_t late_max;       /* timer ticks from the timer period to the handler, the most seen */
    uint32_t isr_max;        /* timer ticks the handler took, the most seen */
    uint32_t ticks_per_us;   /* timer clock, for the two above */
    uint32_t spi_hz;         /* the 595 shift clock */
    uint8_t adc_channels;    /* fader channels sampled (of 8) */
    uint32_t wake_asked;     /* input_br25_wake() calls */
    uint32_t wake_taken;     /* runs of the wake interrupt's handler */
    uint32_t wake_us_sum, wake_us_max; /* the handler's time, the control task's wake included */
};

/* Starts the scan timer as the log's microsecond clock, without scanning yet: first thing at
 * start, so that the boot journal times the rest in µs (diag_br25.h). Task context, once. */
void input_br25_clock_start(void);

/* Sets up the pins, SPI2 and the fader inputs, and starts scanning into s on the scan timer
 * (started here if input_br25_clock_start() has not run), the faders included. Call once, from
 * task context, after scan_init(). */
void input_br25_start(struct scan *s);

void input_br25_get_stats(struct input_br25_stats *st);

/* Waking a task for the scan's events. The scan interrupt may not call the RTOS (it runs above the
 * SDK's mask), so the scan's sink calls input_br25_wake(): it raises a software interrupt at a low
 * priority whose handler calls fn, which may post a semaphore. Set before input_br25_start(),
 * from task context. */
void input_br25_set_waker(void (*fn)(void));
void input_br25_wake(void); /* any context; in RAM */

/* The log clock (smclog_port_br25.c), any context: µs from the scan timer (metrics/usclock.h) once
 * it runs, the SDK's millisecond clock × 1000 before. */
uint32_t input_br25_time_us(void);

/* The heartbeat (LOG-06): the scan's, the clock's, the fader ADC's and the CPU load's window into
 * w (have_scan, have_clock, have_adc, have_load, busy_scan_us and their fields), then a new
 * window. */
void input_br25_take_window(struct metrics_window *w);

#endif /* INPUT_BR25_H */
