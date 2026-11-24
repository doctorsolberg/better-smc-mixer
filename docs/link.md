# The command link

Commands from the host and their replies over USB CDC or SysEx on USB/BLE MIDI (SPEC
FR-IO-02, §7). The device log stream uses CDC only (LOG-12). Device side:
[`firmware/link/`](../firmware/link/) (portable) and [`firmware/app/usb/`](../firmware/app/usb/)
(the USB part); host side: `tools/smclog/smclog.py` (`cmd`, `capture`).

**Status (2026-11-23):**

- The link is written and tested on the host:
  - `firmware/tests/test_link.c`: framing, commands, retries and pushed events;
  - the device scenario runner against the controller's portable code over a pseudo-terminal
    (`tools/smcsim/test_smcsim.py`, [core.md](core.md), "Scenarios on the device");
  - the host tool against the same C code over a pseudo-terminal (`tools/smclog/test_smclog.py`).
- Both application variants carry it over USB CDC. They build with the JieLi toolchain.
- ✅ It runs on the mixer since 2026-11-11 ([hwtest.md](hwtest.md)).
- The configuration protocol ("Configuration" below, 2026-11-11): written and tested on the host
  (`firmware/tests/test_cfglink.c`, `tools/smcconf/test_device.py` against `devhost`), in the
  controller's build; ✅ on the mixer since controller 20 (2026-11-12). SysEx over USB/BLE MIDI
  followed on controller 31, live edits/Save/Revert on 32, EVENT/SELECT on 34 and SENT on 35.
- ✅ On 2026-11-23 on a retail mixer: the end-to-end test 37/37 over USB, the editor's page
  self-test 29/29, USB and Bluetooth firmware cycles with configuration read-back, and the
  mixer-assigned revision (P-6) noticed by an open editor ([integration.md](integration.md),
  "Hardware results, 2026-11-23").

The command numbers and payloads are the current pre-release contract. CDC and SysEx share
the records and command semantics; framing and the unavailable SysEx log stream differ below.

## Frames and records

Both directions use the log's frames ([logging.md](logging.md), "Frames"):

1. the record;
2. its CRC-16/CCITT-FALSE, little-endian;
3. COBS;
4. a `0x00` delimiter.

A frame that does not check is dropped and counted; there is no negative reply, because its
`seq` cannot be trusted. The host sends a `0x00` before each command, which ends any partial
frame left on the device.

| Kind | Byte 0 | Direction | Fields after it |
|---|---|---|---|
| LOG, LOST, HEADER, LOG_TRUNC | `0x01`–`0x04` | device → host | [logging.md](logging.md), "Records" |
| CMD | `0x10` | host → device | seq u8, cmd u8, payload |
| REPLY | `0x11` | device → host | seq u8, cmd u8, status u8, payload |
| EVENT | `0x12` | device → host | count u8 (events sent on this link since EVENTS on, mod 256), the panel's state ("The panel's state" below) |
| SENT | `0x13` | device → host | first u32 (the sequence number of the first entry), lost u8 (entries this link lost before it, at most 255), count u8 (1–16), the entries ("What the mixer sent" below) |

- A CMD record is at most 192 bytes (`LINK_CMD_MAX`); a longer frame is dropped and counted.
- A REPLY record is at most 192 bytes (`LINK_REPLY_MAX`; 64 before the configuration protocol).
- All numbers are little-endian.

## Rules

- **One reply per command, one command at a time.** The device takes the next command only once
  the last reply is handed to the transport. The host waits for each reply (stop and wait); the
  bytes it sent meanwhile wait in the CDC buffer.
- **Retries (FR-CFG-03).** If the host gets no reply within a timeout (1 s in the tool), it sends
  the same command again with the same `seq`, up to 3 times. A command with the same `seq` and
  `cmd` as the last one gets the last reply again and is **not executed again**: an injection is
  not doubled. A new command takes the next `seq` (mod 256). The tool starts from a random `seq`,
  and the device forgets the last one on a reset (below). So a restarted tool does not meet a
  stale reply.
- **The log stream.**
  - Nothing is sent until LOG_SUBSCRIBE. Then come the reply, a HEADER (every stream starts with
    one, LOG-12), and the records from the ring, oldest first: the boot records are still there
    unless the ring filled before the host subscribed.
  - The stream stops after LOG_UNSUBSCRIBE or a reset.
  - The device drains only whole frames that fit in the transport's free room. When the host is
    slow, records stay in the ring; when the ring is full, new ones are dropped and counted, and
    a LOST record follows (LOG-03).
  - The pending reply goes before the header, the header before the records.
- **Events (P-2, FR-CFG-06).** Nothing comes until EVENTS on, on this link (each transport has
  its own: the CDC, the USB MIDI config port, BLE). Then the device sends an EVENT whenever the
  panel's state differs from the one it last sent on this link, the first one right away: so a
  client learns where the panel stands, and then every change of the preset, the page, Shift,
  Bluetooth, the storage, the running configuration's ID or revision, and every touch of a
  control (the state counts them). At most one EVENT per 40 ms per link (`LINK_EVENT_GAP_US`):
  a fader swept gives at most 25 a second, each the newest state, and one touched in between is
  counted but not named. The state is whole in every EVENT, so a lost one costs nothing but the
  name of a touch. A pending reply goes first; an EVENT is never split, and waits while the
  transport has no room. EVENTS off, or a reset, stops them.
- **What the mixer sent (P-5).** With EVENTS' bit 1, the device sends SENT records: every MIDI
  message the core sent from then on, with the control that sent it ("What the mixer sent"
  below), up to 16 a record, at most one record per 20 ms per link (`LINK_SENT_GAP_US`) while
  entries wait. A reply goes first, then an EVENT, then a SENT. The device keeps the last 63
  entries (`CTL_SENT_RING` - 1); a link that falls further behind skips the oldest and says how
  many in the next record's `lost`.
- **Reset.** The USB part resets the link when:
  - the host closes the port (DTR drops);
  - a bus reset happens;
  - the configuration is lost.

  A reset stops the stream and the events, drops a partial frame and an unsent reply, forgets the
  last `seq` and clears the CDC transmit buffer.
- **Logging.** The link logs to module `link`:
  - INFO: subscriptions (the log stream, the events), level changes, raw mode;
  - WARN: commands that fail, and the first bad frame and the first too-long frame;
  - DEBUG: each command.

## Commands

| cmd | Name | Payload | Reply payload (status ok) |
|---|---|---|---|
| `0x01` | PING | up to 32 bytes | `t_us` u32 (the device's log clock), the payload |
| `0x02` | GET_INFO | — | link version u8 (1), log format u8 (1), variant u8 (0 app, 1 hwtest), capabilities u32, ring size u32, timer Hz u32, build ID length u8, build ID, version length u8 (0: none; at most 16), the product's version as ASCII (`0.1.0`; [build.md](build.md), "The version"; a firmware before 2026-11-14 ends at the build ID, and readers take its absence as no version) |
| `0x20` | LOG_SUBSCRIBE | — | — (then the stream) |
| `0x21` | LOG_UNSUBSCRIBE | — | — |
| `0x22` | LOG_SET_LEVEL | module u8 (`0xFF`: all), level u8 (0 off … 5 TRACE) | — |
| `0x23` | LOG_GET_LEVELS | — | count u8, a level per module (`smclog_modules.h` order) |
| `0x24` | LOG_RAW_INPUT | which `u8`: bit 0 the matrix and the encoder lines, bit 1 the faders' ADC values; bits 4–7: 1–8 probes that encoder (a diagnostic: [hwtest.md](hwtest.md), "Encoder probe"); `0` off; bits 2–3 and a probe above 8 refused | — (LOG-07) |
| `0x25` | METRICS_NOW | — | — (the statistics and a metrics heartbeat go to the log, LOG-06: [logging.md](logging.md), "Metrics") |
| `0x26` | BOOT_LOG | — | — (the boot journal goes to the log again, LOG-11: [logging.md](logging.md), "Boot journal") |
| `0x27` | RESET | how u8: 0 soft, 1 fault test, 2 watchdog test | reset status u8 (0 ok, 1 disabled); the reset follows about 100 ms later |
| `0x28` | EVENTS | what u8: bit 0 the panel's EVENTs, bit 1 SENT (P-5; capability bit 9); 0 off; other bits refused (bad argument) | — (then the EVENT and SENT records: "Rules" above) |
| `0x29` | STOCK | what u8: 0 check, 1 back, which must be followed by `BACK` (ASCII; else bad argument); 1 or 5 bytes (else bad length) | going-back status u8, sector 0's boot pointer u32 (after a back that held: `0x1000`), tries u8 (writes of sector 0; 0 for a check). The back's reset follows about 100 ms later |
| `0x30` | INPUT_INJECT | the injection payload ([hwtest.md](hwtest.md)) | injection status u8, accepted u8, refused u8 (LOG-33) |
| `0x31` | CORE_SETUP | settings as text, at most 180 bytes: `NAME VALUE` pairs separated by spaces or newlines (`firmware/core/core_settings.h`); empty: the factory configuration | setup status u8, the number of the pair not understood u8 (LOG-35) |
| `0x40`–`0x4C` | CFG_* | the configuration protocol: "Configuration" below | a configuration status u8, then the command's fields |

- Capabilities: bit 0 log stream, bit 1 raw input, bit 2 injection, bit 3 metrics, bit 4 setup,
  bit 5 boot journal, bit 6 reset, bit 7 the configuration protocol, bit 8 events (EVENTS: the
  configuration protocol's firmware, version 3 on), bit 9 what the mixer sent (EVENTS' bit 1,
  CFG_SENT: version 4 on), bit 10 going back to M-Vave's firmware (STOCK). The link's version
  stays 1:
  new commands come with a capability bit.
- Status:
  - 0 ok;
  - 1 unknown command;
  - 2 bad length;
  - 3 bad argument;
  - 4 unavailable (this firmware does not offer it).
- An injection that the device refused is still status ok: the reply's own status says why
  (`disabled`, `bad format`, `bad event`, `full`).
- CORE_SETUP restarts the core on a built-in configuration (`config factory`, the default, or
  `config test`) with some of its settings changed, for scenarios run on the device
  ([core.md](core.md), "Scenarios on the device"). Like injection it is a debug command:
  - it works only while injection is on (else status `disabled`);
  - it changes RAM only, never flash, and a reboot undoes it (LOG-34);
  - the control task applies it in its next round (log: "setup: … configuration, … settings
    changed", then the core's "core start"); a second one before that is `busy`;
  - a pair it does not understand is `bad setting` with the pair's number, and nothing changes.
  The targets go back to their defaults; LEDs are redrawn where they change.
- RESET answers first; the reset comes about 100 ms later, once the reply has reached the host
  (on BR25 the USB task does it, `diag_br25_service()`). It logs `reset on request: …` first, so
  the next boot replays that record ([logging.md](logging.md), "Boot journal"). The modes:
  - **soft** (0): a soft reset of the chip, `chip_reset()`; always allowed;
  - **fault** (1): a deliberate CPU exception, a misaligned word load in `diag_br25_fault()`. On
    the host stand-in our exception handler writes the crash record and resets, so this tests the
    crash record's path. ✅ On BR25 too (2026-11-11): the exception comes to our handler on IRQ 1,
    a few instructions late ([logging.md](logging.md), "The exception handler (BR25)"). If the load
    does not trap, nothing resets, and the log says so;
  - **watchdog** (2): a hang with interrupts off until the watchdog resets the chip. The SDK's
    setup sets the watchdog to 8 s, and only the idle hook clears it (from the image's
    disassembly). If the watchdog were off, the device would hang until its power is removed.

  The fault and watchdog modes are debug functions: they need injection on, otherwise the status
  is `disabled` (LOG-34, SPEC D-9). None of the three changes flash.
- STOCK points the mixer back at M-Vave's firmware, which our installation leaves in flash
  ([image.md](image.md), "Going back"); it is also the update's first half ([app.md](app.md),
  "Firmware"). Check writes nothing; back checks the same, writes sector 0 and reads it back, and
  on ok resets the chip (soft) once the reply has gone, as RESET does. The going-back status:
  - 0 ok;
  - 1 sector 0 does not parse;
  - 2 sector 0 does not point at ours (nothing to do);
  - 3 M-Vave's area does not check;
  - 4 flash (a read failed, nothing written; or sector 0 never read back as written);
  - 5 busy (a configuration is being saved: try again);
  - 6 not offered (devhost without `--boot`).

  It is not a debug command: injection need not be on. Without the operation (`struct link_ops`'s
  `stock`) the link status is `unavailable`. The magic keeps a stray byte from writing sector 0.
  ✅ Check/back ran on the mixer on 2026-11-14, controller 36, over USB and BLE MIDI; item 9
  below and [image.md](image.md), "Going back", record the results. A new run still needs
  current-session authorization; a successful historical back does not verify today's boot state.
- Levels above the build's `SMCLOG_LEVEL_MAX` are accepted but produce nothing (their call sites
  were not compiled).

What each variant offers (`struct link_ops`, `firmware/app/include/variant.h`):

| Variant | GET_INFO variant | Raw input | Metrics | Injection |
|---|---|---|---|---|
| `app` (the controller) | 0 | `scan.raw_mode`, off at start | the scan statistics, the control path's and the MIDI queue's counters, the USB driver's, the link's and USB MIDI's counters | into the input queue: the core plays them like the panel ([core.md](core.md), "On the device"); CORE_SETUP too; the configuration protocol |
| `hwtest` | 1 | `scan.raw_mode`, the matrix at start | the scan statistics, the USB driver's, the link's and USB MIDI's counters | into the input queue, logged as `(injected)`; no CORE_SETUP and no configuration protocol (no core) |

## Configuration

The configuration protocol (SPEC §7; stage 4): the host reads the running configuration,
replaces it, changes parts of it without a restart (CFG_SET, P-1), saves it and goes back to the
saved one (CFG_SAVE, CFG_REVERT, Q-26). What travels is the configuration's binary form ([config.md](config.md), "Binary
form"), opaque to the protocol: it is cut into chunks of at most 128 bytes, and a CRC-32 (zlib's,
`cfgbin_crc32()`) covers the whole transfer on top of each frame's CRC-16. Device side:
`firmware/link/cfglink.c` (portable) on the control path (`firmware/ctl/ctl.c`); host side:
`smcconf info`, `pull`, `push`, `edit`, `save` and `revert` (`tools/smcconf/devlink.py`,
[config.md](config.md), "Command line"); the editor's `app/editor/js/link.js`.

**Saving (Q-26).** What the mixer plays (RAM) and what it starts with (flash) are two copies. The
configuration's own global setting `save` says how the first becomes the second: `manual` (the
default) on CFG_SAVE only, `auto` by itself about 3 s after the last change and the last touch of
a control (SPEC Q-22). The next start runs what was saved. The slots, the order of the writes and
why a power cut cannot lose both: [config.md](config.md), "Storage". ✅ Saves ran on the mixer on
2026-11-12 (controller 21, [config.md](config.md), "At the new place").

### Commands

Every reply's payload (link status ok) starts with a configuration status u8; the fields after it:

| cmd | Name | Payload | Reply after the status |
|---|---|---|---|
| `0x40` | CFG_INFO | — | protocol version u8 (4; 3 before SENT; 2 before STATE, SELECT and the events; 1 before SET, SAVE and REVERT), binary version u8, schema version u16, largest length u32, chunk u8 (128), state u8, source u8, revision u32, ID 16 bytes (zeros: none), storage u8, saved generation u32 (0: none) |
| `0x41` | CFG_GET_BEGIN | — | length u32, CRC-32 u32 of a snapshot of the running configuration (its canonical binary form, with its ID and revision) |
| `0x42` | CFG_GET_CHUNK | offset u32, count u8 (more than 128 reads 128) | offset u32, the bytes (fewer at the end, none at the length) |
| `0x43` | CFG_PUT_BEGIN | length u32, CRC-32 u32 | — |
| `0x44` | CFG_PUT_CHUNK | offset u32, 1–128 bytes | bytes received u32 (also when refused: where the upload stands) |
| `0x45` | CFG_PUT_COMMIT | — | on INVALID, cfgbin's error: code u8, preset u8, page u8, control u8, field u8, offset u32 (`cfgbin.h`, 255: none) |
| `0x46` | CFG_ABORT | — | — |
| `0x47` | CFG_SET | legacy revision u32 (ignored by the device), preset u8 (0-based), records (1–184 bytes) | on INVALID, cfgbin's error as for COMMIT |
| `0x48` | CFG_SAVE | — | — |
| `0x49` | CFG_REVERT | — | — |
| `0x4A` | CFG_STATE | — | the panel's state (below) |
| `0x4B` | CFG_SELECT | preset u8, page u8 (0-based; `0xFF` keeps it; a preset alone goes to its last page) | — |
| `0x4C` | CFG_SENT | from u32 (a sequence number; `0xFFFFFFFF`: now) | first u32, count u8 (0–16), the entries from `first` ("What the mixer sent" below) |

- Configuration status (`enum cfglink_status`): 0 ok; 1 busy (a change waits for the control
  task: ask again); 2 no transfer (a chunk or a commit without its BEGIN); 3 bad offset; 4 size
  (more than the device takes, less than a header, or past the announced length); 5 incomplete
  (a commit before every byte arrived); 6 bad CRC (the bytes are not what PUT_BEGIN said); 7
  invalid (cfgbin refused it; the error follows); 8 unavailable (SAVE: no flash storage in this
  firmware, or the running configuration is a setup's, which is never saved; SET, REVERT: a host
  of the protocol without them).
- State: 0 idle, 1 get (a snapshot to read), 2 put (an upload under way), 3 applying (a running
  configuration change awaits the control task; do not use the accompanying identity), 4 saving
  (being written to flash).
- Source (`enum ctl_source`): 0 the factory configuration, 1 the test configuration (a
  CORE_SETUP), 2 received, 3 stored (loaded from flash at start); bit 7 set when a CORE_SETUP
  changed some of its settings. The received or stored configuration supplies the ID (FR-APP-24);
  the built-in configurations have no ID (zeros). The revision is the device's running counter.
- Storage (`enum cfglink_store`): 0 none (no flash storage in this firmware), 1 clean (nothing
  waits), 2 waiting (a received configuration waits for the quiet pause), 3 writing, 4 failed (the
  last save failed three times; the configuration saved before stays), 5 unsaved (the setting is
  `manual`: what the mixer plays differs from what it starts with until a CFG_SAVE). The saved
  generation is the
  newest slot's: it counts the saves, 0 when nothing is saved.
- Wrong payload lengths are the link's status 2 (bad length); a CFG command on a firmware without
  the protocol is status 4 (unavailable).

### The panel's state

What CFG_STATE answers and every EVENT carries (P-2, P-6; 30 bytes, `CFGLINK_STATE_LEN`):

| Offset | Field | |
|---|---|---|
| 0 | preset u8 | 0-based |
| 1 | page u8 | 0-based |
| 2 | shift u8 | 1 while Shift is active |
| 3 | bluetooth u8 | 0 off, 1 on, 2 none in this firmware; bit 7 set while a central is connected |
| 4 | touches u8 | the controls touched since the start, mod 256 |
| 5 | touched preset u8 | where the last touch was: the preset, |
| 6 | touched page u8 | the binary form's page (0-3; 255 the bottom row; [config.md](config.md), "Binary form"), |
| 7 | touched control u8 | the binary form's control ID (strips: group × 8 + strip; the row: its button), |
| 8 | touched layer u8 | 0 Normal, 1 Shift (as it was when the control was used); all four 255 before the first touch |
| 9 | storage u8 | as CFG_INFO's |
| 10 | revision u32 | the running configuration's, as CFG_INFO's |
| 14 | ID, 16 bytes | the running configuration's, as CFG_INFO's (zeros: none) |

- **A touch** is what the panel's scan (or an injection) reports for a configurable control: a
  press of a strip button or a bottom-row button (not its release), a turn of an encoder, a move
  of a fader. Shift and BT are not touches (their effects are in the state), nor is MIDI in. A
  row button that switches the preset or the page with Shift is a touch on the Shift layer, and
  the preset and page then change too.
- **Has it changed (P-6).** The ID and the revision are the cheap answer: a client that knows the
  configuration it last read or wrote compares them (CFG_STATE, or the EVENTs) instead of reading
  the configuration again. The mixer increments the running revision on every applied upload,
  SET, revert or runtime setup, regardless of the revision in the request. It wraps from
  `0xFFFFFFFF` to zero. Save, panel changes, refused or abandoned transfers, and link retries
  replaying a cached reply do not increment it. Even identical accepted writes count if applied;
  an edit with no commands to send does not. Boot takes the saved revision, or zero with factory
  defaults. Compare both fields for equality, not numerical order, and read the configuration
  again on reconnect. The wire layouts and versions are unchanged; older firmware may still use
  the revision sent by the client. While a change is pending CFG_STATE is `busy` and no transient
  identity is emitted as an EVENT.
- **SELECT (P-3)** asks the control task to switch at the start of its next round, as the panel's
  own buttons do (`core_select()`, log "preset P page G"); the state then shows it. A newer SELECT
  replaces one not yet applied. It is not a debug command, and it changes nothing in flash
  directly: the preset and the page are part of the panel's state that is kept across a
  power-off ([config.md](config.md), "The panel's state"), saved after the quiet pause like a
  switch made on the panel.
- CFG_STATE and CFG_SELECT are not transfer commands: they do not delay the autosave.

### What the mixer sent

P-5: the editor sees the MIDI on its port but not which control sent it; two controls may send
the same message. The control task numbers every message the core sends (a u32 that starts at 0
and wraps) and keeps the last 63 with their source (`ctl_sent_read()`). An entry is 7 bytes:

| Offset | Field | |
|---|---|---|
| 0 | the message, 3 bytes | status, data 1, data 2 (0 when the message is shorter) |
| 3 | preset u8 | the source, as the panel state's touch: the preset, |
| 4 | page u8 | the binary form's page (255 the bottom row), |
| 5 | control u8 | the binary form's control ID, |
| 6 | layer u8 | 0 Normal, 1 Shift, as it was when the event came; all four 255: no control (MIDI in, the time, a selection) |

- The source is the event the core was taking: a strip or row button's press **and** release, an
  encoder's turn, a fader's move. A note-off of a button released after Shift was let go names
  the layer of the release, not of the press.
- An entry is kept when the USB queue took the message (the BLE queue may still merge or refuse
  it: [ble.md](ble.md)).
- CFG_SENT `from`: entries from that number on, at most 16; `first` is later than `from` when the
  ring no longer holds them (`first - from` were lost); `from` = `0xFFFFFFFF` gives none and
  `first` = the next number to come. SENT records are the same entries pushed: EVENTS with bit 1
  starts at the next number.
- A client joins them to the MIDI it receives by order and bytes (the editor: `js/mixer.js`); the
  port's MIDI stays what the instrument got.

### Rules

- **Versions (FR-CFG-07).** CFG_INFO gives the protocol's version, the binary form's and the
  schema's. The host refuses a device whose versions are not its own (`smcconf`: "update the
  firmware or the tools").
- **One transfer at a time, in one buffer** of 16 KB (`CFGLINK_BUF`, the build option `CFG_BUF=n`;
  CFG_INFO reports it). It takes the factory and the test configurations (7.2 and 8.4 KB on
  2026-11-12) with room to spare; the largest form possible (`CFGBIN_MAX`, 30 KB: every field of every control set, with a
  Shift layer each) would cost 14 KB more of the heap, so a larger upload is refused (`size`;
  `smcconf push` says so before sending). The owner's call: SPEC §13.1, Q-22. A BEGIN drops the transfer under way, so a host that died mid-upload is
  no obstacle; CFG_ABORT drops it too. A snapshot stays readable, at any offset and as often as
  the host likes, until the next BEGIN or ABORT.
- **Upload.** Chunks come in order: a chunk at another offset is `bad offset` and the reply says
  where the upload stands, so a host goes on from there. A retry with the same `seq` is answered
  with the same reply and not executed again (the link's rule, FR-CFG-03), so a chunk is never
  taken twice.
- **Commit.** In this order: every byte received (else `incomplete`; the upload stays), the
  CRC-32 (else `bad CRC`; the upload is over), the whole form by `cfgbin_decode()` with no
  destination (else `invalid` with its error; the upload is over; cfgbin logs where). Only then is
  it handed to the control task (`ctl_request_config()`), which first sends a Note Off for every
  note the old configuration left on (`core_notes_off()`, the owner's decision of 2026-11-12; a
  CORE_SETUP does not), then decodes it into the configuration
  and restarts the core on it at the start of its next round, as for a CORE_SETUP: the targets
  back to their defaults, the LEDs redrawn where they change (log: "configuration received: …
  bytes, revision …", then the core's "core start"). Until then the running configuration is
  untouched, so an interrupted or refused upload changes nothing (FR-CFG-04's promise, in RAM).
- **Live edit (CFG_SET, P-1).** The records are the binary form's ([config.md](config.md),
  "Edits"), for the one preset named, with no header: at most one global record, at most that
  preset's settings record, and that preset's Normal and Shift records. A setting record replaces
  those settings whole; each control named is replaced whole (no Normal record: none; no Shift
  record: the default Shift layer). The USB task checks them with `cfgbin_patch()` and no
  destination (else `invalid`, nothing changed), then hands them to the control task
  (`ctl_request_edit()`), which applies them at the start of its next round without a restart:
  `core_reconfigured()` keeps the page, Shift, the held buttons and the values of every target the
  configuration still addresses, drops the others' and adds the new ones at their defaults, and
  the LEDs follow. Before a control changes, the notes it holds on end (a Note Off each; a held
  button of it then releases nothing); a preset-settings record ends every note on, a global one
  none ([core.md](core.md), "Live edits"). The mixer advances the revision; the source
  becomes received (a setup's stays a setup's). One edit waits at a time: a second while one does
  is `busy`. 184 bytes is the most a 192-byte command carries; a host sends a larger change as
  several SETs (`codec.edits()` and the editor's `Codec.edits()` make the same list) or as a PUT.
  An edit cancels a save under way and marks the configuration unsaved.
- **Save (CFG_SAVE).** Starts saving the running configuration now: no quiet pause first (a touch
  still pauses it between steps), a failed step tried again at once, three times, then `failed`.
  ok also when nothing is to be saved (the running configuration is what the mixer starts with:
  stored, or factory with nothing saved). The reply comes before the save; CFG_INFO's storage
  follows it (`writing`, then `clean`). Save changes storage metadata, not the running revision.
  `unavailable` with no flash storage or a setup's
  configuration; `busy` while a change waits for the control task.
- **Revert (CFG_REVERT).** The mixer plays what it starts with again: the newest saved
  configuration, or the factory one when none is saved. It is loaded into the transfer buffer
  (a transfer or a snapshot under way is dropped, a save cancelled) and applied as an upload is,
  with a restart and the Note Offs first (log "configuration reverted…"); the source is then stored
  (or factory) and storage clean. Revert advances the running revision but does not write flash.
- **Busy.** While a CORE_SETUP or a committed configuration waits for the control task: GET_BEGIN
  is `busy` (the configuration may be rewritten), and while a committed one waits PUT_BEGIN,
  ABORT and COMMIT are `busy` too (the buffer holds it). A COMMIT while a setup waits is `busy`
  and keeps the upload: commit again. A CORE_SETUP while a configuration waits is `busy` (LOG-35).
  `smcconf` asks again for up to 2 s.
- **Who touches the configuration.** Only the control task writes it, and only while a change is
  pending; the USB task encodes it (GET_BEGIN) only while none is. The USB task is the only one
  that asks for changes, so the check and the encoding cannot meet a write.
- **Autosave** (`cfglink_poll()`, from the USB task every round, host or none), when the running
  configuration's `save` is `auto` (with `manual` a change leaves storage `unsaved` until a SAVE,
  which the same poll then writes). A commit or an edit marks the received configuration unsaved
  (`waiting` in auto mode, `unsaved` in manual mode). Once 3 s (`CFGLINK_QUIET_US`) have passed
  since the last transfer command (a BEGIN, a chunk, a commit; not CFG_INFO or ABORT) and since
  the last touch of a control (`ctl.last_touch_us`: a button, an encoder or a fader event, not MIDI
  in), and no change is pending, the running configuration is encoded into the transfer buffer and
  written one step per round: a 4 KB sector erased, or up to 4 pages programmed and read back, or
  the header (state `saving`, storage `writing`). A touch pauses it between steps until the panel
  has been quiet for 3 s again. A BEGIN cancels it (the slot saved before stays) and it starts over
  after the next pause; an abandoned download snapshot is dropped for it. A failed save is tried
  again after another pause, three times (`CFGLINK_SAVE_TRIES`), then storage is `failed` until
  the next upload. A configuration a CORE_SETUP replaced before it was saved is not saved: setups
  are debugging. `smcconf push --wait-save` waits for the save. With the panel's state kept
  (`cfglink_set_state()`, [config.md](config.md), "The panel's state"), the same poll writes it
  once it differs from the one in flash and the panel has been quiet for 3 s, never while a
  configuration is being saved: one task reaches the flash.
- **Not a debug command.** Unlike CORE_SETUP the protocol works with injection off.
- **Logging** (module `cfg`): INFO a download, an upload announced, an upload complete, a save
  begun and done ("configuration saved: flash slot …, generation …"), a save cancelled, the
  configuration loaded at start or none; WARN an upload dropped by a BEGIN, a size refused, a
  commit refused (and cfgbin's own WARN with where), a slot passed over at start; ERROR a failed
  flash step, giving up; DEBUG every status but ok. The control path logs the apply (module
  `engine`).
- **Costs.** On the controller the buffer is 16 KB of `.bss`: the linker's heap region is 37 984
  bytes in the build of 2026-11-11 with the flash storage (38 400 without), against 59 520 on
  controller 15 (the rest of the difference is
  the larger decoded configuration, NRPN, and the codec's state). The SDK's reported 95 KB free
  on controller 15 is virtual address space, not that physical region. A configuration is decoded
  twice, in the USB task to check it and in the control task to apply it; ❓ how long that takes
  on the device is not measured yet. A
  factory configuration (7 363 bytes) is 58 chunks each way. The virtual/physical heap distinction
  is explained in [ble.md](ble.md), "The two heap numbers"; a heap-free heartbeat alone is not
  a physical RAM budget.

## Over SysEx (P-4)

**The frame: decided by the owner 2026-11-13 (SPEC Q-28, option (a) with the tag).** The iPad and a
browser without Web Serial reach the mixer only through MIDI, so the same commands go over SysEx
on USB MIDI and BLE MIDI (SPEC FR-CFG-01, [app.md](app.md), P-4). FR-CFG-01 fixes the manufacturer
ID, `0x7D` (non-commercial use); Q-28 the rest:

```
F0 7D 53 4D <the link record and its CRC-16, 7-bit packed> F7
```

| Part | Bytes | |
|---|---|---|
| start | `F0` | |
| manufacturer | `7D` | SPEC FR-CFG-01 |
| device tag | `53 4D` ("SM") | other devices that use `7D` ignore ours, and we theirs: a SysEx without this tag is not a link frame |
| body | the record of "Frames and records" (CMD: `10` seq cmd payload; REPLY: `11` seq cmd status payload) followed by its CRC-16/CCITT-FALSE (little-endian), packed 7 bits to a byte | the same records, commands, `seq` and retries as on the CDC: only the framing differs (the CDC's COBS and `0x00` become `F0 … F7`) |
| end | `F7` | |

- **Packing.** Each group of up to 7 bytes becomes 1 + n bytes: first a byte whose bit i (0–6) is
  the top bit of the group's byte i, then the group's bytes with their top bit cleared. 192 bytes
  of record and 2 of CRC become at most 222; a whole frame is at most 227 bytes, within FR-CFG-02's
  256.
- **What goes over SysEx.** CMD, REPLY, EVENT and SENT records: every command of the CDC link (PING,
  GET_INFO, the configuration protocol, injection while it is on); the log stream stays on the CDC
  (LOG_SUBSCRIBE answers `unavailable` over SysEx until LOG-12's SysEx transport is built).
- **Where the reply goes.** On the transport the command came from (USB MIDI or BLE MIDI); each
  transport has its own link state (the last `seq`, the reply waiting). The configuration's transfer
  is one for the whole device: a BEGIN from any transport drops a transfer under way, as today.
- **Not MIDI for the core.** A link frame is never passed to the core, never echoed, never counted as
  MIDI in; a SysEx that is not ours is ignored (counted).
- **A worked frame** (the C and Python tests' fixed vector): PING, seq 7, payload `80 FF 00` is the
  record `10 07 01 80 FF 00`, CRC-16 `BD51` (little-endian `51 BD`), on the wire
  `F0 7D 53 4D 18 10 07 01 00 7F 00 51 00 3D F7`.

**Built (I-2, 2026-11-13); on the mixer since controller 31 (I-4, below):**

| Part | Where | What it does |
|---|---|---|
| The framing | `firmware/link/link.c`: `link_init_sysex()`, `link_pack7()`, `link_unpack7()` | the same link with SysEx framing: `link_rx()` takes the SysEx bytes a transport received (F0 opens, F7 ends, a real-time byte inside is skipped, any other status ends the message unfinished and counts it bad; data outside a SysEx is ignored); a SysEx without `7D 53 4D` is counted `foreign`; `link_tx()` gives one whole SysEx message per call, a reply, EVENT or SENT (at most 227 bytes; BLE MIDI's sender takes one message at a time and refuses bytes after its F7); GET_INFO leaves out the log capability, LOG_SUBSCRIBE answers `unavailable` |
| USB MIDI | `firmware/app/usb/usb_app.c` | packets on cable 1 ("SMC-Mixer Config"): their bytes (`usbmidi_packet_bytes()`) to the config port's link; its replies go back on cable 1 (`usbmidi_sysex_pack()`: CIN 4 while the end is further on, 5–7 with the F7), a packet at a time as TinyUSB takes them. While a reply waits, no packet is read (the performance port's wait in TinyUSB's FIFO too, a few ms). The link starts again when the MIDI function goes away |
| BLE MIDI | `firmware/app/ble/ble_br25.c`, `firmware/link/sxpipe.c` | SysEx the BLE stack received goes through a pipe (512 bytes, lock-free, one producer and one consumer each way) to the USB task, which runs a third link; the reply comes back through a one-reply slot and the BLE sender puts it into notifications ahead of the controls' messages (a SysEx begun is finished before any other message, BLE-MIDI's rule). A central connecting or leaving starts that link again and drops a reply waiting for the old one. Runs without USB (the USB task runs on a power adapter too) |
| One task | `usb_app.c` | every link (CDC, USB MIDI, BLE MIDI) and so the configuration protocol run in the USB task: no lock in `cfglink.c` |
| Statistics | `usb_app_log_stats()` | per link: frames, bad, too long, not commands, foreign SysEx, commands, retries, replies; BLE: bytes the pipe could not take |
| Host | `tools/hostmidi/hostmidi.py`, `tools/smclog/smclog.py` (`sysex_command`, `sysex_record`, `SysexLink`), `smcconf … --midi PORT` | the same commands as over the CDC on a MIDI port: by the name the OS gives it (CoreMIDI, ALSA rawmidi, WinMM, through `ctypes`), or `fd:N` / `raw:PATH` for a byte stream |
| The editor | `app/editor/js/link.js` (I-3) | the same frame and configuration commands in JavaScript over Web MIDI; used by the page since I-5 (✅ on the mixer, 2026-11-13); `tools/editortest/link.mjs` checks the T-2 transcript, and T-2L exercises the client live against devhost |
| Host tests | `firmware/tests/test_link.c` (`test_sysex_*`, `test_sxpipe`), `test_midi.c` (`test_sysex_packets`), `tools/smcconf/test_device.py` (`SysexTest`: smcconf over SysEx against `devhost --sysex-fd`), `tools/hostmidi/test_hostmidi.py`, `tools/editortest/transcripts.py` (the T-2 transcript, [integration.md](integration.md)) | |

The editor's `LinkClient.close()` is local cancellation, not a new wire command: stop retries
and reject queued/future calls, so an interrupted configuration upload sends no later chunk or
commit. A reply accepted before close remains a valid acknowledgement; already-sent commands
cannot be undone. Late callbacks are ignored, send failures release their waiter/timer and are
`LinkError`s, and Web MIDI cleanup detaches only the listener that transport owns. The device's
framing, sequence/retry semantics and protocol version are unchanged. `tools/editortest/link.mjs`
checks these cases with in-memory transports and model uploads; physical reconnect is not checked.
This does not define cancellation of the separate M-Vave firmware installer.

On macOS, the host backend uses CoreMIDI's `displayName`, falling back to `name` if the display
name is missing, empty or cannot be converted. A newly enumerated controller can have raw endpoint
names `""` and `"Config"`; its display names are `"SMC-Mixer"` and `"SMC-Mixer Config"`. ✅ Read
on the mixer, 2026-11-15, build `a628aa6a` with USB serial `C1`; after this host-side fix,
`smclog cmd --midi "SMC-Mixer Config" info` answered. Host tests cover those shortened names,
missing/empty display names, failed conversion and releasing the CoreFoundation strings.

**Host-platform corrections (R03–R05):** file-only `smcconf` commands do not import device
transports, and `smclog` imports `termios`/`tty` only when configuring a POSIX terminal. Fake
Windows routing tests reach the WinMM backend with those modules unavailable. ALSA send retries
only EAGAIN/EINTR (and zero-progress writes), preserving partial-write offsets; write and drain
share one two-second monotonic deadline. Permanent errors raise `MidiError` with the operation
and errno name. WinMM queues short `MIM_DATA` and SysEx buffer completions in callback order;
the worker decodes channel-message lengths (including two-byte Program Change/Channel Pressure),
feeds the existing parsers and returns SysEx buffers. No native API is called by the callback.
These regressions pass with blocked imports and fake APIs on macOS, without opening native
ports. Native Windows/ALSA device validation remains unrun; this is not hardware certification.

**On the mixer (controller 31, 2026-11-13, `make e2e` and `smcconf --midi`):** ✅ USB MIDI:
`smcconf info`, pull (7 363 bytes in 0.24 s) and push with a read-back (8 556 bytes in 0.47 s);
`make e2e` over SysEx and over the CDC, four configurations each (7.4 to 13.8 KB), every push
applied with its revision and ID and every pull byte for byte, no WARN or ERROR in the log
(`logs/e2e/2026-11-13-*`, local). ✅ BLE MIDI (macOS's "Bluetooth" port): `smcconf` pull of 7 363 bytes in 2.7 s, push with a read-back in 5.7 s (about 45 ms a chunk's round trip: the connection interval); `make e2e E2E_ARGS="--ble Bluetooth"` with the same four configurations, every check. ⚠️ Not run: ALSA and WinMM (no Linux or Windows host
here).

- **Not taken** (Q-28): (b) the SPEC's literal layout, `F0 7D <tag> <cmd> <seq> <payload, packed>
  <crc> F7` with a 7-bit `seq`: a second record format beside the CDC's; (c) no device tag: shorter,
  but our frames and other `7D` devices' can be taken for each other on a shared bus (a MIDI
  interface, a host that routes everything).

## On USB (the application)

`firmware/app/usb/usb_app.c`, with TinyUSB 0.21 and our BR25 controller driver
([usb.md](usb.md)):

- **Descriptors.** One configuration with a CDC ACM function (interfaces 0, control, and 1,
  data) and a USB MIDI function (interfaces 2, audio control, and 3, MIDI streaming). The CDC
  endpoints are notification `0x81`, data OUT `0x02`, data IN `0x83`; MIDI's are `0x04` and
  `0x84` ([usb.md](usb.md), "Endpoints in the application" and "USB MIDI"). The device uses the
  pid.codes test ID `1209:0001` (development builds only, SPEC Q-14). The product string is
  `SMC-Mixer` or `SMC-Mixer hardware test`, the CDC interface's `SMC-Mixer link`.
- **The interrupt.** IRQ 9, priority 3, as the SDK's own USB device uses. The handler runs
  `dcd_int_handler()` and posts a semaphore to the USB task. It is installed and masked before
  `tusb_init()`, which attaches the device and unmasks it once the stack is ready
  ([usb.md](usb.md), item 6).
- **The task** `smc_usb`: priority 3 (above `app_core` and `smc_ctl`, equal to `btstack` and
  `smc_ble`), 512 words of stack (`app_main.c`, since controller 11).
  It waits on the semaphore, at most one OS tick (10 ms); the controller's control task posts it
  too when MIDI packets wait. Each time it runs:
  1. `tud_task()`;
  2. USB MIDI: packets from the host in, queued packets out ([usb.md](usb.md), "USB MIDI");
  3. the BLE SysEx pipe and its link (also without a mounted USB host);
  4. the CDC link, up to 8 rounds: the receive FIFO → `link_rx()`, then `link_tx()` → the CDC
     transmit FIFO, never more than its free room.

  Configuration/panel-state storage polling and deferred reset service also run each task wake.

  So a log record reaches the host within about 10 ms.
- **FIFOs.** Receive 256 bytes, transmit 1024 bytes. The transmit FIFO never overwrites
  (`CFG_TUD_CDC_TX_OVERWRITABLE_IF_NOT_CONNECTED` 0): what does not fit stays in the log ring.
- **Power.** The board configuration keeps the chip out of power-down
  (`TCFG_LOWPOWER_LOWPOWER_SEL` 0). The board's sleep hook would switch the USB pins to GPIO, so
  a power-down mode needs a USB check first.
- The application's sizes: [build.md](build.md).

## Host tool

```sh
python3 -I tools/smclog/smclog.py cmd --port /dev/cu.usbmodemXXXX info
python3 -I tools/smclog/smclog.py cmd --port /dev/cu.usbmodemXXXX --binary build/hwtest/app.elf level input debug
python3 -I tools/smclog/smclog.py cmd --port /dev/cu.usbmodemXXXX inject on
python3 -I tools/smclog/smclog.py cmd --port /dev/cu.usbmodemXXXX inject button:3:press encoder:1:-2
python3 -I tools/smclog/smclog.py cmd --port /dev/cu.usbmodemXXXX setup config=test note_off_p2=8n
python3 -I tools/smclog/smclog.py capture --port /dev/cu.usbmodemXXXX --dict build/hwtest/app.smclog.json \
    --session logs/sessions/2026-11-09-hwtest
```

- `cmd` sends one command and prints its reply (`--json` for the decoded record). `--midi PORT`
  instead of `--port` sends it as SysEx on a MIDI port (`tools/hostmidi`), USB MIDI or BLE MIDI
  (on macOS the BLE port is named `Bluetooth`); the log commands answer `unavailable` there.
  ✅ `info`, `stock check` and `stock back` over BLE MIDI (controller 36 over V013, 2026-11-14:
  `back` ok, 1 try, pointer `0x1000`; the mixer restarted as M-Vave's `SMC-Mixer_013`).
  - Commands: `ping`, `info`, `subscribe`, `unsubscribe`, `level MODULE LEVEL`, `levels`,
    `raw off|matrix|faders|on|probe1..probe8`, `metrics`, `boot`, `reset soft|fault|watchdog`,
    `stock check|back` (back writes sector 0: a device action that needs the owner's go-ahead),
    `inject on|off|EVENT...`,
    `setup [NAME=VALUE...]` (no arguments: the factory configuration).
  - Injection events: `button:N:press|release`, `encoder:N:CLICKS`, `fader:N:VALUE`,
    `midi:usb|ble:B0,B1,B2` (hex).
  - Module names come from the dictionary (`--dict` or `--binary`; it now records each call
    site's module index). Numbers and `all` work without one.
  - It exits non-zero on any status but ok, and on no reply after 3 tries.
- `capture` subscribes at the start and unsubscribes at the end. Every second (`--sync`) it sends
  a PING carrying its own send time. The reply in `device.jsonl` then holds three times:
  - `t_host_send_ns`, when the host sent it;
  - `t_us`, the device's clock;
  - `t_host_ns`, when the host got it.

  These are the samples for clock synchronization (LOG-20): `smclog clock SESSION` fits the
  device's clock to the host's ([logging.md](logging.md), "The device's clock on the host").
  `--no-subscribe` only listens: for a stream that runs by itself, as in the older
  tests.

## Tests

- `firmware/tests/test_link.c` (`make -C firmware test`) covers:
  - every command and its errors;
  - the stream: nothing before the subscription, the header first, nothing after unsubscribing;
  - retries, including after a reset;
  - one reply at a time;
  - bad, too-long and cut frames, and records that are not commands;
  - whole frames for any room;
  - EVENTS (`test_events`): none without the configuration protocol or its state, the first at
    once, nothing while the state stands, one per 40 ms with only the newest state, an EVENT that
    waits whole for room, a reply before a due EVENT, off, on again (the count from 0), a reset,
    and the same over SysEx, one message per call;
  - SENT (`test_sent`): unavailable without CFG_SENT, nothing until an entry comes, one record per
    20 ms, 16 entries at most, the oldest skipped and counted (at most 255), a record that waits
    whole for room, the reply, the EVENT and the SENT in that order, off, a reset, and over SysEx
    one message per call.

  It decodes the device's output with its own COBS/CRC code.
- `firmware/tests/test_cfglink.c`: the configuration protocol through the link and the control
  path with the core, as the controller wires them: CFG_INFO and the capability, a download equal
  to the encoder's bytes, reads at and past the end, an upload applied in the next round with its
  ID and revision (the core then plays it, a download gives it back), busy while it waits, every
  refusal (sizes, order, a retry not taken twice, past the length, incomplete, CRC, invalid with
  cfgbin's error, a BEGIN that drops an upload) with the running configuration unchanged, a commit
  while a setup waits. The autosave on a model of NOR flash: nothing before 3 s, held off by a
  touch and paused by one while writing, saved once, loaded at the next start with its ID and
  revision (source stored), the next save into the other slot; a transfer that cancels a save, a
  setup that replaces the configuration (not saved), an abandoned snapshot; failed saves tried
  again and given up. Protocol 2 (`test_edit`, `test_save_revert`): a SET applied in the next round
  without a restart (the page and a target's value kept, nothing sent) with its revision and
  source, busy while one waits, refusals (another preset, a broken record, too short) with
  nothing changed, a held note's button changed, a setup's configuration staying a setup's; with
  `manual` nothing saved until a SAVE, then saved at once, SAVE again while it runs; REVERT to the
  saved configuration with its ID and revision, and to the factory one with nothing saved; the
  setting turned to `auto` and back by edits; a failed SAVE tried again at once, then failed; a
  setup's configuration never saved. Protocol 3 (`test_panel`): CFG_STATE at start, SELECT applied
  in the next round (refused out of range, bad length), not a transfer command, the revision and ID
  of an upload, a touch counted with its address, and a host without the panel (STATE and SELECT
  unavailable, EVENTS too). Protocol 4: CFG_SENT gives what a touch sent with its control, "now"
gives the next number, a bad length, not a transfer command; unavailable without the panel.
`test_ctl.c`'s `test_sent` covers the ring: a press and a release named alike, a selection and the
row on Shift sending nothing, an encoder on another preset and page, the oldest skipped. It also covers `ctl_request_config()`, `ctl_encode_config()`, the last
  touch and `ctl_set_stored()`. The slots themselves: `test_cfgstore.c` ([config.md](config.md),
  "Storage").
- `firmware/tests/test_app_descriptors.c` walks the application's descriptors as a host would:
  - the IAD class triple;
  - four interfaces (CDC control and data, audio control, MIDI streaming);
  - five endpoints, bulk and interrupt only, within the driver's endpoints; only MIDI's endpoint 4
    is used both ways;
  - eight MIDI jacks, two per MIDI endpoint, and the port names;
  - the strings, including a product name cut at 32 characters.
- `tools/smclog/test_smclog.py`:
  - reply decoding, command frames, injection events, module and level names, and no reply;
  - `test_link --serve`: the C link, with stand-in variant functions and a log record every
    100 ms, on a pseudo-terminal's master side. `cmd` and `capture` run against it: the info and
    the build ID, PING, levels, raw mode, metrics, injection, setup (accepted and refused), and a
    capture with its subscription, header, records and sync replies.
- `tools/smcconf/test_device.py`: `smcconf info`, `pull` and `push` against `devhost`: the factory
  configuration pulled is `config/factory.json` byte for byte; a pushed file runs (the core sends
  its notes to the host) and pulls back the same, with its ID and revision; a binary file; refusals
  on the device (magic, CRC) and here (a file that is not valid, before the device is asked); an
  upload cut off, then a pull and a whole push; a device without the protocol (`test_link
  --serve`) and a port with nothing behind it. With `devhost --flash FILE` (a model of NOR flash
  in a file): `push --wait-save` saves after the pause, a RESET and a new process run the stored
  configuration; injected encoder turns hold the save off.
- `tools/smclog/test_firstrun.py`: the first run (`firstrun.py`, [hwtest.md](hwtest.md), "The
  first run") against `devhost`, whose RESET restarts the process in place on the same keep file:
  all three resets, the RAM kept, the replay, the crash record. Also a port with nothing behind it,
  another build, the debug resets refused without injection, and the frame check on the image's
  own symbols.

## Not checked without hardware

This is the original hardware checklist, not a list of wholly unrun features. ✅ Enumeration,
info, streaming, PING and coexistence with the scan ran on 2026-11-11 (`firstrun.py` and the
12-minute manual run, [hwtest.md](hwtest.md)). Later configuration E2E and browser runs are in
[integration.md](integration.md); read-only USB captures of 45 minutes and two hours on
2026-11-15 answered every PING, with no log loss in complete capture windows. A deliberate unread-port overflow/recovery, every
reconnect/reset boundary and separately timed application/heap costs are not certified by those
runs. `firstrun.py` covers items 1 and 3, part of 2 and the resets; repeat only with permission.

1. Enumeration on macOS:
   - the CDC port appears as `/dev/cu.usbmodem*`;
   - `cmd info` answers with the image's build ID.
2. The stream:
   - `capture` gets the header and the boot records;
   - no `bad_frame` and no `gap` in a 10-minute run with raw input on.
3. The latency of a record (about 10 ms expected) and the PING round trip.
4. Back-pressure: stop reading without closing the port; there must be LOST records and no
   corrupted frames when reading resumes.
5. A close and reopen of the port, and a bus reset (unplug, plug): the stream stops and comes
   back on a new subscription.
6. That the USB interrupt and task coexist with the scan (TIMER3, priority 7): the scan's
   `late max` and `skipped` stay as without USB traffic.
7. The configuration protocol on the controller (✅ controller 20, 2026-11-12: `info`, `pull` equal
   to `config/factory.json`, `push config/test.json` and back, each applied and read back the same;
   the times and the heap not measured separately): `smcconf info`, `pull`
   (equal to `config/factory.json`), `push config/test.json` and back, the time a push takes and
   the control task's longest round while it applies (the metrics heartbeat), the heap afterwards.
8. The autosave on the controller (✅ controller 20, 2026-11-12, the first flash write by our
   firmware: [config.md](config.md), "On the device", "The first run"; the USB task's longest wake
   during a save not read separately):
   `push --wait-save`, the log's "configuration saved", a power cycle and `info` saying `stored`;
   the time a save takes, and the scan's `late max` and `skipped` and the USB task's longest wake
   while it erases (the steps run in the USB task). The order and the cautions:
   [config.md](config.md), "Storage", "On the device".
9. STOCK on the controller, in two runs:
   - `stock check` (writes nothing): `ok` with pointer `0x41000` on the installed mixer. It shows
     that `sfc_read` gives sector 0 and M-Vave's area as stored ([image.md](image.md), "Going
     back"). ✅ Controller 36, 2026-11-14: `stock: ok; sector 0 points at 0x41000`.
   - `stock back` (writes sector 0: the owner's go-ahead, the recovery key at hand): `ok`, tries 1,
     the mixer restarting on M-Vave's firmware (its SysEx name answers), then the editor's install
     again (`earlier`). ✅ Controller 36, 2026-11-14: ok, tries 1, pointer `0x1000`; the mixer came
     up as `SINCO`, `SMC-Mixer_012`. ✅ Then the editor's page in Chrome installed it again
     (`earlier`, 5.8 s): after a power cycle 0.1.0 runs with the saved configuration.
