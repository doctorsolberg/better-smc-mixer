/*
 * Code the scan interrupt reaches (docs/hwtest.md, "While flash is written").
 *
 * While the SDK erases or programs flash, nothing can be read from flash. On BR25 the scan
 * interrupt runs at the one priority the SDK's flash code leaves unmasked, so everything it
 * reaches must be in RAM: functions marked SMC_RAM_CODE go to the SDK's .volatile_ram_code (part
 * of .data_code, copied to RAM at start). A constant table such code reads is marked SMC_RAM_DATA
 * (.data, also copied to RAM), or replaced by code. tools/re/rampath.py checks the result on the
 * image's disassembly.
 *
 * Here because every module that needs it includes smclog.h's directory; it has nothing to do
 * with logging otherwise. Off (empty) unless the build defines SMC_RAM_SECTIONS: the host builds
 * and the JieLi checks of the portable code (tools/env/check_device.sh) do not.
 */
#ifndef SMC_RAM_H
#define SMC_RAM_H

#ifdef SMC_RAM_SECTIONS
#define SMC_RAM_CODE __attribute__((section(".volatile_ram_code")))
#define SMC_RAM_DATA __attribute__((section(".data"))) /* a table that code in RAM reads */
#else
#define SMC_RAM_CODE
#define SMC_RAM_DATA
#endif

/* SMC_HOT: a function of the control path (core, ctl). With the build switch CORE_RAM=1
 * (SMC_CORE_RAM) it goes to RAM too: a measurement of that path's cost from flash (docs/core.md,
 * "Fast sweeps"); nothing with the switch off. */
#if defined(SMC_RAM_SECTIONS) && defined(SMC_CORE_RAM)
#define SMC_HOT SMC_RAM_CODE
#else
#define SMC_HOT
#endif

#endif /* SMC_RAM_H */
