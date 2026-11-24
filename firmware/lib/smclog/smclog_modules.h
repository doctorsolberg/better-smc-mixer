/*
 * Log modules (SPEC LOG-02). The order gives the module index in the dictionary; names are stored
 * with every call site, so the decoder does not depend on this list. Module names must not be
 * macros (LOG_x expands its arguments before they are pasted).
 */
#ifndef SMCLOG_MODULES_H
#define SMCLOG_MODULES_H

#define SMCLOG_MODULES(X) \
    X(boot)               \
    X(smclog)             \
    X(sys)                \
    X(input)              \
    X(engine)             \
    X(midi_usb)           \
    X(midi_ble)           \
    X(usb)                \
    X(ble)                \
    X(storage)            \
    X(cfg)                \
    X(power)              \
    X(test)               \
    X(link)               \
    X(metrics)

#endif /* SMCLOG_MODULES_H */
