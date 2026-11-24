/*
 * Boot marks (docs/bringup.md, "Step 4": the first power-on stopped before the scan, with no USB
 * and no log to read). A diagnostic build only: BOOTMARK=1 tools/env/build_app.sh defines
 * SMC_BOOTMARK; in every other build bootmark() is nothing.
 *
 * Each mark appends one record to the SDK's settings store (VM): item BOOTMARK_ITEM, one byte, the
 * stage. The VM sits above the SFC window and is stored as is, so a raw read in the ROM mode
 * (jlrom read) shows the records in the order they were written: how far each start got, and
 * whether the chip started more than once (the watchdog). It works once the SDK has opened the VM:
 * syscfg_tools_init(), a platform initcall that app_task_init() runs before board_init() (seen in
 * the disassembly of the hwtest build). Before that, vm_write() returns an error and stores
 * nothing. vm_write() also skips a record equal to the item's stored value: a mark repeated on a
 * later start is not stored again.
 *
 * This writes flash on every start: never in a build that is not for this diagnosis.
 */
#ifndef BOOTMARK_BR25_H
#define BOOTMARK_BR25_H

/* The stages, in the order of the start (board_smc_mixer.c, app_main.c, hwtest.c). */
enum bootmark_stage {
    BOOTMARK_BOARD = 0x0a,        /* board_init(): entry */
    BOOTMARK_POWER_INIT = 0x0b,   /* board_power_init() returned */
    BOOTMARK_VBG = 0x0c,          /* adc_vbg_init() returned */
    BOOTMARK_ADC = 0x0d,          /* adc_init() returned */
    BOOTMARK_CFG = 0x50,          /* the copy of cfg_file_parse() (board_smc_mixer.c): entry */
    BOOTMARK_CFG_NAME = 0x51,     /* the Bluetooth name read */
    BOOTMARK_CFG_NAME_LOG = 0x52, /* ... and logged */
    BOOTMARK_CFG_RF = 0x53,       /* the RF power read */
    BOOTMARK_CFG_PWR = 0x54,      /* bt_max_pwr_set() returned */
    BOOTMARK_CFG_LOGS = 0x55,     /* the fixed settings logged */
    BOOTMARK_CFG_MAC = 0x56,      /* the MAC read (BTIF) */
    BOOTMARK_CFG_MAC_NEW = 0x57,  /* the MAC was invalid: a random one is written */
    BOOTMARK_CFG_MAC_2 = 0x58,    /* the second MAC read */
    BOOTMARK_CFG_MAC_DUMP = 0x59, /* the MAC logged (hex dump) */
    BOOTMARK_CFG_LRC = 0x5a,      /* the LRC settings read */
    BOOTMARK_CFG_LRC_SET = 0x5b,  /* ... and applied */
    BOOTMARK_CFG_DONE = 0x5c,     /* lp_winsize_init() returned */
    BOOTMARK_VM_OPEN = 0x10,      /* board_init(): cfg_file_parse() returned */
    BOOTMARK_DEVICES = 0x11,      /* devices_init() returned */
    BOOTMARK_BOARD_DEVICES = 0x12,
    BOOTMARK_POWER_MODE = 0x13,   /* before power_set_mode() */
    BOOTMARK_BOARD_DONE = 0x14,   /* board_init() returns */
    BOOTMARK_APP_MAIN = 0x20,     /* app_main(): the SDK started its tasks */
    BOOTMARK_CLOCK = 0x21,        /* input_br25_clock_start() returned */
    BOOTMARK_DIAG = 0x22,         /* diag_br25_begin() returned */
    BOOTMARK_DIAG_TAG = 0x60,     /* diag_br25_begin(): smclog_build_tag() returned */
    /* 0x61 (our IRQ 0 handler installed) and 0x65 (skipped) until image 7: IRQ 0 is the SDK's */
    BOOTMARK_DIAG_IRQ_CPU = 0x62, /* ... and for the CPU exception */
    BOOTMARK_DIAG_TIME = 0x63,    /* the reset cause and the µs clock read */
    BOOTMARK_DIAG_NAMES = 0x64,   /* the cause named */
    /* 0x70..0x77: diag.c, diag_boot_begin(), DIAG_STEP(): its fields set, the log's recent ring
     * attached, the kept block checked, the crash record taken, the block sealed, then each of
     * log_head(), log_prev(), log_crash() returned (log_recent() returning is BOOTMARK_DIAG) */
    BOOTMARK_LOG = 0x23,          /* smclog_init() returned */
    BOOTMARK_VARIANT = 0x24,      /* variant_start() returned */
    BOOTMARK_USB = 0x25,          /* usb_app_start() returned */
    BOOTMARK_DONE = 0x26,         /* diag_br25_done() returned */
    BOOTMARK_HW_START = 0x30,     /* hwtest variant_start(): entry */
    BOOTMARK_HW_SCAN_INIT = 0x31, /* the queue, injection and scan initialized */
    BOOTMARK_HW_SCAN = 0x32,      /* input_br25_start() returned */
    BOOTMARK_HW_TIMERS = 0x33,    /* the timers added */
    /* 0x40, 0x41 (the LED walk's first two steps) until image 6; image 7 writes no mark once the
     * start is done: no flash write while the walk runs. */
};

#ifdef SMC_BOOTMARK
/* The SDK's settings store (include_lib/system/syscfg_id.h): user items are 1..49. */
int syscfg_write(unsigned short item_id, void *buf, unsigned short len);
#define BOOTMARK_ITEM 49
static inline void bootmark(unsigned char stage)
{
    syscfg_write(BOOTMARK_ITEM, &stage, 1);
}
#else
static inline void bootmark(unsigned char stage)
{
    (void)stage;
}
#endif

#endif /* BOOTMARK_BR25_H */
