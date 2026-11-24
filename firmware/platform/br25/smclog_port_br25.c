/*
 * smclog port for BR25 under the JieLi SDK.
 *
 * Time: microseconds from the scan timer once the scan has started (input_br25_time_us(),
 * metrics/usclock.h), the SDK's millisecond clock before that. Lock: irq_br25.h's, which also
 * holds off the scan interrupt (priority 7, which logs); inside it, a sample of the stack for the
 * stack metric (metrics_app.h): every log call passes here, deep in its caller's stack. The time
 * and the lock run from RAM, as the scan interrupt's code must (smc_ram.h). Build ID: the GNU
 * build-id note that tools/env/build_app.sh places in flash (__build_id_note).
 */
#include "smclog_port.h"

#include "irq_br25.h"
#include "smc_ram.h"

extern uint32_t input_br25_time_us(void);          /* input_br25.h */
extern void metrics_app_stack_sample(uintptr_t sp); /* metrics_app.h */

/* namesz, descsz, type, "GNU\0", then the ID */
extern const uint32_t __build_id_note[];

SMC_RAM_CODE uint32_t smclog_port_time_us(void)
{
    return input_br25_time_us();
}

SMC_RAM_CODE uint32_t smclog_port_lock(void)
{
    char here;
    uint32_t state = irq_br25_save();
    metrics_app_stack_sample((uintptr_t)&here);
    return state;
}

SMC_RAM_CODE void smclog_port_unlock(uint32_t state)
{
    irq_br25_restore(state);
}

const uint8_t *smclog_port_build_id(size_t *len)
{
    *len = __build_id_note[1];
    return (const uint8_t *)&__build_id_note[4];
}
