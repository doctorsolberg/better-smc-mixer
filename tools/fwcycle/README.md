# Firmware-cycle runner

A checkpointed, host-tested controller for the real editor's rollback/install/reconnect
scenario over a strictly selected USB or Bluetooth transport. On the mixer it reached
`passed_assisted` over both transports with exact configuration and fresh panel equality; the
USB repeat needed only the power-switch pause. On 2026-11-23 it ran the Bluetooth cycle of the
release candidate ([integration.md](../../docs/integration.md), "Hardware results, 2026-11-23").
The final comparison passes on byte-identical configuration content when only the revision
differs and storage is clean after the restart: the mixer assigns revisions, and a restart boots
the saved one.

## Scope

- USB: our configuration MIDI endpoint -> rollback -> stock USB service SysEx -> backup ->
  install/read-back -> physical power switch -> our USB endpoint and final verification.
- BLE: our system BLE MIDI endpoint -> rollback -> stock Web Bluetooth AE40 -> backup ->
  install/read-back -> physical power switch -> system BLE MIDI and final verification.
- The selected transport must match actual initial, stock and final connection observations.
  No automatic fallback between transports.
- The existing editor is the sole flash writer. This tool does not implement flashing again.
- No default test command accesses a device, starts Chrome, toggles permissions, erases pairings
  or changes system settings. Hardware execution needs current-session device and flash consent.

## Safety boundaries

Before rollback and install, persist the exact intended action and its prerequisites. An
interrupted action is ambiguous until actual device/page state is reconciled. A timeout during
installation is not permission to disconnect, restart Chrome, replay Install or power off.
Human power-switch acknowledgements are followed by real firmware identity checks.

Evidence belongs in a unique ignored private run directory. Full flash backups contain vendor
and Bluetooth data: never commit or attach them publicly. A file length/hash is necessary but
not sufficient for a safe installation: retain the real install-plan checks too.

The initial implementation uses local Python standard library and Node's built-in WebSocket;
no npm/pip installation is required. Missing runtime support is a prerequisite error, not a
request to install packages automatically. Commands do not depend on a particular private
interpreter or tool installation.

## Commands

Host-only, no browser/device attachment:

```sh
python3 -I tools/fwcycle/fwcycle.py check --host-only
python3 -I tools/fwcycle/fwcycle.py plan --transport usb --release build/release/manifest.json
python3 -I tools/fwcycle/fwcycle.py plan --transport ble --release build/release/manifest.json
python3 -I -m unittest discover -s tools/fwcycle -p 'test_*.py'
node --test tools/fwcycle/test_cdp.mjs tools/fwcycle/test_bridge.mjs tools/fwcycle/test_editor.mjs tools/fwcycle/test_compact.mjs tools/editortest/cycle-observer.mjs
```

Hardware invocation, only after current-session owner device/flash consent and an exclusive
connected editor page with observer v1. These commands are not part of default tests:

```sh
python3 -I tools/fwcycle/fwcycle.py start --transport usb \
  --release build/release/manifest.json --run-dir materials/fwcycle-UNIQUE \
  --profile /path/to/owned/chrome-profile \
  --endpoint http://127.0.0.1:PORT --origin http://127.0.0.1:8876 \
  --allow-device --allow-flash
python3 -I tools/fwcycle/fwcycle.py status --run materials/fwcycle-UNIQUE --json
python3 -I tools/fwcycle/fwcycle.py ack --run materials/fwcycle-UNIQUE \
  --checkpoint CHECKPOINT_ID --token ONE_USE_TOKEN
python3 -I tools/fwcycle/fwcycle.py continue --run materials/fwcycle-UNIQUE \
  --profile /path/to/owned/chrome-profile \
  --endpoint http://127.0.0.1:PORT --origin http://127.0.0.1:8876 \
  --allow-device --allow-flash
python3 -I tools/fwcycle/fwcycle.py stop --run materials/fwcycle-UNIQUE
```

Choose `ble` for the Bluetooth scenario. Parent evidence directory must exist; the unique run
directory must not. Flags record consent rather than granting it. No automatic browser/server
startup or pairing deletion. The macOS helper can connect only our exact BLE MIDI row or
disconnect stock MIDI when needed; unknown native UI/discovery requires explicit assistance.
Each invocation ends at a checkpoint or uncertainty, leaving the browser intact. Exit codes:
0 passed_assisted, 1 failed, 2 prerequisite/usage error, 3 waiting, 4 needs_reconciliation.
Do not treat an interrupted write as resumable by clicking Install again.

`start` and `continue` hold a project-local lock, `build/fwcycle.lock`, so two run directories
cannot drive the mixer at once; it does not exclude other editors or phones. Native scheduling
jitter in the host test suite is a separate known test-reliability issue.

Optional macOS host browser smoke (isolated headless Chrome and simulated devhost, no mixer):

```sh
node tools/fwcycle/browser-smoke.mjs
```

It requires existing build/host/devhost, local installed Chrome and Node with native WebSocket.
It starts its own private loopback server/profile and checks actual UI pull/download. The test
server uses a larger request backlog; this is not a fix to the normal server's intermittent
connection-reset behavior. No physical firmware installation is claimed by this smoke.
