/*
 * The product's version (one semantic version for the firmware, the editor and the release package:
 * the owner, 2026-11-14; docs/build.md, "The version"). The source is VERSION at the repository's
 * root; tools/release/check_versions.py fails when this copy differs.
 *
 * It shows in the boot journal (SPEC LOG-11), in GET_INFO (docs/link.md) and as the USB device's
 * bcdDevice. The build ID still identifies an image (LOG-12): builds between releases carry the
 * version of the release they lead to.
 */
#ifndef APP_VERSION_H
#define APP_VERSION_H

#define APP_VERSION_MAJOR 0
#define APP_VERSION_MINOR 1
#define APP_VERSION_PATCH 2

#define APP_VERSION_STR2(x) #x
#define APP_VERSION_STR(x) APP_VERSION_STR2(x)
#define APP_VERSION                                                                                \
    APP_VERSION_STR(APP_VERSION_MAJOR) "." APP_VERSION_STR(APP_VERSION_MINOR) "." APP_VERSION_STR(    \
        APP_VERSION_PATCH)

/* USB's bcdDevice, 0xJJNN in BCD: the major and the minor number, two digits each (the patch number
 * does not fit; GET_INFO and the journal carry it) */
#define APP_VERSION_BCD2(n) ((((n) / 10) << 4) | ((n) % 10))
#define APP_VERSION_BCD ((APP_VERSION_BCD2(APP_VERSION_MAJOR) << 8) | APP_VERSION_BCD2(APP_VERSION_MINOR))
_Static_assert(APP_VERSION_MAJOR <= 99 && APP_VERSION_MINOR <= 99, "bcdDevice: two digits each");

#endif /* APP_VERSION_H */
