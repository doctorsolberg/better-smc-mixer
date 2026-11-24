/*
 * What smclog needs from the platform. Implementations: firmware/port/host (tests, simulator),
 * the BR25 port in the application.
 */
#ifndef SMCLOG_PORT_H
#define SMCLOG_PORT_H

#include <stddef.h>
#include <stdint.h>

/* Microseconds since start; wraps after 2^32 µs (about 71 minutes), the decoder unwraps it. */
uint32_t smclog_port_time_us(void);
/* Rate of the timestamp clock, reported in the stream header. */
#define SMCLOG_TIMER_HZ 1000000u

/* Short critical section around ring updates (interrupts off on the device). The returned state
 * goes back to smclog_port_unlock(). */
uint32_t smclog_port_lock(void);
void smclog_port_unlock(uint32_t state);

/* Build ID of the running image (the GNU build-id note on the device, LC_UUID on macOS). */
const uint8_t *smclog_port_build_id(size_t *len);

#endif /* SMCLOG_PORT_H */
