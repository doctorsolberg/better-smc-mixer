# The configuration editor

The browser editor for the mixer (SPEC §6). Design and behaviour: [docs/app.md](../docs/app.md);
for musicians: [the editor guide](../docs/user/editor.md).

| Where | What |
|---|---|
| `editor/` | the editor: plain HTML, CSS and JavaScript modules, no build step or dependencies; configuration over USB and Bluetooth MIDI, firmware installation over USB and Web Bluetooth, offline file editing without Web MIDI, and explicit simulation (`?mock`) and host-firmware (`?devhost`) test modes |

Run it from the repository root:

```sh
python3 -I app/editor/serve.py
```

then open http://localhost:8765/app/editor/ (an iPad on the same network cannot reach it: the
server listens on this computer only). In the repository it reads `config/schema.json` and
`config/factory.json`; a release package serves the same files from `editor/data/`
(`make release`, [docs/build.md](../docs/build.md)). File ▸ Open a file takes a configuration
file; File ▸ Save a copy exports the canonical form. The working copy is kept in browser
storage; File ▸ Start from the factory settings starts over after confirmation. In Separate
mode these actions do not send changes until Send.

With Web MIDI and SysEx permission, **Connect** uses the real mixer. For our firmware over
Bluetooth, connect it as a Bluetooth MIDI device in the OS first; **Install over Bluetooth**
reaches M-Vave's stock GATT service for installation. Installation uses only the release
manifest and images served next to the editor. Without Web MIDI the page is an offline file
editor. Installing firmware writes the mixer's flash: keep the backup file the wizard saves.

Use `?mock` for the simulated mixer, or `?devhost&selftest` for the page against the firmware
built for the host (`make -C firmware sim` first; POSIX host). Neither is a physical-device test.
Hardware results are in [docs/integration.md](../docs/integration.md); for musicians, the
[user guide](../docs/user/README.md). Native mobile shells and the proposed TypeScript
implementation (FR-APP-02) are not built.
