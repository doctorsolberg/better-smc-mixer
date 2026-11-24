/*
 * Diagnostics across a reset on BR25 (firmware/diag/diag.h; docs/logging.md, "Boot journal"): the
 * block a reset keeps, the reset cause, the exception handler that writes the crash record, and the
 * boot journal of this run. One instance; the functions run in task context.
 */
#ifndef DIAG_BR25_H
#define DIAG_BR25_H

#include <stdint.h>

/* First thing in app_main(), once the log's clock runs (input_br25_clock_start()): installs the
 * exception handler, takes what the previous run left and writes the journal's first records. */
void diag_br25_begin(const char *version);
/* A part of the start is done (what: a string literal). */
void diag_br25_mark(const char *what);
/* The start is done. */
void diag_br25_done(void);
/* The journal again (the link's BOOT_LOG; struct link_ops.boot_log). */
void diag_br25_log(void *ctx);
/* Each metrics heartbeat (metrics_app.h): the next boot reports this run's last one. */
void diag_br25_heartbeat(uint32_t seq);
/* The link's RESET (how: link.h, enum link_reset): logged now, done by diag_br25_service() about
 * 100 ms later, once the reply has reached the host. The caller checks that a debug mode is
 * allowed (LOG-34). */
void diag_br25_reset(unsigned how);
/* From the USB task at every wake-up (at least every 10 ms): a reset that is due. */
void diag_br25_service(void);
/* The fault test (LINK_RESET_FAULT): a misaligned word load, which the CPU does not allow (an
 * emulator exception, IRQ 0, which stays the SDK's breakpoint_isr: ⚠️ a hang until the watchdog,
 * not our crash record, docs/logging.md). Returns only if it did not trap. Not static: `firstrun.py` finds it
 * by name among the crash record's addresses. */
void diag_br25_fault(void);

#ifdef SMC_STALL_CHECK
/* From the scan interrupt at every step (input_br25.c), in RAM: whether the step interrupted the idle
 * task, and the program counter it interrupted (RETI). After 4 s without the idle task, counted
 * from the scan's first step or the idle task's last run (the SDK's idle hook is what clears the
 * watchdog, 8 s), the next 5 counters go into a crash record (DIAG_CAUSE_STALL: RETI and the trace;
 * the busy task's handle as the frame; stack[0..4] the samples' task handles, stack[5] the USB
 * interrupts in the 4 s, stack[6] 1 if the idle task ran at all) and the chip resets
 * (chip_reset(), ROM: the one call outside RAM), before the watchdog would. */
void diag_br25_stall_step(int idle, uint32_t pc, void *task);
/* usb_isr()'s entries (usb_app.c), for the stall record's count of USB interrupts. */
extern volatile uint32_t diag_br25_usb_irqs;
#endif

#endif /* DIAG_BR25_H */
