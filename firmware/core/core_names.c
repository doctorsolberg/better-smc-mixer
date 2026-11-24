/* Control and LED names for the simulator, scenarios and logs (docs/core.md). */
#include <stddef.h>
#include <string.h>

#include "core.h"

static const char *const button_names[CORE_BUTTONS] = {
    "m1", "s1", "r1", "sel1", "m2", "s2", "r2", "sel2", "m3", "s3", "r3", "sel3",
    "m4", "s4", "r4", "sel4", "m5", "s5", "r5", "sel5", "m6", "s6", "r6", "sel6",
    "m7", "s7", "r7", "sel7", "m8", "s8", "r8", "sel8",
    "play", "stop", "rec", "rew", "ff", "bank_left", "bank_right", "up", "down", "left", "right",
    "bt", "shift",
};

static const char *const encoder_led_names[CORE_STRIPS] = {
    "enc1", "enc2", "enc3", "enc4", "enc5", "enc6", "enc7", "enc8",
};

const char *core_button_name(unsigned button)
{
    return button < CORE_BUTTONS ? button_names[button] : NULL;
}

const char *core_led_name(unsigned led)
{
    if (led < CORE_BUTTONS)
        return button_names[led];
    if (led < CORE_LEDS)
        return encoder_led_names[led - CORE_BUTTONS];
    return NULL;
}

/* "<prefix>N" with N in 1..CORE_STRIPS → N - 1, else -1. */
static int numbered(const char *name, const char *prefix)
{
    size_t n = strlen(prefix);
    if (strncmp(name, prefix, n) != 0 || name[n] < '1' || name[n] > '0' + CORE_STRIPS ||
        name[n + 1] != '\0')
        return -1;
    return name[n] - '1';
}

int core_find_name(char kind, const char *name)
{
    switch (kind) {
    case 'b':
    case 'l':
        for (unsigned i = 0; i < CORE_BUTTONS; i++)
            if (strcmp(name, button_names[i]) == 0)
                return (int)i;
        if (kind == 'l' && numbered(name, "enc") >= 0)
            return CORE_BUTTONS + numbered(name, "enc");
        return -1;
    case 'e':
        return numbered(name, "enc");
    case 'f':
        return numbered(name, "fader");
    default:
        return -1;
    }
}
