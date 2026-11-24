/*
 * The heartbeat's BR25 part (metrics_app.h).
 *
 * Stacks: sampled (metrics.h, stack_mark), never written. The SDK's RTOS (port_overwrite.c in
 * system.a, from the image's disassembly) allocates task stacks through the MMU
 * (pvPortVMallocStack) and at every task switch (vCreateTaskStack) moves the pages below the
 * outgoing task's stack pointer over to the incoming task, checking a 0x5A5A5A5A canary at the
 * stack's bottom (a chip reset if it is gone). So a painted stack would not keep its paint, and
 * painting its bottom would reset the chip. A watched stack's bounds: its bottom from uxTaskStack()
 * on the task's handle (TCB + 64), its size from task_info_table (the RTOS's own, TCB + 4, is
 * logged if it differs); used only if the task's current frame lies inside. ⚠️ Not run on
 * hardware.
 *
 * Heap: the SDK's heap (heap_mmu.c in system.a): free now and the lowest since start. The stacks'
 * free parts come from the same pages.
 */
#include "system/includes.h"

#include <string.h>

#include "diag_br25.h"
#include "input_br25.h"
#include "metrics_app.h"
#include "smc_ram.h"
#include "smclog.h"
#include "smclog_port.h"
#include "usb_app.h"

/* The SDK's (include_lib/system/os: os_api.h, FreeRTOS/task.h); uxTaskStackDepth has no
 * declaration (it returns TCB + 4, the size in words), xPortGetFreeHeapSize's is commented out in
 * FreeRTOS/portable.h. */
extern void *os_task_get_handle(const char *name);
extern void *uxTaskStack(void *tcb);
extern uint32_t uxTaskStackDepth(void *tcb);
extern size_t xPortGetFreeHeapSize(void);
extern size_t xPortGetMinimumEverFreeHeapSize(void);

static struct {
    const char *name;
    uint16_t size;          /* bytes */
    bool sampled;           /* bounds found: mark is valid */
    volatile bool ready;    /* the fields above are set */
    struct stack_mark mark; /* under the interrupt lock */
} stacks[METRICS_STACKS];
static unsigned n_stacks; /* slots taken */
static struct metrics_state state;
static bool started;

void metrics_app_task_start(const char *name, unsigned stack_words)
{
    char here; /* in this task's current frame */
    uintptr_t sp = (uintptr_t)&here;
    uint32_t l = smclog_port_lock();
    unsigned i = n_stacks < METRICS_STACKS ? n_stacks++ : METRICS_STACKS;
    smclog_port_unlock(l);
    if (i == METRICS_STACKS) {
        LOG_WARN(metrics, "stack %s: no room to watch it (%u tasks)", name, METRICS_STACKS);
        return;
    }
    uint32_t size = stack_words * 4u;
    void *tcb = os_task_get_handle(name);
    uintptr_t lo = tcb ? (uintptr_t)uxTaskStack(tcb) : 0;
    uint32_t depth = tcb ? uxTaskStackDepth(tcb) : 0;
    stacks[i].name = name;
    stacks[i].size = (uint16_t)size;
    if (!lo || sp < lo || sp - lo >= size) {
        LOG_WARN(metrics, "stack %s: bottom %p, frame %p, %u bytes: not measured", name, (void *)lo,
                 (void *)sp, size);
    } else {
        if (depth != stack_words)
            LOG_WARN(metrics, "stack %s: the RTOS has %u words, task_info_table %u", name, depth,
                     stack_words);
        l = smclog_port_lock();
        stack_mark_init(&stacks[i].mark, lo, lo + size, sp);
        stacks[i].sampled = true;
        smclog_port_unlock(l);
        LOG_DEBUG(metrics, "stack %s: %p, %u bytes; %u free now", name, (void *)lo, size,
                  (unsigned)(sp - lo));
    }
    __asm__ __volatile__("" ::: "memory");
    stacks[i].ready = true;
}

SMC_RAM_CODE void metrics_app_stack_sample(uintptr_t sp)
{
    for (unsigned i = 0; i < METRICS_STACKS; i++)
        if (stacks[i].ready && stacks[i].sampled && stack_mark_sample(&stacks[i].mark, sp))
            return;
}

void metrics_app_begin(struct metrics_window *w)
{
    uint32_t now = smclog_port_time_us();
    if (!started) { /* the first window starts at boot */
        metrics_state_init(&state, 0);
        started = true;
    }
    metrics_begin(&state, w, now);
    diag_br25_heartbeat(w->seq); /* the next boot reports this run's last heartbeat */
    input_br25_take_window(w);
    usb_app_take_window(w);
    w->have_memory = true;
    w->heap_free = (uint32_t)xPortGetFreeHeapSize();
    w->heap_min = (uint32_t)xPortGetMinimumEverFreeHeapSize();
    unsigned k = 0;
    for (unsigned i = 0; i < METRICS_STACKS; i++) {
        if (!stacks[i].ready)
            continue;
        w->stack[k].name = stacks[i].name;
        w->stack[k].size = stacks[i].size;
        if (stacks[i].sampled) {
            uint32_t l = smclog_port_lock();
            w->stack[k].unused = (uint16_t)stack_mark_free(&stacks[i].mark);
            smclog_port_unlock(l);
        } else {
            w->stack[k].unused = METRICS_STACK_UNKNOWN;
        }
        k++;
    }
    w->stacks = k;
}
