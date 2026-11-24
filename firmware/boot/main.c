/*
 * SMC-Mixer DFU bootloader: our code, no SDK (SPEC D-1, C-4). Stage 1 skeleton: it is built and
 * checked, but not run on the device, and it cannot write flash.
 *
 * Stage 2 (SPEC §3.4, FR-BOOT-01, NFR-06): stay in DFU when Shift + Play are held at power-on,
 * when the application is missing or fails its CRC check, or after 4 crashes in a row within 3 s
 * of starting it; otherwise start the application. Now it always stays, and every DFU download is
 * refused: there is no flash driver, and nothing is written to the device before recovery is
 * rehearsed (C-6).
 *
 * The bootloader is polled: the main loop runs the USB controller's handler, no interrupts are
 * enabled.
 */
#include "smclog.h"
#include "tusb.h"
#include "device/dcd.h"

/* Why the bootloader stays in DFU instead of starting the application. */
enum boot_stay {
    STAY_KEYS,       /* Shift + Play held at power-on (FR-BOOT-01) */
    STAY_NO_APP,     /* no application, or its CRC does not match */
    STAY_CRASH_LOOP, /* 4 crashes in a row within 3 s of starting it (NFR-06) */
};

static const char *const stay_names[] = {
    [STAY_KEYS]       = "Shift + Play",
    [STAY_NO_APP]     = "no valid application",
    [STAY_CRASH_LOOP] = "crash loop",
};

static enum boot_stay boot_decide(void)
{
    /* Stage 1: no key scan, no application header and no crash records yet. */
    return STAY_NO_APP;
}

void boot_main(void)
{
    smclog_init();
    LOG_INFO(boot, "bootloader start (stage 1 skeleton)");

    enum boot_stay stay = boot_decide();
    LOG_INFO(boot, "staying in DFU: %s", stay_names[stay]);

    const tusb_rhport_init_t usb = { .role = TUSB_ROLE_DEVICE, .speed = TUSB_SPEED_FULL };
    if (!tusb_init(0, &usb))
        LOG_ERROR(usb, "USB init failed, DFU is not available");
    /* Stage 2: blink the reason (FR-BOOT-04). */

    for (;;) {
        dcd_int_handler(0);
        tud_task();
    }
}

void tud_mount_cb(void)
{
    LOG_INFO(usb, "configured by the host");
}

void tud_umount_cb(void)
{
    LOG_INFO(usb, "deconfigured");
}

/* Poll timeout the host waits before GETSTATUS in dfuDNBUSY and dfuMANIFEST (ms). */
uint32_t tud_dfu_get_timeout_cb(uint8_t alt, uint8_t state)
{
    (void)alt;
    (void)state;
    return 1;
}

void tud_dfu_download_cb(uint8_t alt, uint16_t block_num, const uint8_t *data, uint16_t length)
{
    (void)data;
    LOG_WARN(boot, "DFU alt %u block %u (%u bytes) refused: no flash writes in stage 1", alt,
             block_num, length);
    tud_dfu_finish_flashing(DFU_STATUS_ERR_WRITE);
}

void tud_dfu_manifest_cb(uint8_t alt)
{
    LOG_WARN(boot, "DFU alt %u manifest refused: nothing was written", alt);
    tud_dfu_finish_flashing(DFU_STATUS_ERR_FIRMWARE);
}

void tud_dfu_abort_cb(uint8_t alt)
{
    LOG_INFO(boot, "DFU alt %u aborted by the host", alt);
}
