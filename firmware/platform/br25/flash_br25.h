/*
 * The configuration's flash slots and the panel's state on BR25 (store/cfgstore.h, statestore.h;
 * docs/config.md, "Storage"): read,
 * erase and program through the SDK's flash driver (cpu.a), the one its own VM uses.
 */
#ifndef FLASH_BR25_H
#define FLASH_BR25_H

#include "cfgstore.h"

/* The operations for cfgstore_init() and statestore_init(). Only the slots and the state's two
 * sectors are reachable through them. */
const struct cfgstore_flash *flash_br25_ops(void);

/* The operations for store/bootsel.c: reads below our area (sector 0 and M-Vave's area, as
 * stored), erase and program of sector 0 alone. */
const struct cfgstore_flash *flash_br25_boot_ops(void);

#endif /* FLASH_BR25_H */
