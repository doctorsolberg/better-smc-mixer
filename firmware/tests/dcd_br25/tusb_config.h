/*
 * TinyUSB configuration for the BR25 driver's host test (test_dcd_br25.c): device mode, all five
 * of the controller's endpoints, as the application will use them.
 */
#ifndef TUSB_CONFIG_H
#define TUSB_CONFIG_H

#define CFG_TUSB_MCU               OPT_MCU_NONE
#define CFG_TUSB_OS                OPT_OS_NONE
#define CFG_TUSB_DEBUG             0

#define TUP_DCD_ENDPOINT_MAX       5
#define CFG_TUD_ENABLED            1
#define CFG_TUD_MAX_SPEED          OPT_MODE_FULL_SPEED
#define CFG_TUD_ENDPOINT0_SIZE     64

#endif /* TUSB_CONFIG_H */
