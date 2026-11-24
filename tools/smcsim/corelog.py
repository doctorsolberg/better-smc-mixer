"""The control core's log records, read back from their text (docs/core.md, "Outputs").

The core (firmware/core/core.c) logs its input events (module input) and what it does with them
(module engine). The scenario runner on the device (device.py) and replay (smcsim.py) read these
records, so their wording is an interface. This module is the one place that knows it: FORMATS
lists the format strings core.c must contain (test_smcsim.py checks the source for them), and the
parse_* functions read the decoded text. To change a record's wording, change core.c, FORMATS and
the pattern here together.

Standard library only.
"""
import re

# The format strings of core.c these parsers depend on, as written in the source.
FORMATS = (
    "button %u down", "button %u down (injected)",
    "button %u up", "button %u up (injected)",
    "encoder %u %+d", "encoder %u %+d (injected)",
    "fader %u = %d", "fader %u = %d (injected)",
    "midi in %u: %02x %02x %02x", "midi in %u: %02x %02x %02x (injected)",
    "out %02x %02x %02x", "out %02x %02x",
    "led %u = %u",
    "preset %u page %u",
    "shift %u",
)

_INJ = r"( \(injected\))?$"
_BUTTON = re.compile(r"button (\d+) (down|up)" + _INJ)
_ENCODER = re.compile(r"encoder (\d+) ([-+]?\d+)" + _INJ)
_FADER = re.compile(r"fader (\d+) = (-?\d+)" + _INJ)
_MIDI_IN = re.compile(r"midi in (\d+): ([0-9a-f]{2}) ([0-9a-f]{2}) ([0-9a-f]{2})" + _INJ)
_OUT = re.compile(r"out ([0-9a-f]{2}(?: [0-9a-f]{2}){1,2})$")
_LED = re.compile(r"led (\d+) = (\d+)$")
_BANK = re.compile(r"preset (\d+) page (\d+)$")
_SHIFT = re.compile(r"shift ([01])$")


def parse_input(msg):
    """An input record (module input) as (kind, index, value, injected), or None.

    kind "b": index is the button (0-44), value 1 for down, 0 for up.
    kind "e": index is the encoder (0-7; the log says 1-8), value the signed clicks.
    kind "f": index is the fader (0-7; the log says 1-8), value the position.
    kind "m": index is the transport (0 USB, 1 BLE), value the message, "b0 b1 b2" in lower case.
    """
    m = _BUTTON.match(msg)
    if m:
        return "b", int(m.group(1)), 1 if m.group(2) == "down" else 0, bool(m.group(3))
    m = _ENCODER.match(msg)
    if m:
        return "e", int(m.group(1)) - 1, int(m.group(2)), bool(m.group(3))
    m = _FADER.match(msg)
    if m:
        return "f", int(m.group(1)) - 1, int(m.group(2)), bool(m.group(3))
    m = _MIDI_IN.match(msg)
    if m:
        return "m", int(m.group(1)), " ".join(m.group(i) for i in (2, 3, 4)), bool(m.group(5))
    return None


def parse_out(msg):
    """The MIDI message of an "out ..." record (module engine), "B0 14 40" in upper case, or None."""
    m = _OUT.match(msg)
    return m.group(1).upper() if m else None


def parse_led(msg):
    """(led, state) of a "led N = S" record (module engine, TRACE), or None."""
    m = _LED.match(msg)
    return (int(m.group(1)), int(m.group(2))) if m else None


def parse_bank(msg):
    """(preset, page), both from 1, of a "preset P page G" record (module engine), or None."""
    m = _BANK.match(msg)
    return (int(m.group(1)), int(m.group(2))) if m else None


def parse_shift(msg):
    """True or False for a "shift 1" / "shift 0" record (module engine), or None."""
    m = _SHIFT.match(msg)
    return (m.group(1) == "1") if m else None
