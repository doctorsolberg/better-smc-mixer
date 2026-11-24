/*
 * Diagnostics across a reset on BR25 (diag_br25.h, firmware/diag/diag.h; docs/logging.md, "Boot
 * journal").
 *
 * - What a reset keeps: struct diag_keep in section .smc_keep, NOLOAD, between the end of the RAM
 *   code (data_code_limit_end, where the SDK's CPU write limit ends) and the heap
 *   (firmware/app/sdk_ld_patch.py). The startup code clears only .bss, so a reset keeps the block
 *   as long as nothing before us writes there. ⚠️ Whether the mask ROM and the factory uboot leave
 *   this RAM alone is not known yet; the magics and checks make a lost block read as "nothing kept".
 * - The reset cause: P3_RST_SRC (P33 register 0x12) in bits 0-7 and P3_PMU_CON1 (0x31) in bits
 *   8-15, read here after the SDK's own read in main(), which names the P3_RST_SRC bits (bit 0
 *   VDDIO POR, 1 VDDIO LVD, 2 WDT, 3 VCM, 4 PPINR, 5 SYS RESET, 6 SOFT RESET) and, when P3_PMU_CON1
 *   bit 7 is set, reports "power return" and does not read P3_RST_SRC; otherwise it sets
 *   P3_PMU_CON1 bit 6. ⚠️ Whether the bits are sticky across resets, or cleared by a read, is not
 *   known: the raw value is logged with the names.
 * - Exceptions: our handler replaces the SDK's on IRQ 1 (exceptions). The SDK's handler in this
 *   build saves the frame and resets the chip (its exception_analyze() is reduced to chip_reset()),
 *   so nothing is lost by replacing it. IRQ 0 (emulator exceptions) stays the SDK's: its
 *   breakpoint_init() puts breakpoint_isr there and sets three hardware breakpoints on the ROM's
 *   local_irq_disable, local_irq_enable and cpu_irq_disabled, which breakpoint_isr sends to RAM
 *   replacements. Taking IRQ 0 over turned every one of those calls into a crash (✅ on the device,
 *   docs/bringup.md, "Step 4", runs 5 to 7). ⚠️ A real emulator exception under breakpoint_isr
 *   matches no breakpoint and returns to the same instruction, again and again, until the watchdog
 *   (from the disassembly). Ours saves the same frame, stops the CPU's
 *   trace (ETM) so that it keeps the program counters from before the exception, writes the crash
 *   record (the frame, the debug unit's messages, the trace, the top of the task's stack, the task's
 *   name) and resets the chip the same way (chip_reset(), in ROM). Like the SDK's it runs from RAM
 *   (flash may be busy when an exception comes) and calls only RAM and ROM code: jiffies_msec()
 *   (RAM), os_current_task_rom() and chip_reset() (ROM), memcpy/memset if the compiler emits them
 *   (ROM). ⚠️ The order of the frame (diag.h, enum diag_reg) is read from the ROM's own handlers.
 *
 * - Resets on request (the link's RESET, docs/link.md): a soft reset (chip_reset()); a fault, a
 *   misaligned word load (emu_misalign_enable() in asm/debug.h, on by default), which comes to
 *   our handler on IRQ 1, not to IRQ 0, with EMU_MSG bit 0 set, a few instructions late: after
 *   diag_br25_fault() has returned; a hang with interrupts off until the watchdog resets the chip.
 *   The SDK's setup (inlined into main() in this build) sets the watchdog to 8 s and enables it;
 *   only the idle hook clears it (from the image's disassembly). ✅ All three on the mixer,
 *   2026-11-11 (firstrun.py, image 30): each comes back with the RAM kept and the right cause.
 *
 * Nothing here writes flash (C-6).
 */
/* The SDK first: it typedefs bool, which <stdbool.h> (through diag.h) then redefines. */
#include "system/includes.h"
#include "asm/power/p33.h"

#include "diag_br25.h"
#include "bootmark_br25.h"

#include <stddef.h>
#include <string.h>

#include "diag.h"
#include "link.h"
#include "smclog.h"
#include "smclog_port.h"

#define RAM_CODE __attribute__((section(".volatile_ram_code")))
#define EXCEPTION_PRIORITY 2 /* the SDK's (cpu/br25/setup.c) */
#define RAM_LO 0x10000u      /* RAM_LIMIT_L in the SDK's linker script */
#define RAM_HI 0x31AE0u      /* RAM_LIMIT_H: the update area and the ROM's RAM above it */
#define ETM_CON_ADDR 0x10F1C0u /* bit 0: the trace on (the SDK's debug_init() sets it) */

_Static_assert(q32DSP_sfr_base + __builtin_offsetof(JL_TypeDef_q32DSP, ETM_CON) == ETM_CON_ADDR,
               "the entry code's ETM_CON address");
_Static_assert(__builtin_offsetof(struct diag_crash, reg) % 4 == 0, "the crash record's registers"); /* the SDK's offsetof is not a constant */

extern void chip_reset(void);                 /* ROM (maskrom_stubs.ld): P33 soft reset */
extern const char *os_current_task_rom(void); /* ROM: the current task's name, from its TCB */
extern unsigned long jiffies_msec(void);      /* the SDK, in RAM */
extern uint32_t _IRQ_MEM_ADDR[];              /* the vector table (maskrom_stubs.ld; ISR_BASE) */

static struct diag_keep keep __attribute__((section(".smc_keep"), aligned(32)));
static struct diag_boot boot;
static uint32_t tag; /* smclog_build_tag(), for the handler: the build ID is in flash */
static char reset_text[96];
static volatile uint8_t reset_due; /* a requested reset: how + 1, 0 for none */
static uint32_t reset_asked_ms;
volatile uint32_t diag_br25_fault_value; /* what the fault test's load read, if it did not trap */

#define RESET_DELAY_MS 100 /* the reply's time to reach the host */

/* --- the exception handler -------------------------------------------------------------------- */

/* From the entries below, never returns. frame: r0-r15, then RETI .. SSP (enum diag_reg). */
RAM_CODE __attribute__((used, noinline, noreturn)) void diag_br25_capture(uint32_t *frame,
                                                                         uint32_t cause)
{
    static volatile uint32_t entered;
    struct diag_crash *c = &keep.crash;

    if (entered++) /* a fault while writing the record */
        chip_reset();
    c->magic = 0;
    c->tag = tag;
    c->boot = keep.boots;
    c->t_ms = (uint32_t)jiffies_msec();
    c->cause = cause;
    c->frame = (uint32_t)(uintptr_t)frame;
    for (unsigned i = 0; i < DIAG_FRAME_REGS; i++)
        c->reg[i] = frame[i];
    c->reg[DIAG_EMU_MSG] = q32DSP(0)->EMU_MSG;
    c->reg[DIAG_DEBUG_MSG] = JL_DEBUG->MSG;
    c->reg[DIAG_PRP_MMU_MSG] = JL_DEBUG->PRP_MMU_MSG;
    c->reg[DIAG_PRP_WR_LIMIT_MSG] = JL_DEBUG->PRP_WR_LIMIT_MSG;
    c->trace[0] = q32DSP(0)->ETM_PC0;
    c->trace[1] = q32DSP(0)->ETM_PC1;
    c->trace[2] = q32DSP(0)->ETM_PC2;
    c->trace[3] = q32DSP(0)->ETM_PC3;

    uint32_t usp = frame[DIAG_USP];
    c->stack_at = 0;
    if (!(usp & 3u) && usp >= RAM_LO && usp + 4u * DIAG_STACK <= RAM_HI) {
        const uint32_t *s = (const uint32_t *)(uintptr_t)usp;
        c->stack_at = usp;
        for (unsigned i = 0; i < DIAG_STACK; i++)
            c->stack[i] = s[i];
    }

    const char *name = os_current_task_rom();
    unsigned n = 0;
    if ((uintptr_t)name >= RAM_LO && (uintptr_t)name + DIAG_TASK <= RAM_HI)
        while (n < DIAG_TASK - 1 && name[n] >= ' ' && name[n] < 0x7F) {
            c->task[n] = name[n];
            n++;
        }
    while (n < DIAG_TASK)
        c->task[n++] = 0;

    diag_crash_seal(c);
    chip_reset();
    for (;;)
        ;
}

/* The entries: the frame as the SDK's handler saves it, the trace stopped, then the capture with the
 * frame's address and the interrupt index. */
#define EXCEPTION_ENTRY(name, cause)                                                               \
    __attribute__((naked, used, section(".volatile_ram_code"))) static void name(void)            \
    {                                                                                              \
        __asm__ volatile("[--sp] = {ssp, usp, icfg, psr, rets, retx, rete, reti}\n\t"              \
                         "[--sp] = {r15-r0}\n\t"                                                   \
                         "r1 = 0x10f1c0\n\t" /* ETM_CON_ADDR */                                    \
                         "r2 = [r1+0x0]\n\t"                                                       \
                         "r2 &= 0xfffffffe\n\t" /* the trace off */                               \
                         "[r1+0x0] = r2\n\t"                                                       \
                         "r0 = sp\n\t"                                                             \
                         "r1 = " #cause "\n\t"                                                     \
                         "call diag_br25_capture\n\t");                                            \
    }
EXCEPTION_ENTRY(exception_cpu, 1)
#ifdef SMC_EMU_CHAIN
/* EMU_CHAIN=1 builds only: on the device it hung the start in usb_app_start (docs/bringup.md,
 * "Step 4", run 9). */
EXCEPTION_ENTRY(exception_emu_fault, 0)

/* --- IRQ 0: the SDK's breakpoints passed on, the rest a crash --------------------------------- */

/* The SDK's breakpoint_init() sets three hardware breakpoints (registers from BP_REGS_ADDR, each
 * the address | 1) on ROM functions, and its breakpoint_isr sends each hit to a RAM replacement
 * through its table bp_tab: (ROM address, replacement) pairs. Both are local symbols of the SDK's
 * library, so diag_br25_emu_take() finds the table near breakpoint_isr and checks it against the
 * hardware before ours replaces it (docs/logging.md, "The exception handler (BR25)"). */
#define BP_REGS_ADDR 0x10F088u
#define BP_COUNT 3
#define BP_SEARCH 0x400u /* bytes after breakpoint_isr to look for its table (0x96 in this SDK) */
static uint32_t bp_from[BP_COUNT], bp_to[BP_COUNT];
static const char *emu_state = "not tried";

/* From exception_emu with RETE: the replacement for a breakpoint's address, 0 for anything else. */
RAM_CODE __attribute__((used, noinline)) uint32_t diag_br25_bp_target(uint32_t rete)
{
    for (unsigned i = 0; i < BP_COUNT; i++)
        if (bp_from[i] == rete)
            return bp_to[i];
    return 0;
}

/* breakpoint_isr's own steps (from the image's disassembly), with our lookup; anything that is not
 * a breakpoint goes, with every register as it came, to the crash entry. */
__attribute__((naked, used, section(".volatile_ram_code"))) static void exception_emu(void)
{
    __asm__ volatile("[--sp] = {r15-r0}\n\t"
                     "[--sp] = {icfg, psr, rets}\n\t"
                     "r0 = reti\n\t"
                     "[--sp] = {r0}\n\t"
                     "r0 = rete\n\t"
                     "call diag_br25_bp_target\n\t"
                     "if (r0 == 0) goto .Lemu_fault\n\t"
                     "rete = r0\n\t"
                     "{r0} = [sp++]\n\t"
                     "reti = r0\n\t"
                     "{icfg, psr, rets} = [sp++]\n\t"
                     "{r15-r0} = [sp++]\n\t"
                     "rte\n\t"
                     ".Lemu_fault:\n\t"
                     "{r0} = [sp++]\n\t"
                     "reti = r0\n\t"
                     "{icfg, psr, rets} = [sp++]\n\t"
                     "{r15-r0} = [sp++]\n\t"
                     "goto exception_emu_fault\n\t");
}

static bool in_ram(uint32_t a)
{
    return a >= RAM_LO && a < RAM_HI;
}

/* Ours on IRQ 0 if the SDK's table is found and matches the hardware; the SDK's otherwise. */
static void diag_br25_emu_take(void)
{
    uint32_t isr = _IRQ_MEM_ADDR[IRQ_EMUEXCPT_IDX];
    volatile const uint32_t *hw = (volatile const uint32_t *)BP_REGS_ADDR;
    uint32_t want[BP_COUNT];
    if (!in_ram(isr)) {
        emu_state = "IRQ 0's handler is not in RAM: left to the SDK";
        return;
    }
    for (unsigned i = 0; i < BP_COUNT; i++) {
        if (!(hw[i] & 1u)) {
            emu_state = "a hardware breakpoint is off: IRQ 0 left to the SDK";
            return;
        }
        want[i] = hw[i] & ~1u;
    }
    for (uint32_t a = (isr + 3u) & ~3u; a + 8u * BP_COUNT <= isr + BP_SEARCH && in_ram(a + 8u * BP_COUNT); a += 4u) {
        const uint32_t *t = (const uint32_t *)(uintptr_t)a;
        unsigned i = 0;
        while (i < BP_COUNT && t[2 * i] == want[i] && in_ram(t[2 * i + 1]))
            i++;
        if (i < BP_COUNT)
            continue;
        for (i = 0; i < BP_COUNT; i++) {
            bp_from[i] = t[2 * i];
            bp_to[i] = t[2 * i + 1];
        }
        __asm__ volatile("" ::: "memory");
        _IRQ_MEM_ADDR[IRQ_EMUEXCPT_IDX] = (uint32_t)(uintptr_t)exception_emu;
        emu_state = "ours, the SDK's breakpoints passed on";
        return;
    }
    emu_state = "the SDK's breakpoint table not found: IRQ 0 left to the SDK";
}
#endif
_Static_assert(DIAG_CAUSE_EMU_EXCEPTION == 0 && DIAG_CAUSE_EXCEPTION == 1, "the entries' causes");

#ifdef SMC_BOOTMARK
/*
 * The boot-mark build only (docs/bringup.md, "Step 4: boot marks"), at the start of the journal,
 * into the VM:
 * - item 47: the debug unit now and the interrupts' state, 8 words: EMU_MSG, JL_DEBUG MSG,
 *   PRP_MMU_MSG, PRP_WR_LIMIT_MSG, EMU_CON, the vector slots of IRQ 0 and 1 (what the SDK put
 *   there), IPCON0 (the enable and priority of IRQ 0..7, as request_irq() writes them);
 * - item 46, when the kept block holds a valid crash record (the last start's): cause, RETI, RETE,
 *   RETX, RETS, EMU_MSG, DEBUG_MSG, PRP_MMU_MSG, PRP_WR_LIMIT_MSG, the 4 trace PCs (13 words).
 */
#define IPCON0_ADDR 0x10F100u    /* request_irq()'s priority register for IRQ 0..7 */

static void bootmark_debug_state(void)
{
    uint32_t now[8] = {q32DSP(0)->EMU_MSG,         JL_DEBUG->MSG,
                       JL_DEBUG->PRP_MMU_MSG,      JL_DEBUG->PRP_WR_LIMIT_MSG,
                       q32DSP(0)->EMU_CON,         _IRQ_MEM_ADDR[IRQ_EMUEXCPT_IDX],
                       _IRQ_MEM_ADDR[IRQ_EXCEPTION_IDX], *(volatile uint32_t *)IPCON0_ADDR};
    syscfg_write(47, now, sizeof now);
    const struct diag_crash *c = &keep.crash;
    if (!diag_crash_valid(c))
        return;
    uint32_t r[13] = {c->cause,
                      c->reg[DIAG_RETI],
                      c->reg[DIAG_RETE],
                      c->reg[DIAG_RETX],
                      c->reg[DIAG_RETS],
                      c->reg[DIAG_EMU_MSG],
                      c->reg[DIAG_DEBUG_MSG],
                      c->reg[DIAG_PRP_MMU_MSG],
                      c->reg[DIAG_PRP_WR_LIMIT_MSG],
                      c->trace[0],
                      c->trace[1],
                      c->trace[2],
                      c->trace[3]};
    syscfg_write(46, r, sizeof r);
    if (c->cause == DIAG_CAUSE_STALL)
        syscfg_write(45, (void *)c->stack, 7 * sizeof c->stack[0]); /* item 45: tasks, USB IRQs, idle ran */
}
#else
#define bootmark_debug_state() do {} while (0)
#endif

#ifdef SMC_STALL_CHECK
/* --- the stall check (diag_br25.h) ------------------------------------------------------------ */

#define STALL_STEPS 80000u /* 4 s of 50 us scan steps */
#define STALL_SAMPLES (1 + DIAG_TRACE)
static uint32_t stall_quiet, stall_n, stall_armed, stall_usb0;
static uint32_t stall_pc[STALL_SAMPLES], stall_task[STALL_SAMPLES];
volatile uint32_t diag_br25_usb_irqs; /* usb_app.c's usb_isr(): its entries */

RAM_CODE void diag_br25_stall_step(int idle, uint32_t pc, void *task)
{
    if (idle) {
        stall_armed = 1; /* the idle task has run at least once (run 11: it never did) */
        stall_quiet = 0;
        stall_n = 0;
        return;
    }
    /* Counted from the scan's first step too: a start that never reaches the idle task is caught
     * 4 s after the scan began, before the watchdog's 8 s (run 11 ended with no record). */
    if (++stall_quiet == 1)
        stall_usb0 = diag_br25_usb_irqs; /* the window's start */
    if (stall_quiet < STALL_STEPS)
        return;
    stall_task[stall_n] = (uint32_t)(uintptr_t)task;
    stall_pc[stall_n++] = pc;
    if (stall_n < STALL_SAMPLES)
        return;
    struct diag_crash *c = &keep.crash;
    c->magic = 0;
    c->tag = tag;
    c->boot = keep.boots;
    c->t_ms = (uint32_t)jiffies_msec();
    c->cause = DIAG_CAUSE_STALL;
    c->frame = (uint32_t)(uintptr_t)xTaskGetCurrentTaskHandle(); /* the busy task's TCB */
    for (unsigned i = 0; i < DIAG_REGS; i++)
        c->reg[i] = 0;
    c->reg[DIAG_RETI] = stall_pc[0];
    for (unsigned i = 0; i < DIAG_TRACE; i++)
        c->trace[i] = stall_pc[1 + i];
    c->stack_at = 0; /* not a stack: the samples' tasks, then the USB interrupts in the 4 s */
    for (unsigned i = 0; i < DIAG_STACK; i++)
        c->stack[i] = i < STALL_SAMPLES ? stall_task[i] : 0;
    c->stack[STALL_SAMPLES] = diag_br25_usb_irqs - stall_usb0;
    c->stack[STALL_SAMPLES + 1] = stall_armed; /* 0: the idle task never ran since the scan began */
    for (unsigned i = 0; i < DIAG_TASK; i++) /* the name would need the RTOS: the TCB is in frame */
        c->task[i] = 0;
    diag_crash_seal(c);
    chip_reset();
}
#endif

/* --- the journal ------------------------------------------------------------------------------ */

static const char *reset_names(uint32_t raw)
{
    static const char *const names[8] = {"power-on",     "low voltage", "watchdog",   "VCM",
                                         "pin reset",    "system reset", "soft reset", "bit 7"};
    size_t len = 0;
    reset_text[0] = 0;
    for (unsigned b = 0; b < 9; b++) {
        const char *name = b < 8 ? names[b] : "power return";
        if (!(raw & (b < 8 ? 1u << b : 1u << 15)))
            continue;
        size_t n = strlen(name);
        if (len + n + 3 > sizeof reset_text)
            break;
        if (len) {
            memcpy(reset_text + len, ", ", 2);
            len += 2;
        }
        memcpy(reset_text + len, name, n + 1);
        len += n;
    }
    return len ? reset_text : "none";
}

void diag_br25_begin(const char *version)
{
    tag = smclog_build_tag();
    bootmark(BOOTMARK_DIAG_TAG);
    bootmark_debug_state();
#ifdef SMC_EMU_CHAIN
    diag_br25_emu_take();
#endif
    request_irq(IRQ_EXCEPTION_IDX, EXCEPTION_PRIORITY, exception_cpu, 0);
    bootmark(BOOTMARK_DIAG_IRQ_CPU);
    uint32_t raw = p33_rx_1byte(P3_RST_SRC) | (uint32_t)p33_rx_1byte(P3_PMU_CON1) << 8;
    uint32_t now = smclog_port_time_us();
    bootmark(BOOTMARK_DIAG_TIME);
    const char *names = reset_names(raw);
    bootmark(BOOTMARK_DIAG_NAMES);
    diag_boot_begin(&boot, &keep, now, raw, names, version);
}

#ifdef SMC_BOOTMARK
/* diag.c's DIAG_STEP() in the boot-mark build (firmware/app/Makefile): 0x70..0x77. */
void bootmark_step(unsigned char step)
{
    bootmark(step);
}
#endif

void diag_br25_mark(const char *what)
{
    diag_boot_mark(&boot, what, smclog_port_time_us());
}

extern const struct task_info task_info_table[];
extern void *xTaskGetHandle(const char *name);

/* The tasks' handles, to name the ones a crash record holds (the stall record's samples). In the
 * boot-mark build also into the VM, item 44: IDLE's, then task_info_table's in its order. */
static void log_tasks(void)
{
    uint32_t h[12];
    unsigned n = 0;
    h[n++] = (uint32_t)(uintptr_t)xTaskGetHandle("IDLE");
    LOG_INFO(boot, "task IDLE: handle 0x%08x", h[0]);
    for (const struct task_info *t = task_info_table; t->name && n < 12; t++, n++) {
        h[n] = (uint32_t)(uintptr_t)xTaskGetHandle(t->name);
        LOG_INFO(boot, "task %s: handle 0x%08x", t->name, h[n]);
    }
#ifdef SMC_BOOTMARK
    syscfg_write(44, h, (unsigned short)(n * sizeof h[0]));
#endif
}

void diag_br25_done(void)
{
    log_tasks();
#ifdef SMC_EMU_CHAIN
    LOG_INFO(boot, "IRQ 0: %s; breakpoints 0x%08x 0x%08x 0x%08x", emu_state, bp_from[0], bp_from[1],
             bp_from[2]);
#endif
    diag_boot_done(&boot, smclog_port_time_us());
}

void diag_br25_log(void *ctx)
{
    (void)ctx;
    diag_boot_log(&boot);
}

void diag_br25_heartbeat(uint32_t seq)
{
    diag_keep_heartbeat(&keep, seq, (uint32_t)jiffies_msec());
}

static const char *const reset_how[] = {"soft reset", "fault test", "watchdog test"};
_Static_assert(sizeof reset_how / sizeof reset_how[0] == LINK_RESET_MAX + 1, "RESET's modes");

void diag_br25_reset(unsigned how)
{
    if (how > LINK_RESET_MAX)
        return;
    LOG_WARN(boot, "reset on request: %s in %u ms", reset_how[how], RESET_DELAY_MS);
    reset_asked_ms = (uint32_t)jiffies_msec();
    reset_due = (uint8_t)(how + 1);
}

__attribute__((noinline, used)) void diag_br25_fault(void)
{
    static uint32_t words[2];
    volatile uintptr_t at = (uintptr_t)words + 1; /* not known to the compiler: a plain load */
    diag_br25_fault_value = *(volatile const uint32_t *)at;
}

void diag_br25_service(void)
{
    if (!reset_due || (uint32_t)jiffies_msec() - reset_asked_ms < RESET_DELAY_MS)
        return;
    unsigned how = reset_due - 1u;
    reset_due = 0;
    switch (how) {
    case LINK_RESET_SOFT:
        chip_reset();
        break;
    case LINK_RESET_FAULT:
        diag_br25_fault();
        LOG_ERROR(boot, "fault test: the misaligned load did not trap (read 0x%08x)",
                  diag_br25_fault_value);
        break;
    case LINK_RESET_WATCHDOG:
        local_irq_disable();
        for (;;)
            ;
    }
}
