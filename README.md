# better-smc-mixer

Alternative firmware for the M-Vave SMC-Mixer MIDI controller, with a browser-based
configuration editor. It turns the mixer's eight faders, eight endless encoders and lit buttons
into a configurable controller for hardware synthesizers and music apps.

**Open the editor:** https://doctorsolberg.github.io/better-smc-mixer/editor/ (Google Chrome with
Web MIDI on macOS, Windows or Android). Releases:
[GitHub Releases](https://github.com/doctorsolberg/better-smc-mixer/releases).

- Four presets of four pages, eight strips each. Every fader, encoder and button sends CC, 14-bit
  CC, NRPN, RPN, notes, program change, aftertouch or pitch bend on its own channel.
- A Shift layer for every control; fader pickup and scale; encoder acceleration and fine steps.
- USB MIDI and Bluetooth MIDI at the same time.
- Live editing in the browser over USB or Bluetooth MIDI, Learn from an instrument, parameter
  lists from the [MIDI Guide](https://midi.guide), firmware installation and a way back to
  M-Vave's firmware.

Version 0.1.0 is a pre-release. It was verified on a retail mixer: installation, update and going
back over USB and Bluetooth, power off/on with byte-exact configuration read-back, and the
end-to-end and editor self-tests on the device.

## Documentation

- [User guide](docs/user/README.md): installing, connecting, the editor, the panel, a first preset
- [Feature overview](docs/about.md) and [product intent](IDEA.md)
- [Requirements](docs/SPEC.md) and [ideas for later](docs/ideas.md)
- [Firmware architecture](firmware/README.md), [the editor](app/README.md),
  [editor and firmware integration](docs/integration.md)
- [Build environment](docs/build.md) and [recovery](docs/recovery.md)
- [Contributor and agent instructions](AGENTS.md)

## Development

Run commands from the project root. Python tools use the standard library; the editor uses
native browser ES modules and needs no npm installation.

```sh
make quick      # host tests, a few minutes
make test       # every host test
make device     # the device images (Docker; tools/env/fetch_deps.sh fetches the pinned JieLi toolchain and SDK)
make release    # the release package in build/release/, every image built twice and compared
```

Serve the editor locally:

```sh
python3 -I app/editor/serve.py 8876
```

and open `http://127.0.0.1:8876/app/editor/`.

## Releases

A tag `vX.Y.Z` makes GitHub Actions build the package from source, check that every image is
byte-identical to the candidate verified on the mixer (`release/expected.json`) and create a draft
release. Publishing the draft deploys the site: the home page, the editor and the firmware it
installs, with older releases kept under `/vX.Y.Z/`. Details: [docs/build.md](docs/build.md),
"Publishing".

## Licence

Our code is MIT licensed ([LICENSE](LICENSE)). The firmware is built against the JieLi AC63 SDK
(Apache License 2.0) and includes TinyUSB, newlib and compiler-rt code: see [NOTICE](NOTICE).
better-smc-mixer is an independent project, not affiliated with M-Vave.
