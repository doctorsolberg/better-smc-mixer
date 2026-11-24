# TinyUSB (frozen copy)

- Upstream: https://github.com/hathach/tinyusb
- Version: release **0.21.0**, commit `dae3f9a366bfcddbf9dcf1b48d7500286a849539` (the same release
  open-smc-pad builds with, so the BR25 controller driver interface matches what is known to work)
- License: MIT, see [LICENSE](LICENSE)
- Local changes: **none**. Every file is listed with its SHA-256 in [MANIFEST](MANIFEST).

Only what the firmware needs is kept (SPEC C-4): the device core (`src/device`), the MIDI, CDC and
DFU (and DFU runtime) device classes, `class/audio/audio.h` (MIDI descriptors use it), the common
code and the no-OS abstraction. Host, Type-C, the other classes and all controller drivers are
left out. The BR25 device controller driver (`dcd_*`) is our own code under `firmware/`.

```sh
tools/env/vendor_tinyusb.sh check    # the copy matches MANIFEST
tools/env/vendor_tinyusb.sh update   # fetch the pinned commit again and recopy (changes MANIFEST
                                     # only if the pin or the file list in the script changes)
```

Configuration (`tusb_config.h`) is ours and lives next to the code that uses it. Without
`TUP_DCD_ENDPOINT_MAX` in the configuration TinyUSB warns and assumes 8 endpoints, because BR25 is
not one of its MCUs (`CFG_TUSB_MCU = OPT_MCU_NONE`).
