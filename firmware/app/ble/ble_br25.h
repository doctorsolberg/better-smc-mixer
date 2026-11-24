/*
 * BLE-MIDI on the SDK's BLE stack (docs/ble.md). Built with BLE=1 only (firmware/app/Makefile);
 * without it ble_stub.c gives the same calls, which do nothing.
 */
#ifndef BLE_BR25_H
#define BLE_BR25_H

#include <stdbool.h>
#include <stdint.h>

#include "bleq.h"
#include "metrics.h"

/* What the variant does with BLE MIDI (controller.c). */
struct ble_br25_midi {
    struct bleq *out;                 /* the core's output for BLE (filled by the control task) */
    void (*in)(const uint8_t msg[3]); /* a channel message received (the BT stack's task) */
    void (*room)(void);               /* the BLE queue has room again (bleq_room_due()) */
    void (*link)(bool up);            /* a central connected (true) or left (the BT stack's task) */
};

/* Before ble_br25_start(): the variant's BLE MIDI (kept; NULL: none). */
void ble_br25_set_midi(const struct ble_br25_midi *m);

/* Starts the BLE controller and host and the sender's task; advertising begins when the stack
 * reports it is up. Call once, from task context, after smclog_init(). */
void ble_br25_start(void);

/* Any task: Bluetooth on (advertising, the default) or off (no advertising, a central that is
 * connected is let go). The sender's task applies it; before the stack is up, the stack's start
 * does. */
void ble_br25_enable(bool on);

struct sxpipe;

/* Before ble_br25_start(): the command link over BLE MIDI. Returns the pipe its SysEx goes
 * through (link/sxpipe.h), which the USB task serves (usb_app_set_ble_link); wake wakes that task.
 * Without BLE=1: NULL. */
struct sxpipe *ble_br25_link_pipe(void (*wake)(void));

/* Any task: messages wait in the BLE queue: wake the sender. */
void ble_br25_wake(void);

/* The heartbeat (control task): the BLE window into w (lat_ble, the sender's and the receiver's
 * counters), then a new one. */
void ble_br25_take_window(struct metrics_window *w);

/* The statistics since start to the log. */
void ble_br25_log_stats(void);

#endif /* BLE_BR25_H */
