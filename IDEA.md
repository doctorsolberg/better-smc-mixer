# Product intent

better-smc-mixer turns the M-Vave SMC-Mixer into a configurable MIDI controller for hardware
synthesizers and iOS music apps, rather than a controller organized around a DAW.

This file explains **why the product exists and what guides its design**. What it does today
is in the [feature overview](docs/about.md) and the [user guide](docs/user/README.md).

## The problem

The hardware already provides eight faders, eight endless encoders, buttons, USB and Bluetooth
in a portable battery-powered box. Its stock DAW-oriented layout is a poor fit for a synth or
an iPad setup: players need their own assignments, more parameter groups than one physical
panel, and a way to change groups without sudden value jumps.

## The intended experience

- Organize a setup by instrument and parameter group: presets and pages, with a Shift layer
  for alternate assignments and fine control.
- Make non-motorized controls predictable: fader pickup, remembered target values and feedback
  from the instrument, with encoder behaviour suited to absolute synth parameters.
- Configure and try a change together. The editor and firmware are one product; what the editor
  says a control sends must match what the mixer actually sends.
- Keep live changes, saved startup configuration and files distinct, so the player knows what
  will survive power-off and can keep a readable backup.
- Reach gear through USB or BLE MIDI, including the vendor's wireless DIN adapter, without
  requiring a DAW or a hardware modification.
- Make installation reversible and failures diagnosable: backups, recovery, logs and replay
  matter as much as the normal playing path.

The requirements behind these goals are in [SPEC.md](docs/SPEC.md). A native iOS editor and
several advanced features are planned for later releases.

## Boundaries and priorities

Release 0.1.0 covers the core of the version-1.0 requirements: presets and pages, Shift,
pickup, feedback, the browser editor and installation over USB and Bluetooth. Finishing and
polishing what exists comes before new features; multi-message controls, learn on the mixer,
light settings and the iOS editor come later. DAW protocol emulation, audio functions, USB host
mode and board modifications are not version-1.0 goals.

Resource limits, minimal external dependencies, device data kept local and private, and a
rehearsed recovery path shape the implementation. Scope decisions belong to the specification,
not to an expansion of this vision.

## Where the detail belongs

| Question | Document |
|---|---|
| What exists, and what is planned, in musician-facing language? | [Feature overview](docs/about.md) |
| What exactly must it do, and what has been decided? | [Specification](docs/SPEC.md) |
| Which later ideas deserve discussion? | [Ideas](docs/ideas.md) |
| How do I use, build or change it? | [README](README.md), [user guide](docs/user/README.md), [agent instructions](AGENTS.md) |

This file stays short and changes only when the product's purpose or boundaries change.
