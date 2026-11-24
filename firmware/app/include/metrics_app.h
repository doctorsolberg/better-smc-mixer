/*
 * The heartbeat's BR25 part (SPEC LOG-06, docs/logging.md, "Metrics"), for both variants: the
 * window of the scan and the clock (input_br25.h), of USB (usb_app.h), the heap and our tasks'
 * stacks. The variant adds its own part (the controller: ctl_take_window()) and calls
 * metrics_log().
 */
#ifndef METRICS_APP_H
#define METRICS_APP_H

#include <stdint.h>

#include "metrics.h"

#define METRICS_APP_MS 10000 /* the heartbeat's period */

/* From a task's function, first thing: its stack is watched for the stack metric (metrics.h,
 * stack_mark). name: as in task_info_table; stack_words: its stack size there. At most
 * METRICS_STACKS tasks. */
void metrics_app_task_start(const char *name, unsigned stack_words);

/* With interrupts off (the log's lock, the scan interrupt once a pass of 16 steps): sp, the address of a local, counts for
 * the watched stack that holds it. */
void metrics_app_stack_sample(uintptr_t sp);

/* A heartbeat's start (one context at a time): metrics_begin() and the parts above into w. */
void metrics_app_begin(struct metrics_window *w);

#endif /* METRICS_APP_H */
