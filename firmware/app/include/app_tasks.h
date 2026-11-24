/*
 * Our tasks: their names and stack sizes, for task_info_table (app_main.c), the tasks themselves
 * and the stack metric (metrics_app.h).
 */
#ifndef APP_TASKS_H
#define APP_TASKS_H

#define APP_TASK_USB "smc_usb" /* USB, the command link and USB MIDI (usb/usb_app.c) */
#define APP_TASK_USB_STACK 512 /* words */
#define APP_TASK_CTL "smc_ctl" /* the controller: events, the core, LEDs (controller.c) */
#define APP_TASK_CTL_STACK 768 /* words */
#define APP_TASK_BLE "smc_ble" /* BLE-MIDI: the sender (ble/ble_br25.c, BLE=1 only) */
#define APP_TASK_BLE_STACK 256 /* words */

#endif /* APP_TASKS_H */
