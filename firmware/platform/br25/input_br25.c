/*
 * The input hardware on BR25 (input_br25.h, docs/hwtest.md).
 *
 * The setup follows the stock firmware's (docs/hardware.md; static analysis, ⚠️ until the hardware
 * test confirms it):
 *
 * - SPI2 on its port A pins: clock PB6, data out PB7. Master, one data line, clock idle low, data
 *   changed on the falling edge (the 595s shift on the rising one). Each word goes out as 4 bytes
 *   by DMA from RAM, its lowest byte first.
 * - PB4: the 595 latch (storage clock), a rising edge after the rows are read.
 * - PB5: driven low once a defined word is latched: ⚠️ taken to be the 595s' output enable (/OE).
 * - PB0-PB3: the input rows, inputs with pull-ups.
 * - PA2, PA5, PC1, PC3: LED columns 0-3, outputs, low drives a column.
 *
 * One difference: the stock firmware chains the steps (each SPI2 DMA-complete interrupt shifts the
 * next word, as fast as the SPI runs). Here TIMER3 paces them at SCAN_STEP_US, so the pass rate,
 * the debounce time and the LED refresh do not depend on the SPI clock, and the share of CPU time
 * the scan takes is bounded. Nothing else in the build uses TIMER3 (tools/env/build_app.sh checks
 * the disassembly).
 *
 * While flash is written (docs/hwtest.md): the SDK's flash code masks interrupt priorities 0-6
 * (local_irq_disable(), irq_br25.h), for a sector erase tens of milliseconds. The scan interrupt
 * runs at priority 7 so that it goes on meanwhile; nothing it reaches may then be in flash: its
 * code is marked SMC_RAM_CODE (smc_ram.h) and tools/re/rampath.py checks every build. The lock it
 * shares with our tasks is irq_br25.h's (cli), since the SDK's does not hold priority 7 off.
 *
 * The scan timer is also the log's microsecond clock (metrics/usclock.h): it starts first, alone
 * (input_br25_clock_start(), so that the boot journal is timed in µs), and the scan joins it later.
 * The interrupt counts its periods, input_br25_time_us() adds the counter, and every SYNC_STEPS
 * periods the clock is compared with the SDK's millisecond clock (and jumps forward if it fell
 * behind: periods lost while the interrupt was held off). The interrupt also keeps the
 * heartbeat's window (LOG-06): its own run time, how late it starts, skipped steps, the time
 * between matrix passes, the clock's drift, the fader ADC's counts and the CPU load samples; and
 * samples the stack it runs on (metrics_app.h).
 *
 * Above the SDK's mask, the interrupt may not call the RTOS either (it may have interrupted the
 * RTOS inside its own critical section). A task that must wake for its events is woken through a
 * software interrupt at a low priority (input_br25_wake()), whose handler may.
 *
 * The fader ADC is ours too (below, "Fader ADC"), polled from the scan interrupt.
 *
 * The CPU load: at each step the interrupt notes whether the task it interrupted is the RTOS's
 * idle task ("IDLE", which sleeps in the CPU's idle instruction). The share of other samples is
 * the load. Interrupt handlers that run while the idle task is current count as idle; our own
 * handler is not sampled (its time is measured: busy_scan_us).
 */
/* The SDK first: it typedefs bool, which <stdbool.h> (through scan.h) then redefines. */
#include "system/includes.h"
#include "asm/adc_api.h"

#include "input_br25.h"
#ifdef SMC_STALL_CHECK
#include "diag_br25.h"
#endif

#include <string.h>

#include "evq.h"
#include "irq_br25.h"
#include "metrics_app.h"
#include "smc_ram.h"
#include "smclog.h"
#include "usclock.h"

#define SCAN_TIMER JL_TIMER3
#define SCAN_TIMER_IRQ IRQ_TIME3_IDX
#define SCAN_IRQ_PRIORITY 7 /* above the SDK's mask while it writes flash (the stock uses 5) */

/* 32 bits in 8 µs: well inside a 25 µs step even when the step starts late (at 2 MHz, 16 µs, about
 * 1.7% of the steps found the word still shifting and were skipped). 64 MHz / 16 at 192 MHz; the
 * stock firmware's BAUD 0x0B would be 5.3 MHz from the same source (⚠️ its source not traced),
 * and 74HC595s shift far faster. Checked on the mixer, 2026-11-23. */
#define SPI_TARGET_HZ 4000000u
#define SPI_CON_PND BIT(15)
#define SPI_CON_CLR_PND BIT(14)
#define SPI_CON_UD_FALL BIT(5) /* data changed on the falling clock edge */
#define SPI_CON_EN BIT(0)
#define IOMAP_SPI2_PORT_B BIT(16)

#define SYNC_STEPS 16u /* a matrix pass */
#define TIMER_PND BIT(15)
#define TIMER_CLR_PND BIT(14)

/* The waker: soft interrupt 0, unused by the SDK (it switches tasks with 3). Soft interrupts 60-63
 * are ILAT bits 0-3: the ROM's vPortYield raises 63 with ILAT_SET bit 3 and OS_ClrPending clears it
 * with ILAT_CLR bit 3 (cpu/br25/tools/rom.lst). ✅ Bit 4, read from cpu.h's CPU_TASK_SW (BIT(7 - a)),
 * never ran the handler on the mixer (2026-11-11: 10 asked, 0 taken). */
#define WAKE_IRQ IRQ_SOFT0_IDX
#define WAKE_IRQ_PRIORITY 1
#define WAKE_ILAT_BIT BIT(WAKE_IRQ - IRQ_SOFT0_IDX)

#define ROWS_MASK 0x0Fu /* PB0-PB3 */
#define LATCH BIT(4)    /* PB4 */

/*
 * Fader ADC. The SDK's driver (cpu/br25/adc_api.c) converts one channel every 2 ms in turn, so
 * with eight faders each would be read every 20 ms or so. Ours converts the eight in turn from the
 * scan interrupt, polling, and shares the converter with the SDK's driver, which keeps sampling
 * the battery and its reference:
 *
 * - the SDK starts a conversion only when the ADC's enable bit is clear (adc_scan()) and its
 *   handler ends each with CON = 0. Ours starts only when CON reads 0, never enables the ADC
 *   interrupt (the SDK's handler would take the result), and ends the same way;
 * - if CON no longer holds our setting while ours runs, the SDK took the ADC (it was interrupted
 *   between its check and its write): ours is dropped and counted;
 * - after each conversion the ADC stays idle for one step, so that the SDK's 2 ms poll finds it
 *   free now and then.
 *
 * The setting is the SDK's (adc_sample()): clock / 96 and BIT(3) as it sets it, but a start delay
 * of 4 × 8 ADC clocks, not its 15 × 8. The conversion time is that delay (3 µs a clock at 32 MHz):
 * ✅ on the mixer a fader is read every 3.68 ms at 15, 1.62 ms at 4, with the same noise and no
 * crosstalk from the fader before; 1 is noisier (docs/hwtest.md, "The faders measured"). The
 * interrupt counts the steps each takes (stats, heartbeat). One fader is read every
 * (steps + 1) × 8 × SCAN_STEP_US.
 */
#ifndef SMC_ADC_DELAY
#define SMC_ADC_DELAY 4 /* the start delay in 8 ADC clocks (the SDK's: 15); ADC_DELAY=n in the build */
#endif
_Static_assert(SMC_ADC_DELAY >= 1 && SMC_ADC_DELAY <= 15, "ADC start delay: 1-15");
/* CON's clock divider field: 6 is the SDK's /96. ⚠️ The codes read as /1, 6, 12, 24, 48, 72, 96,
 * 128 for 0..7 (the SDK's get_adc_div() table, sys2adc_clk_info); only 6 and 7 are used here.
 * The device build passes ADC_DIV (firmware/app/Makefile): 7, /128, for the CPU at 192 MHz, where
 * the ADC's source clock doubles to 64 MHz and /96 let the mid-scale wrong conversions grow from
 * 1.5% to 21% of a fader's samples (docs/core.md, "The CPU clock"). */
#ifndef SMC_ADC_DIV
#define SMC_ADC_DIV 6
#endif
_Static_assert(SMC_ADC_DIV == 6 || SMC_ADC_DIV == 7, "ADC divider code: 6 (/96) or 7 (/128)");
#define ADC_DIVISOR (SMC_ADC_DIV == 7 ? 128u : 96u)
#define ADC_CON_SETTING ((uint32_t)SMC_ADC_DIV | BIT(3) | ((uint32_t)SMC_ADC_DELAY << 12))
#define ADC_CON_EN BIT(4)
#define ADC_CON_IE BIT(5)
#define ADC_CON_START BIT(6) /* writing it starts a conversion; also clears the pending flag */
#define ADC_CON_PND BIT(7)
#define ADC_RES_MASK 0x3FFu  /* 10 bits */
#define ADC_TIMEOUT_STEPS (2000u / SCAN_STEP_US) /* 2 ms: a conversion that long is given up */

static const uint8_t row_pins[SCAN_ROWS] = {IO_PORTB_00, IO_PORTB_01, IO_PORTB_02, IO_PORTB_03};
static const uint8_t led_col_pins[SCAN_LED_COLS] = {IO_PORTA_02, IO_PORTA_05, IO_PORTC_01, IO_PORTC_03};

/* Faders 1-8, left to right (docs/hwtest.md, "Faders": the run of 2026-11-11 found each on one of
 * the candidate ADC inputs, in the reverse of their pin order; bottom reads 0). Read in the
 * interrupt, so in RAM. */
static SMC_RAM_DATA const uint8_t fader_ch[INPUT_BR25_FADER_CHANNELS] = {
    AD_CH_PC5, AD_CH_PC4, AD_CH_PC2, AD_CH_PA6, AD_CH_PA4, AD_CH_PA3, AD_CH_PA1, AD_CH_PA0,
};
static const uint8_t fader_pins[INPUT_BR25_FADER_CHANNELS] = {
    IO_PORTC_05, IO_PORTC_04, IO_PORTC_02, IO_PORTA_06, IO_PORTA_04, IO_PORTA_03, IO_PORTA_01, IO_PORTA_00,
};

/* The ROM's RTOS (cpu/br25 maskrom stubs): callable from the interrupt, never in flash. */
extern void *xTaskGetHandle(const char *name);
extern void *xTaskGetCurrentTaskHandle(void);
/* Where the RTOS keeps its running task, as the SDK's context switch (pvPortSwitch, in RAM) finds
 * it: CurrentTCB holds the address of the ROM's own variable (seen on the mixer, run 16). The ROM's
 * xTaskGetCurrentTaskHandle() reads it between local_irq_disable() and local_irq_enable(), the two
 * ROM functions the SDK patches through hardware breakpoints (diag_br25.c): the scan's load sample
 * took about 8 us of each 50 us step at 48 MHz (docs/bringup.md, runs 15 and 16). */
extern void *volatile *CurrentTCB;
extern void local_irq_disable_rom(void); /* the ROM's, through the SDK's stub table */
extern void local_irq_enable_rom(void);
extern unsigned long jiffies_msec(void); /* the SDK's, in RAM */

static struct scan *scan;
static volatile uint32_t word __attribute__((aligned(4))); /* the DMA source */
static bool shifting;                                       /* a word was handed to the DMA */
static int8_t led_driven = -1;
static struct input_br25_stats stats;
static struct usclock clock;
static volatile bool clock_on;
static uint32_t clock_steps; /* timer periods, for the clock's syncs */
static void *idle_task;      /* the RTOS's idle task, for the load samples */
static void *volatile *tcb_at; /* the running task's variable, once it agreed with the RTOS */
static void (*waker)(void);  /* input_br25_set_waker() */
static struct {              /* the fader ADC */
    bool on, busy;
    uint8_t fader;    /* being converted, or next */
    uint8_t steps;    /* steps the conversion has taken */
    uint32_t con;     /* our setting while it runs */
} adc;
static struct { /* the heartbeat's window (input_br25_take_window) */
    uint32_t steps, skipped, ticks_sum, ticks_max, late_max;
    struct span pass_us;
    uint32_t load_samples, load_busy;
    uint32_t adc_samples, adc_deferred, adc_lost, adc_timeouts, adc_steps_max;
} win;
static uint32_t last_pass_us; /* across windows */
static bool have_pass;

/* Lost scan periods: the timer has one pending flag, so a step that starts more than a period
 * late loses a period (and the µs clock falls behind). Each step places the start of its period
 * on the CPU's tick timer (TICK_CNT, the RTOS tick's, a 10 ms period, counting on with interrupts
 * off): its count now less the scan timer's. When two starts lie more than 1.5 periods apart, a
 * period was lost; the step after it records the interrupted address (reti) and task, which is
 * where interrupts came back on: the code that held them off (docs/hwtest.md, "The CPU's time"). */
#define GAP_SITES 10u
struct gap_site {
    uint32_t pc;
    void *task;
    uint16_t n, max_us;
};
static struct {
    uint32_t last, per_us, scale, thr, period; /* scale: tick counts per scan timer count */
    bool armed;
} tick;
static struct {
    uint32_t n, lost, max_us, other; /* other: gaps whose site found no room */
    struct gap_site at[GAP_SITES];
} gaps;

SMC_RAM_CODE static void gap_note(uint32_t dt, uint32_t lost, uint32_t pc, void *task)
{
    uint32_t us = dt / tick.per_us;
    if (us > 0xFFFFu)
        us = 0xFFFFu;
    gaps.n++;
    gaps.lost += lost;
    if (us > gaps.max_us)
        gaps.max_us = us;
    for (unsigned i = 0; i < GAP_SITES; i++) {
        struct gap_site *g = &gaps.at[i];
        if (g->n && (g->pc != pc || g->task != task))
            continue;
        g->pc = pc;
        g->task = task;
        g->n++;
        if (us > g->max_us)
            g->max_us = (uint16_t)us;
        return;
    }
    gaps.other++;
}

#ifdef SMC_SCAN_PROFILE
/* SCAN_PROFILE=1 builds (firmware/app/Makefile): where the scan handler's time goes, phase by phase,
 * in timer ticks, logged with each heartbeat window (input_br25_take_window). ENTRY is the timer's
 * count when the handler starts: the interrupt's entry, plus any wait behind another handler. */
enum { PROF_ENTRY, PROF_LOAD, PROF_CLOCK, PROF_PREP, PROF_STEP, PROF_OUT, PROF_ADC, PROF_N };
static struct {
    uint32_t sum[PROF_N], max[PROF_N];
    uint32_t n, idle_n, entry_idle; /* handler entries; those that interrupted the idle task */
} prof;
#define PROF_START(t0)                                                                             \
    uint32_t prof_t = (t0);                                                                        \
    prof.n++;                                                                                      \
    prof.sum[PROF_ENTRY] += prof_t;                                                                \
    if (prof_t > prof.max[PROF_ENTRY])                                                             \
        prof.max[PROF_ENTRY] = prof_t
#define PROF_MARK(i)                                                                               \
    do {                                                                                           \
        uint32_t t_ = SCAN_TIMER->CNT;                                                             \
        uint32_t d_ = t_ >= prof_t ? t_ - prof_t : t_ + SCAN_TIMER->PRD + 1u - prof_t;             \
        prof.sum[i] += d_;                                                                         \
        if (d_ > prof.max[i])                                                                      \
            prof.max[i] = d_;                                                                      \
        prof_t = t_;                                                                               \
    } while (0)
#else
#define PROF_START(t0) (void)0
#define PROF_MARK(i) (void)0
#endif

SMC_RAM_CODE uint32_t input_port_lock(void)
{
    return irq_br25_save();
}

SMC_RAM_CODE void input_port_unlock(uint32_t state)
{
    irq_br25_restore(state);
}

/* All columns off (high), then the one driven low; -1 leaves them off. No table, no switch: a
 * jump table would be read from flash. */
SMC_RAM_CODE static void led_columns(int8_t col)
{
    JL_PORTA->OUT |= BIT(2) | BIT(5);
    JL_PORTC->OUT |= BIT(1) | BIT(3);
#ifdef SMC_LED_DARK
    (void)col; /* LED_DARK=1, a diagnostic: no LED column is ever driven (docs/hardware.md, "The faint glow of 13 LEDs") */
    return;
#endif
    if (col == 0)
        JL_PORTA->OUT &= ~BIT(2);
    else if (col == 1)
        JL_PORTA->OUT &= ~BIT(5);
    else if (col == 2)
        JL_PORTC->OUT &= ~BIT(1);
    else if (col == 3)
        JL_PORTC->OUT &= ~BIT(3);
}

SMC_RAM_CODE static void shift(uint32_t w)
{
    word = w;
    JL_SPI2->CON |= SPI_CON_CLR_PND;
    JL_SPI2->ADR = (uint32_t)(uintptr_t)&word;
    JL_SPI2->CNT = 4; /* starts the transfer */
    shifting = true;
}

___interrupt static void wake_isr(void)
{
    q32DSP(0)->ILAT_CLR = WAKE_ILAT_BIT;
    stats.wake_taken++;
    uint32_t t0 = input_br25_time_us();
    if (waker)
        waker();
    uint32_t d = input_br25_time_us() - t0;
    stats.wake_us_sum += d;
    if (d > stats.wake_us_max)
        stats.wake_us_max = d;
}

void input_br25_set_waker(void (*fn)(void))
{
    waker = fn;
    request_irq(WAKE_IRQ, WAKE_IRQ_PRIORITY, wake_isr, 0);
}

SMC_RAM_CODE void input_br25_wake(void)
{
    stats.wake_asked++;
    q32DSP(0)->ILAT_SET = WAKE_ILAT_BIT;
}

/* Not inlined: tools/env/build_app.sh finds the TIMER3 users by function name. */
SMC_RAM_CODE __attribute__((noinline)) uint32_t input_br25_time_us(void)
{
    if (!clock_on)
        return (uint32_t)jiffies_msec() * 1000u;
    uint32_t l = irq_br25_save();
    uint32_t c1 = SCAN_TIMER->CNT;
    bool pending = (SCAN_TIMER->CON & TIMER_PND) != 0;
    uint32_t c2 = SCAN_TIMER->CNT;
    uint32_t t = usclock_read(&clock, c1, pending, c2);
    irq_br25_restore(l);
    return t;
}

/* One step of the fader ADC (above): finish ours, or start the next if the ADC is free. */
SMC_RAM_CODE static void adc_step(uint32_t now)
{
    uint32_t con = JL_ADC->CON;
    if (adc.busy) {
        if ((con & ~(uint32_t)(ADC_CON_PND | ADC_CON_START)) != adc.con) {
            win.adc_lost++; /* the SDK took the ADC: its conversion now, its handler ends it */
            adc.busy = false;
            return;
        }
        adc.steps++;
        if (!(con & ADC_CON_PND)) {
            if (adc.steps >= ADC_TIMEOUT_STEPS) {
                win.adc_timeouts++;
                JL_ADC->CON = 0;
                adc.busy = false;
                adc.fader = (uint8_t)((adc.fader + 1u) % INPUT_BR25_FADER_CHANNELS);
            }
            return;
        }
        uint16_t v = (uint16_t)(JL_ADC->RES & ADC_RES_MASK);
        JL_ADC->CON = ADC_CON_START; /* clears the pending flag, as the SDK's handler does */
        JL_ADC->CON = 0;
        adc.busy = false;
        win.adc_samples++;
        if (adc.steps > win.adc_steps_max)
            win.adc_steps_max = adc.steps;
        scan_fader(scan, now, adc.fader, v);
        adc.fader = (uint8_t)((adc.fader + 1u) % INPUT_BR25_FADER_CHANNELS);
        return; /* the ADC stays idle for a step */
    }
    if (con) {
        win.adc_deferred++; /* the SDK's conversion */
        return;
    }
    adc.con = ADC_CON_SETTING | ((uint32_t)fader_ch[adc.fader] & 0xFu) << 8 | ADC_CON_EN;
    adc.steps = 0;
    adc.busy = true;
    JL_ADC->CON = adc.con & ~(uint32_t)ADC_CON_EN;
    JL_ADC->CON = adc.con;
    JL_ADC->CON = adc.con | ADC_CON_START;
}

/* One scan step (scan.h): read the rows, latch, shift the next word, drive the LED column; then
 * the fader ADC. */
SMC_RAM_CODE ___interrupt static void scan_isr(void)
{
    uint32_t t0 = SCAN_TIMER->CNT;
    uint32_t tk = TICK_CNT;
    char here;
    PROF_START(t0);
    void *cur = NULL;
    uint32_t lost = 0; /* periods that ended while this step could not start */
    if (idle_task) { /* first: the task this step interrupted */
        cur = tcb_at ? *tcb_at : xTaskGetCurrentTaskHandle();
        int idle = cur == idle_task;
#ifdef SMC_SCAN_PROFILE
        if (idle) {
            prof.idle_n++;
            prof.entry_idle += t0;
        }
#endif
        win.load_samples++;
        if (!idle)
            win.load_busy++;
#ifdef SMC_STALL_CHECK
        uint32_t pc;
        __asm__ volatile("%0 = reti" : "=r"(pc));
        diag_br25_stall_step(idle, pc, cur);
#endif
    }
    if (tick.per_us) {
        uint32_t prd = TICK_PRD + 1u, start = tk - t0 * tick.scale; /* this period's start */
        if (start >= prd)
            start += prd; /* it lies before the tick timer's wrap */
        uint32_t dt = start >= tick.last ? start - tick.last : start + prd - tick.last;
        tick.last = start;
        if (dt > tick.thr && tick.armed) {
            uint32_t pc;
            __asm__ volatile("%0 = reti" : "=r"(pc));
            lost = (dt + tick.period / 2u) / tick.period - 1u;
            gap_note(dt, lost, pc, cur);
        }
        tick.armed = true;
    }
    PROF_MARK(PROF_LOAD);
    uint32_t l = irq_br25_save(); /* a reader sees the flag cleared and the period counted, or neither */
    SCAN_TIMER->CON |= TIMER_CLR_PND;
    usclock_period(&clock);
    for (uint32_t i = 0; i < lost && i < 200u; i++) /* the µs clock keeps the lost ones (at most 10 ms) */
        usclock_period(&clock);
    if (++clock_steps % SYNC_STEPS == 0) /* a sample a pass is enough for the deepest point */
        metrics_app_stack_sample((uintptr_t)&here); /* counts if interrupts run on the task's stack */
    irq_br25_restore(l);
    if (clock_steps % SYNC_STEPS == 0)
        usclock_sync(&clock, (uint32_t)jiffies_msec() * 1000u);
    PROF_MARK(PROF_CLOCK);
    if (!scan)
        return; /* the clock alone, until input_br25_start() */
    stats.steps++;
    win.steps++;
    if (t0 > stats.late_max)
        stats.late_max = t0;
    if (t0 > win.late_max)
        win.late_max = t0;
    uint32_t now = input_br25_time_us();
    PROF_MARK(PROF_PREP);
    if (shifting && !(JL_SPI2->CON & SPI_CON_PND)) {
        stats.spi_busy++; /* latching now would latch half a word: skip this step */
        win.skipped++;
    } else {
        uint32_t passes = scan->stats.passes;
        uint8_t rows = (uint8_t)(JL_PORTB->IN & ROWS_MASK);
        uint32_t next = scan_step(scan, now, rows);
        PROF_MARK(PROF_STEP);
        JL_PORTB->OUT |= LATCH;
        shift(next);
        JL_PORTB->OUT &= ~LATCH;
        if (scan->led_drive != led_driven) {
            led_driven = scan->led_drive;
            led_columns(led_driven);
        }
        scan_pass(scan, now); /* the word is on its way: the pass's evaluation delays nothing */
        if (scan->stats.passes != passes) {
            if (have_pass)
                span_add(&win.pass_us, now - last_pass_us);
            last_pass_us = now;
            have_pass = true;
        }
    }
    PROF_MARK(PROF_OUT);
    if (adc.on)
        adc_step(now);
    PROF_MARK(PROF_ADC);
    uint32_t t1 = SCAN_TIMER->CNT;
    uint32_t took = t1 >= t0 ? t1 - t0 : t1 + SCAN_TIMER->PRD + 1u - t0;
    if (took > stats.isr_max)
        stats.isr_max = took;
    win.ticks_sum += took;
    if (took > win.ticks_max)
        win.ticks_max = took;
}

static void pin_in(uint8_t pin, bool pull_up)
{
    gpio_set_die(pin, 1);
    gpio_set_direction(pin, 1);
    gpio_set_pull_up(pin, pull_up);
    gpio_set_pull_down(pin, 0);
}

static void pin_out(uint8_t pin, int level)
{
    gpio_set_die(pin, 1);
    gpio_set_pull_up(pin, 0);
    gpio_set_pull_down(pin, 0);
    gpio_direction_output(pin, level);
}

static void adc_setup(void)
{
    for (unsigned i = 0; i < INPUT_BR25_FADER_CHANNELS; i++) {
        /* analog: no digital input, no pulls */
        gpio_set_direction(fader_pins[i], 1);
        gpio_set_die(fader_pins[i], 0);
        gpio_set_pull_up(fader_pins[i], 0);
        gpio_set_pull_down(fader_pins[i], 0);
    }
    stats.adc_channels = INPUT_BR25_FADER_CHANNELS;
}

/* Not inlined: tools/env/build_app.sh finds the TIMER3 users by function name. */
__attribute__((noinline)) void input_br25_clock_start(void)
{
    if (clock_on)
        return;
    uint32_t timer_hz = (uint32_t)clk_get("timer");
    stats.ticks_per_us = timer_hz / 1000000u;
    idle_task = xTaskGetHandle("IDLE");
    void *running = xTaskGetCurrentTaskHandle();
    void *volatile *at = CurrentTCB;
    bool in_ram = (uintptr_t)at >= 0x10000u && (uintptr_t)at < 0x40000u && !((uintptr_t)at & 3u);
    tcb_at = running != NULL && in_ram && *at == running ? at : NULL;
    SCAN_TIMER->CON = TIMER_CLR_PND;
    SCAN_TIMER->CNT = 0;
    /* The counter runs 0..PRD: a period is PRD + 1 counts (✅ run 25: with PRD = 1200 the µs clock
     * fell behind by 1/1201). */
    SCAN_TIMER->PRD = stats.ticks_per_us * SCAN_STEP_US - 1u;
    uint32_t tick_prd = TICK_PRD + 1u; /* counts 0..PRD: 10 ms, the RTOS tick (as jiffies_msec() counts it) */
    uint32_t per_us = tick_prd / 10000u;
    tick.scale = stats.ticks_per_us ? per_us / stats.ticks_per_us : 0u;
    tick.period = per_us * SCAN_STEP_US;
    tick.thr = tick.period * 3u / 2u;
    if (tick.scale && tick.scale * stats.ticks_per_us == per_us)
        tick.per_us = per_us; /* last: 0 leaves the check off */
    request_irq(SCAN_TIMER_IRQ, SCAN_IRQ_PRIORITY, scan_isr, 0);
    /* The clock takes over from the millisecond one where it is, with nothing reading in between. */
    uint32_t l = irq_br25_save();
    usclock_init(&clock, (uint32_t)jiffies_msec() * 1000u, SCAN_STEP_US, stats.ticks_per_us);
    SCAN_TIMER->CON = BIT(0) | BIT(3) | TIMER_CLR_PND; /* count on the timer clock (as the SDK does) */
    clock_on = true;
    irq_br25_restore(l);
    LOG_INFO(input, "clock: system %u Hz; scan timer at %u Hz, a period every %u us, priority %u",
             (uint32_t)clk_get("sys"), timer_hz, SCAN_STEP_US, SCAN_IRQ_PRIORITY);
    if (!idle_task)
        LOG_WARN(input, "the RTOS has no task IDLE: no CPU load samples");
    LOG_INFO(input, "lost periods: the tick timer counts %u in 10 ms, %u a scan timer count; check %s", tick_prd,
             tick.scale, tick.per_us ? "on" : "off (the two clocks are not whole multiples)");
    LOG_INFO(input, "running task: read %s (CurrentTCB 0x%08x points at 0x%08x; the RTOS's 0x%08x)",
             tcb_at ? "directly" : "through the ROM", (uint32_t)(uintptr_t)at,
             in_ram ? (uint32_t)(uintptr_t)*at : 0u, (uint32_t)(uintptr_t)running);
#ifdef SMC_SCAN_PROFILE
    /* The least of 16 of each: no interrupt in between. */
    uint32_t reads = UINT32_MAX, sdk = UINT32_MAX, rom = UINT32_MAX, get = UINT32_MAX;
    for (unsigned i = 0; i < 16; i++) {
        uint32_t a = SCAN_TIMER->CNT, b = SCAN_TIMER->CNT;
        if (b >= a && b - a < reads)
            reads = b - a;
        a = SCAN_TIMER->CNT;
        local_irq_disable(); /* the SDK's, in RAM */
        local_irq_enable();
        b = SCAN_TIMER->CNT;
        if (b >= a && b - a < sdk)
            sdk = b - a;
        a = SCAN_TIMER->CNT;
        local_irq_disable_rom(); /* the ROM's, where the breakpoints are */
        local_irq_enable_rom();
        b = SCAN_TIMER->CNT;
        if (b >= a && b - a < rom)
            rom = b - a;
        a = SCAN_TIMER->CNT;
        (void)xTaskGetCurrentTaskHandle();
        b = SCAN_TIMER->CNT;
        if (b >= a && b - a < get)
            get = b - a;
    }
    uint32_t tpu = stats.ticks_per_us;
    LOG_INFO(input, "scan profile, ns: a lock and unlock %u (the SDK's) and %u (the ROM's); "
             "xTaskGetCurrentTaskHandle() %u; two timer reads %u", sdk * 1000u / tpu,
             rom * 1000u / tpu, get * 1000u / tpu, reads * 1000u / tpu);
#endif
}

__attribute__((noinline)) void input_br25_start(struct scan *s)
{
    for (unsigned i = 0; i < SCAN_ROWS; i++)
        pin_in(row_pins[i], true);
    for (unsigned i = 0; i < SCAN_LED_COLS; i++)
        pin_out(led_col_pins[i], 1); /* off */
    pin_out(IO_PORTB_04, 0);          /* latch */
    pin_out(IO_PORTB_06, 0);          /* SPI2 clock */
    pin_out(IO_PORTB_07, 0);          /* SPI2 data */

    JL_IOMAP->CON1 &= ~IOMAP_SPI2_PORT_B; /* SPI2 on its port A pins */
    uint32_t spi_clk = (uint32_t)clk_get("spi");
    uint32_t baud = spi_clk / SPI_TARGET_HZ;
    baud = baud ? baud - 1 : 0;
    stats.spi_hz = spi_clk / (baud + 1);
    JL_SPI2->CON = SPI_CON_UD_FALL | SPI_CON_CLR_PND; /* master, 1 line, out, clock idle low */
    JL_SPI2->BAUD = baud;
    JL_SPI2->CON |= SPI_CON_EN;

    /* A defined word on the outputs before they are enabled: nothing selected, LEDs off. */
    shift(0xFFFFFFFFu);
    unsigned wait = 100000;
    while (!(JL_SPI2->CON & SPI_CON_PND) && --wait)
        ;
    if (!wait)
        LOG_ERROR(input, "SPI2: the first word did not finish shifting");
    shifting = false;
    JL_SPI2->CON |= SPI_CON_CLR_PND;
    JL_PORTB->OUT |= LATCH;
    JL_PORTB->OUT &= ~LATCH;
#ifdef SMC_LED_OE_OFF
    pin_out(IO_PORTB_05, 1); /* LED_DARK=2, a diagnostic: PB5 (⚠️ /OE) high, the 595s' outputs off */
#else
    pin_out(IO_PORTB_05, 0); /* ⚠️ the 595s' /OE */
#endif

    adc_setup();

    input_br25_clock_start();
    uint32_t l = irq_br25_save(); /* the interrupt scans from its next period */
    scan = s;
    adc.on = true;
    irq_br25_restore(l);

    LOG_INFO(input, "scan: step %u us, timer %u Hz, SPI2 %u Hz (from %u Hz), %u fader channels "
             "(ADC clock %u Hz / %u, start delay %u x 8 clocks)", SCAN_STEP_US,
             stats.ticks_per_us * 1000000u, stats.spi_hz, spi_clk, stats.adc_channels,
             (uint32_t)clk_get("adc"), ADC_DIVISOR, (unsigned)SMC_ADC_DELAY);
}

void input_br25_get_stats(struct input_br25_stats *st)
{
    uint32_t l = input_port_lock();
    *st = stats;
    input_port_unlock(l);
}

/* A task's name, from the table the SDK starts them from (app_main.c) and the RTOS's own. */
static const char *task_name(void *task)
{
    extern const struct task_info task_info_table[];
    static const char *const rtos[] = {"IDLE", "Tmr Svc"};
    if (!task)
        return "(none)";
    for (const struct task_info *t = task_info_table; t->name; t++)
        if (xTaskGetHandle(t->name) == task)
            return t->name;
    for (unsigned i = 0; i < sizeof rtos / sizeof rtos[0]; i++)
        if (xTaskGetHandle(rtos[i]) == task)
            return rtos[i];
    return "?";
}

void input_br25_take_window(struct metrics_window *w)
{
    uint32_t l = input_port_lock();
    uint32_t steps = win.steps, skipped = win.skipped, sum = win.ticks_sum, max = win.ticks_max,
             late = win.late_max;
    w->scan_pass_us = win.pass_us;
    w->load_samples = win.load_samples;
    w->load_busy = win.load_busy;
    w->adc_samples = win.adc_samples;
    w->adc_deferred = win.adc_deferred;
    w->adc_lost = win.adc_lost;
    w->adc_timeouts = win.adc_timeouts;
    w->adc_steps_max = win.adc_steps_max;
    memset(&win, 0, sizeof win);
    uint32_t gap_n = gaps.n, gap_lost = gaps.lost, gap_max = gaps.max_us, gap_other = gaps.other;
    struct gap_site gap_at[GAP_SITES];
    memcpy(gap_at, gaps.at, sizeof gap_at);
    memset(&gaps, 0, sizeof gaps);
    usclock_take_window(&clock, &w->clock_resyncs, &w->clock_drift_min_us, &w->clock_drift_max_us);
    input_port_unlock(l);

    uint32_t tpu = stats.ticks_per_us ? stats.ticks_per_us : 1u, handled = steps - skipped;
    w->have_scan = true;
    w->have_clock = clock_on;
    w->have_load = idle_task != NULL;
    w->have_adc = adc.on;
    w->scan_steps = steps;
    w->scan_skipped = skipped;
    w->busy_scan_us = sum / tpu;
    w->scan_step_avg_ns = handled ? sum / handled * 1000u / tpu : 0;
    w->scan_step_max_ns = max * 1000u / tpu;
    w->scan_late_max_us = late / tpu;
    if (gap_n) {
        LOG_INFO(input, "scan periods lost: %u in %u gaps, the longest gap %u us; %u gaps at other sites", gap_lost,
                 gap_n, gap_max, gap_other);
        for (unsigned i = 0; i < GAP_SITES && gap_at[i].n; i++)
            LOG_INFO(input, "scan gap site: pc 0x%08x, task 0x%08x %s: %u gaps, the longest %u us",
                     gap_at[i].pc, (uint32_t)(uintptr_t)gap_at[i].task, task_name(gap_at[i].task),
                     gap_at[i].n, gap_at[i].max_us);
    }
#ifdef SMC_SCAN_PROFILE
    uint32_t ps[PROF_N], mx[PROF_N];
    l = input_port_lock();
    for (unsigned i = 0; i < PROF_N; i++) {
        ps[i] = prof.sum[i];
        mx[i] = prof.max[i];
    }
    uint32_t pn = prof.n, pidle_n = prof.idle_n, pentry_idle = prof.entry_idle,
             psum_entry = prof.sum[PROF_ENTRY];
    memset(&prof, 0, sizeof prof);
    input_port_unlock(l);
    for (unsigned i = 0; i < PROF_N; i++) { /* ns: per step (sum fits: 10 s of ticks times 10) */
        ps[i] = steps ? ps[i] * 10u / steps * 100u / tpu : 0;
        mx[i] = mx[i] * 1000u / tpu;
    }
    LOG_INFO(input, "scan profile, avg ns a step: entry %u, load %u, clock %u, prep %u, step %u, out %u, adc %u",
             ps[0], ps[1], ps[2], ps[3], ps[4], ps[5], ps[6]);
    LOG_INFO(input, "scan profile, max ns: entry %u, load %u, clock %u, prep %u, step %u, out %u, adc %u",
             mx[0], mx[1], mx[2], mx[3], mx[4], mx[5], mx[6]);
    uint32_t busy_n = pn - pidle_n;
    LOG_INFO(input, "scan profile, entry avg: %u ns over %u steps from the idle task, %u ns over %u others",
             pidle_n ? pentry_idle / pidle_n * 1000u / tpu : 0u, pidle_n,
             busy_n ? (psum_entry - pentry_idle) / busy_n * 1000u / tpu : 0u, busy_n);
#endif
}
