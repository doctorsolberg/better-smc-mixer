/* smclog port for host builds (tests, simulator). macOS only for now. */
#ifndef SMCLOG_HOST_H
#define SMCLOG_HOST_H

#include <stdint.h>

/* Replace the monotonic clock with a virtual one (deterministic tests and replay, D-3); NULL
 * goes back to the monotonic clock. */
void smclog_host_set_clock(uint32_t (*now_us)(void));

#endif /* SMCLOG_HOST_H */
