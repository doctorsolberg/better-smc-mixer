/*
 * SMC-Mixer application on the JieLi SDK.
 *
 * The SDK starts its tasks from task_info_table and calls app_main() in the app_core task. The
 * SDK's example-app framework (start_app, intents) is not used. app_main() starts the log's
 * microsecond clock and the boot journal (diag_br25.h, LOG-11: the reset cause, what the previous
 * run left, then how long each part of the start takes), the log, the build's variant (variant.h:
 * the controller or the hardware test; each keeps its metrics heartbeat, metrics_app.h), then USB
 * with the command link and USB MIDI (usb/usb_app.c, docs/link.md). Log records stay in the ring
 * until the host subscribes over USB CDC; when it is full the newest are counted as lost.
 */
#include "system/includes.h"
#include "app_config.h"
#include "app_main.h"

#include "app_tasks.h"
#include "ble_br25.h"
#include "app_version.h"
#include "bootmark_br25.h"
#include "diag_br25.h"
#include "input_br25.h"
#include "smclog.h"
#include "usb_app.h"
#include "variant.h"

/* name, priority, core, stack size (words), queue size: the SDK's tasks without audio and update */
const struct task_info task_info_table[] = {
    {"app_core", 1, 0, 640, 128},
    {"sys_event", 7, 0, 256, 0},
    {"btctrler", 4, 0, 512, 256},
    {"btencry", 1, 0, 512, 128},
    {"btstack", 3, 0, 768, 256},
    {"systimer", 7, 0, 128, 0},
    /* USB above the control task: at the same priority the RTOS shared the CPU between them in
     * 10 ms slices, and MIDI packets waited up to a slice while the control task drained a fader
     * sweep (docs/core.md, "Task priorities"). The USB task's work per wake is short. */
    {APP_TASK_USB, 3, 0, APP_TASK_USB_STACK, 0},
    {APP_TASK_CTL, 2, 0, APP_TASK_CTL_STACK, 0},
    /* BLE=1: the BLE-MIDI sender, as the BT stack's own task and USB (created only with BLE) */
    {APP_TASK_BLE, 3, 0, APP_TASK_BLE_STACK, 0},
    {NULL, 0, 0, 0, 0},
};

/* Used by the SDK's power and user configuration modules. */
APP_VAR app_var;

void app_main(void)
{
    bootmark(BOOTMARK_APP_MAIN);
    app_var.auto_off_time = TCFG_AUTO_SHUT_DOWN_TIME;
#ifdef SMC_SYS_CLOCK_HZ
    /* The board starts the CPU at TCFG_CLOCK_SYS_HZ (96 MHz, board_smc_mixer_cfg.h); this sets
     * the running clock, 192 MHz by default (SYS_CLOCK_MHZ, firmware/app/Makefile; docs/core.md,
     * "The CPU clock"), as the stock firmware does at its application's start. Before the scan
     * timer starts: its clock and the SPI's are read from the clock tree. */
    clk_set("sys", SMC_SYS_CLOCK_HZ);
#endif
    input_br25_clock_start(); /* first: the journal's times in µs */
    bootmark(BOOTMARK_CLOCK);
    diag_br25_begin(APP_VERSION);
    bootmark(BOOTMARK_DIAG);
    smclog_init();
    bootmark(BOOTMARK_LOG);
    diag_br25_mark("log");
    const struct variant *v = variant_start();
    bootmark(BOOTMARK_VARIANT);
    usb_app_start(v->link, v->usb_product, v->usb_serial, &v->midi);
    bootmark(BOOTMARK_USB);
    diag_br25_mark("usb");
#ifdef SMC_BLE
    ble_br25_start();
    diag_br25_mark("ble");
#endif
    diag_br25_done();
    bootmark(BOOTMARK_DONE);
}

/* The system asks this when no task has a timeout pending: 1 = sleep until an event. */
int eSystemConfirmStopStatus(void)
{
    return 1;
}

/* newlib's errno for the SDK libraries (as in the SDK's app_main.c). */
__attribute__((used)) int *__errno(void)
{
    static int err;
    return &err;
}
