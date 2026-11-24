# Joining the editor and the firmware

How the editor ([app.md](app.md)) and the firmware are joined into one product, and how both are
checked together quickly and automatically. The contract they share and the rule for changing it:
[app.md](app.md), "One product". This file records the implemented test layers and their dated
evidence, not an unstarted plan. Built: T-1 (I-1), P-4 and T-2's transcript (I-2), the
editor's link client against it (I-3), `make e2e` on the mixer over USB MIDI, the CDC and BLE MIDI
(I-4, ✅ controller 31), the page on the mixer with the editor's own binary form (I-5, ✅), all
2026-11-13. I-6 (P-1's live edits, Q-26's SAVE, REVERT and the setting `save`) is built on both
sides and ✅ ran on the mixer (controller 32, 2026-11-13: `make e2e` over USB MIDI and BLE MIDI,
`smcsim device`, the page in Chrome). I-7's events/selection and named MIDI followed on
2026-11-14; the protocol is now 4, binary form 1, schema 1, product 0.1.0.

## Hardware results, 0.1.1 (2026-11-24)

The 0.1.1 candidate (controller-ble `f6739e31`, the 0.1.0 firmware with the new version number) on
the same retail mixer, Chrome on macOS: installed through the editor over USB (stock V013 after an
interrupted update session: backup, install, physical off/on; the saved configuration kept).
`make e2e` 37/37 over USB once the faders rested at the bottom: two earlier runs failed only
where a fader resting mid-travel sent values during the run ("no real input during the run"),
the gen-2 comparison once more on one message. The page self-test 30/30 against the factory
baseline, served from the repository (`app/editor/serve.py`): served from a release package it
cannot read `test.json` (the package carries only `factory.json` and `schema.json`), so "open a
file: sent whole" fails there. The owner's configuration restored and verified after each run.

## Hardware results, 2026-11-23

The release candidate on a retail mixer, Chrome on macOS:

- **Firmware cycles.** Install, update and Go back through the editor over USB (builds
  `3105a3b5`, `cd06d825`, `746dd090`, `8e2ee8c4`, each with a fresh 512 KB backup and physical
  off/on) and over Bluetooth with USB unplugged (`8e2ee8c4`: Go back over BLE MIDI, stock Web
  Bluetooth AE40, backup, install and read-back, off/on, ours over BLE MIDI again, driven by
  `tools/fwcycle/`). The configuration read back byte for byte equal to the one before after the
  first USB update (`3105a3b5`) and the Bluetooth cycle, apart from the revision: the mixer
  assigns revisions (P-6), and a restart boots the saved one. The saved configuration was kept
  across the other installs.
- **T-3:** `make e2e` 37/37 over USB on `cd06d825` and `8e2ee8c4`. On `3105a3b5` one run gave
  36/37 ("EVENTS on, the state at once" got no state once); six further runs and five direct
  probes did not repeat it.
- **T-4:** the full page self-test 29/29 on the mixer against the factory baseline. (A first run
  from a non-factory baseline gave 25/29: the self-test assumes the factory configuration.)
- **Scenarios:** all 19 core scenarios on the device (`smcsim device`), `note-cleanup-target` and
  `live-edit` among them, equal to the simulator.
- **P-6 live:** an `smcconf push` of the same file with the same ID raised the revision on the
  mixer (17→18, 18→19), and the open editor showed "The editor and the mixer differ".
- **R01 under load:** a configuration with all 128 strip buttons as note toggles, 128 notes held,
  then the whole configuration replaced: 128 distinct Note Off reached the host, none missing.

The original configuration was restored and saved after each run. Not run on the mixer: Learn
with a real instrument; native Windows and Linux routes.

**Earlier, 2026-11-15:** native macOS `make test` with required editor tests and native-arm64
Docker `make linux` passed (Linux reported 17 explicit skips; this is not native Windows/ALSA
integration). Physical USB and BLE MIDI E2E passed 37 checks each, the CDC smoke 14, restoring
the running and stored configuration content. Targeted editor-client checks in installed Chrome
passed 10/10 over each real USB and BLE MIDI endpoint. The physical page self-test then gave
26/29 ("Physical page and client checks (2026-11-15)" below). A stock Web Bluetooth GATT connection failed
before any write; its error was masked by assigning to `DOMException.message`, fixed since. A
review of 2026-11-15 found defects R01–R12; all are fixed and verified (host and browser
regressions, and the hardware runs above).

## P-6 follow-up: mixer-owned revision

The owner decided on 2026-11-20 that the mixer increments its running revision, not the
client. Upload, each SET and Revert advance it at application; Save only persists the running
revision. Boot takes the saved revision unchanged. See [link.md](link.md), "The panel's state",
for wrap, pending state and unchanged wire formats. The editor uses the confirmed identity and
invalidates its edit baseline when an outside write moves it. The CLI verifies the content and
reported identity rather than expecting the file's revision.

The C link test sends two different configurations with the same ID and a reused revision,
checks that live SETs advance and refusals do not, and covers Save, Revert and a simulated boot.
The Node/devhost live test exercises an outside upload reusing the running ID and revision,
checks that the editor detects it, and verifies that Revert adopts the returned identity. ✅ On
the mixer 2026-11-23 ("Hardware results, 2026-11-23").

## I-7: the panel's state, events and selection (2026-11-13)

P-2, P-3 and the firmware's part of P-6, built on both sides in one step; P-5 (what the mixer sent,
with its source) after them, 2026-11-14 (below, "P-5").

- **The definition**: [link.md](link.md): the EVENT record (`0x12`), EVENTS (`0x28`, capability
  bit 8), CFG_STATE (`0x4A`) and CFG_SELECT (`0x4B`), configuration protocol 3, and "The panel's
  state": 30 bytes with the preset, page, Shift, Bluetooth, the touches counted and the last one's
  binary-form address and layer, the storage, the running configuration's revision and ID. One
  EVENT per 40 ms per link at most, each the whole state.
- **The firmware**: `ctl` keeps the panel in one word (`ctl_panel()`, the touch noted before the
  core takes the event) and applies a selection a client asked for at the start of its next round
  (`ctl_request_select()`, log "select asked: …", then the core's "preset P page G"); cfglink
  answers STATE and SELECT (not transfer commands: the autosave does not wait for them); the link
  frames the EVENTs on every transport (the CDC, SysEx on USB MIDI and BLE MIDI). The controller and
  devhost wire them.
- **The editor**: `js/link.js` (`state()`, `select()`, `watch()`, `onEvent`, `decodeEvent()`,
  `panelState()`), `js/mixer.js` (EVENTS on at connect; the tabs follow the mixer's preset, page
  and Shift; a control touched on the mixer is selected as a click selects it; the storage state
  from the EVENTs; a tab switches the mixer by CFG_SELECT; a revision that moves without the editor
  is reported, "changed by another app"; a PUT takes the next revision and keeps the mixer's ID).
- **Host tools**: `smcconf state`, `select`, `watch`; `smclog`'s links keep EVENTs (`poll()`).

Added to the layers:

- **T-1**: `test_ctl.c` (the panel word, touches by kind and layer, the row, selections),
  `test_link.c` (EVENTS: the first at once, the gap, only the newest, no room, a reply first, off,
  reset, SysEx), `test_cfglink.c` (STATE, SELECT, revision and ID, a touch, a host without the
  panel); the scenario `remote-select.jsonl` (the simulator, and the device runner, where a
  `select` step is now a CFG_SELECT instead of a skip); `test_device.py` (`smcconf state`,
  `select`, `watch` against devhost); `test_e2e.py` (step 4 against devhost).
- **T-2**: the transcript asks the state, turns EVENTS on, selects twice, injects an encoder turn
  and a row button, has a selection refused; `link.mjs` makes the same session and reads the
  EVENTs' counts, states and touches.
- **T-3**: `make e2e` step 4 (`--no-panel` skips it), over the configuration's transport: EVENTS
  on, SELECT, two encoder touches and a row button injected over the same link, EVENTS off,
  CFG_STATE as the last EVENT.

**Runs on the mixer (controller 33, build `e33a1155`, 2026-11-13; `logs/e2e/2026-11-13-*`,
`logs/device/2026-11-13-app33-scenarios`, local):**

- ✅ `smcconf state`, `select`, `watch` over the CDC and over SysEx on USB MIDI ("SMC-Mixer
  Config"); a preset out of range refused.
- ✅ `make e2e` over USB MIDI: 36 of 36, step 4 included (the first EVENT at once, a SELECT seen
  in an EVENT 40 ms later, two encoder touches and a row button named, EVENTS off).
- ✅ `smcsim device`: 17 of 18 with `remote-select.jsonl` (`bluetooth.jsonl` needs no BLE central
  connected, as on controller 32).
- ❌ `make e2e` over BLE MIDI: 31 of 31 up to step 4, then EVENTS got no reply and the link over
  BLE never answered again until a restart (the mixer itself ran on; the CDC and USB MIDI kept
  working). The cause: a SysEx link's `link_tx()` gave the reply to EVENTS and the first EVENT in
  one buffer; BLE's sender takes one SysEx message per slot and refused the bytes after its F7,
  and the encoder stayed inside the unfinished SysEx, so it refused every reply after it. The
  device's log showed it: "replies out" stopped while "messages in" grew. USB MIDI was not hit
  (its sender packs a byte stream). The fix (controller 34): a SysEx link gives one message per
  call (`link.c`, `test_link.c`), and BLE's sender ends a SysEx it refused and counts it
  ("… replies out, N refused", `ble_br25.c`).

**Controller 34 (build `28e1090d`, the fix, 2026-11-14; `logs/e2e/2026-11-14-*`, local):**

- ✅ `make e2e` over BLE MIDI: 36 of 36 (step 4: a SELECT seen in an EVENT 23 ms later; the
  live edits 3.55 s), and over USB MIDI: 36 of 36. `smcconf watch`, then `select` and `state`
  over BLE answer.
- ✅ The page in Chrome over USB: the tabs follow a SELECT sent by another client (preset 2,
  page 3); a tab switches the mixer (preset 4, page 2, read back by `smcconf state`); an encoder
  turn on the mixer selects the control in the sheet ("Encoder 6", the monitor "CC 115 = 65 ch 4 ·
  the mixer"); the storage capsule follows the EVENTs ("Not saved" after `smcconf edit` from the
  CDC, "Saved" after `smcconf revert`).
- Found: an edit by another client that keeps the revision (`smcconf edit` writes the file's
  own, 0) is not seen as "changed by another app": the page went on showing the factory
  configuration while the mixer ran the test one, only the capsule changed. The editor follows
  the rule as built (a revision that moves). This dated observation is separate from the later
  R08 defect: an EVENT with a changed ID but unchanged revision is also missed. Both are
  superseded by the owner's P-6 decision of 2026-11-20 (the mixer assigns the revision on every
  accepted change, whoever sends it), built and ✅ run on the mixer 2026-11-23: see "P-6
  follow-up: mixer-owned revision" above.

### P-5: what the mixer sent (2026-11-14)

- **The definition**: [link.md](link.md), "What the mixer sent": the SENT record (`0x13`), EVENTS'
  bit 1 (capability bit 9), CFG_SENT (`0x4C`), configuration protocol 4. Each message the core
  sent, numbered, with the control that sent it in the binary form's address (the touch's fields:
  preset, page, control, layer; 255 none), up to 16 a record, one record per 20 ms per link.
- **The firmware**: `ctl` keeps the last 63 in a ring written by the control task and read by the
  USB task without a lock (`ctl_sent_read()`: a reader that falls behind skips the oldest, an entry
  the writer may be refilling is not given); the source is the event the core takes (a press and a
  release alike). cfglink answers CFG_SENT; the link frames SENT records after a reply and an EVENT.
- **The editor**: `js/link.js` (`decodeSent()`, `sentEntries()`, `watch(on, sent)`, `onSent`,
  `sentSince()`), `js/mixer.js` (SENT on at connect when the mixer has it; each message from the
  port takes the first entry with its bytes, either may come first; a message waits 250 ms for its
  entry, then goes as "the mixer"; entries the port never shows, merged away on BLE, are passed
  over), `js/app.js` (the monitor names the control, its layer and its name).
- **Host tools**: `smcconf watch --sent`; `devlink.Device.sent()`, `sent_since()`.
- **Tests**: T-1 `test_ctl.c`, `test_link.c`, `test_cfglink.c` (`test_sent`, `test_panel`),
  `test_device.py` (`test_sent` against devhost: the entries, CFG_SENT, the MIDI file equal);
  T-2 the transcript (SENT on, three entries named, CFG_SENT from 0) and `link.mjs` (the
  records decoded, the session, and `sentJoin()`: the monitor's join in `mixer.js` under Node);
  T-3 `make e2e` step 4 (the encoder's messages, then the row's, every one named, none lost, the
  numbers consecutive, CFG_SENT gives the same; how many each sends is the configuration's, so the
  check asks for the row's press and the order, not a count).

**Controller 35 (build `b91e784b`, 2026-11-14; `logs/e2e/2026-11-14-033328` and `-033931`, local):**
✅ `make e2e` 37 of 37 over USB MIDI and over BLE MIDI (step 4 on the test configuration's preset 3
page 2: Rew's press named by the row's page and index; encoder 6 there divides by 3, so a click and
back sends nothing, which the first run's check wrongly required: fixed in the check, not the
firmware); ✅ `smcconf watch --sent` over the CDC named the encoder (control 13) and M1's press and
release (control 16); ✅ the page in Chrome over USB: with the monitor open, injected events showed
as "Fader 3", "S 3" (press, release), "Encoder 1", each with its message. Not checked: the monitor
in Chrome over BLE MIDI at that time (the merged-entry join case was checked in Node). Later
client probes (2026-11-15) named real performance MIDI over both USB and BLE; the whole monitor
page over BLE under load has not been checked separately.

## I-6: live edits and saving (2026-11-13)

Built on both sides in one step (the contract's rule, [app.md](app.md), "One product"):

- **The definition**: protocol 2 ([link.md](link.md), "Configuration": CFG_SET 0x47, CFG_SAVE
  0x48, CFG_REVERT 0x49, status 8 unavailable, storage 5 unsaved), the edit records
  ([config.md](config.md), "Edits"), the global setting `save` (`manual` by default, `auto`) in
  `config/schema.json`; the core's side ([core.md](core.md), "Live edits").
- **The firmware**: `cfgbin_patch()`, `ctl_request_edit()` / `ctl_request_revert()`,
  `core_reconfigured()` / `core_release_control()`, cfglink's SET, SAVE, REVERT and the manual
  mode; devhost and the controller wire them.
- **The editor**: `js/cfgbin.js` `Codec.edits()`, `js/link.js` `set()`, `edit()`, `save()`,
  `revert()`, `js/mixer.js` (a change of up to 8 SETs live, else a PUT; Save; Go back; the setting
  read from the configuration the mixer plays), `js/mock.js` the same model, `js/app.js` (the
  setting is an edit of `global.save`, with undo; Go back waits for the mixer).

Added to the layers:

- **T-1**: `vectors/edits.json` (23 cases from `codec.edits()`), `run.mjs` holds the editor's
  `Codec.edits()` to it byte for byte; `test_smcconf.py` holds `codec.edits()` to C's
  `cfgbin_patch()` on random pairs; the scenario `live-edit.jsonl` (the simulator, and the device
  runner on devhost: an `edit` step is a CFG_SET).
- **T-2**: the transcript (`link-v1.json`) now saves, edits, has an edit refused and reverts;
  `link.mjs` makes the same session with `LinkClient`'s new calls.
- **T-3**: `make e2e` step 3 (`--edit FROM:TO`, factory:gen-2 by default): FROM pushed, turned into
  TO by live edits, pulled byte for byte, the probe plan on it against the simulator's TO; SAVE,
  another edit, REVERT.
- **T-4**: the page with the mock (`?mock`, the built-in browser): the setting, "Not saved", Go
  back; and with the mixer (below).

**On the mixer** (controller 32, `build/app-ble-edit32`, installed through the key on 2026-11-13,
`logs/recovery/app-32-ble-edit-write.jsonl`):

- ✅ `smcconf edit`, `save`, `revert` over USB MIDI (an edit: 1 SET of 9 bytes, 0.47 s with the
  pull before and the read-back after) and over BLE MIDI (6.4 s, the two pulls of 7 KB take most);
  storage `unsaved` after an edit, `clean` after a save, REVERT to the saved one and to the one
  saved before.
- ✅ `make e2e` over USB MIDI (`logs/e2e/2026-11-13-223923`) and over BLE MIDI
  (`logs/e2e/2026-11-13-224551`): 31 of 31 checks each. Step 3: the factory configuration turned
  into gen-2 by 80 live edits (13 710 bytes) in 2.3 s over USB, 4.8 s over BLE; pulled byte for
  byte; the probe plan on it equal to the simulator's (1 795 messages); SAVE (the generation one
  more), another edit `unsaved`, REVERT the saved gen-2 byte for byte.
- ✅ `smcsim device`, the corpus over the CDC with CoreMIDI: 16 of 17, `live-edit.jsonl` passing
  with the device and the simulator agreeing (`logs/device/2026-11-13-app32-scenarios`).
  `bluetooth.jsonl` failed only because macOS was connected over BLE (its LED on, not blinking;
  the scenario needs no central connected) and it turned Bluetooth off, which disconnected macOS.
- ✅ The page in Chrome over USB: connect ("the mixer plays this configuration"), an edit in the
  inspector reached the mixer live (revision 1, `unsaved`, the pill "Not saved"), Save (generation
  one more, "Saved"), another edit, "Go back to the last save…" (the mixer plays the saved one,
  the editor shows it), "The mixer saves: By itself" (an edit of `global.save`: the mixer
  `waiting`, then saved by itself after 3 s). Found and fixed: the mixer sheet showed protocol 1
  (now CFG_INFO's), and after Save the open sheet said "Not yet" (it now follows the storage state; checked with the mock).
- The mixer was left with its factory configuration, saved (as before the run, byte for byte).

## Where it starts

Historical starting point, 2026-11-13, before I-1..I-7. The current implementation and fresh
verification boundary are above; the mock is no longer the only page backend.

- The firmware side of the contract runs on the mixer: the schema and the binary form, the
  configuration protocol over USB CDC (`smcconf info`, `pull`, `push`), the storage, what each
  control sends (the scenario corpus, by hand with synths), USB and BLE MIDI.
- The editor was then a browser prototype (now `app/editor/`) on a mocked mixer (`js/mock.js`). Its
  modules other than `app.js` and `icons.js` use no DOM: `schema.js`, `config.js`, `midi.js`,
  `labels.js`, `mock.js`, `templates.js`.
- The owner's order (SPEC Q-27): Bluetooth first (done in the main), then P-4 (the protocol over
  SysEx), P-1 (SET without a restart), P-2 and P-3 (events, SELECT), P-5 and P-6; the saving model
  of Q-26 (SAVE, REVERT, the storage mode) after P-1.
- Divergences the first contract test settled (2026-11-13, the editor changed, the firmware's
  behaviour kept): `midi.js` wrote the NRPN/RPN parameter number before every value, the firmware
  only at a move's or press's start and when the channel's selection is another parameter (Q-21
  b-1), with the null parameter when the preset asks for it; the editor said a momentary Program
  Change button sends a program on release (the core sends nothing); a note button released to a
  `lo` above 0 sends a Note On with that velocity, not a Note Off; a relative encoder in `div` or
  `step` sends one step per N clicks or N per click, not one per click; a fader's value is rounded
  as the core rounds it (`map_pos`).

## Test layers

Fast first: most of the contract is checked on the host in `make test`, the mixer only for what
needs it.

| Layer | What it checks | Runs where | When |
|---|---|---|---|
| T-1 Contract, host | the editor's modules against the firmware's facts: files, checks, the bytes each control sends, the link's frames | `make quick` / `make test`, the editor's JS in Node in the host test image | every change |
| T-2 Link, host | the editor's link client against the device's link code (`devhost`), byte for byte | `make test` | every change to either side's link |
| T-2L Live, host | the editor's `MixerDevice` (the page's device code) live against `devhost`: connect, edit, save, go back, select, touches, what was sent, a whole file, saving by itself, STOCK check and back (devhost `--boot`) | `make test` (Docker) | every change to either side's link or configuration |
| T-3 Product, device | the editor's predictions against the mixer: push, inputs injected, MIDI received, pull, saving | `make e2e`, the owner's go-ahead | every firmware or contract change |
| T-4 Browser, device | the real page in a browser with Web MIDI on the mixer: connect, edit, try, save | a checklist, driven by the browser tools where they reach | before a release, after UI work |

### T-1: the editor's modules on the host

**Built (I-1, 2026-11-13):** the firmware's side writes shared vectors (A-20 (b)), the
editor's side reads them in Node (A-20 (a), the owner, 2026-11-13: Node.js from Debian's pinned
package, now in `docker/Dockerfile.host`, no npm). Neither side needs the other's runtime:

| Part | Runs where | What it does |
|---|---|---|
| `tools/editortest/vectors.py` | host Python, needs `build/host/smcsim` | writes and checks `tools/editortest/vectors/`: `sends-NAME.json` for the test, factory and generated configurations (every control of every page and layer touched in the simulator, `smcsim --config FILE`: a fader swept up and down in one move, an encoder one event each way, a button pressed and released, Shift held or latched as the preset says; the core's messages and its preset, page and Shift, phase by phase; 1592 probes and about 11 000 messages a configuration, one probe a line); `faults.json` (51 broken files, the cases of `tools/smcconf`'s tests and a few more, each with the field the codec names) |
| `tools/editortest/test_vectors.py` | `make quick` (firmware/Makefile), host Python | the vectors are what the firmware gives now (a stale one fails: one side changed); `gen-N.json` valid and canonical for smcconf, the binary form round trips |
| `tools/editortest/contract.mjs`, `run.mjs` | `make quick` through `tools/env/test_editor.sh`: Node in the host test image, Docker; Windows launchers need a POSIX shell | the editor's own modules: `config/*.json` and `gen-N.json` byte for byte through the reader and writer; the editor still writes `gen-N.json` (its edit code, `Config.setField`, `setShiftState`: every type of every kind in every mode, the preset's and a fixed channel, whole, inverted and partial ranges, `start`, `noRelease`, LED modes, the four Shift forms, presets with another channel, Note Off, `paramNull`, latch and hybrid, global `jump`, `immediate`, `fine14 lsb`); the editor names the field smcconf names in every broken file, and none in the valid ones; every sends vector against `midi.js` (`messages` with the channels' NRPN selection, `buttonValue`, `faderValue` for `jump` faders, `encoderDelta`, `encoderValue`, the null parameter), message for message, and a fader's sweep ending at `hi` and `lo`; every module parses (VM module syntax check) |

The values of a fader in `pickup` or `scale` and of a turned absolute encoder after its first event
are the core's (acceleration, pickup); the test checks how each is sent. Not built yet: test 4 (the
words). A change on either side: `python3 -I tools/editortest/vectors.py --write` (the core, the
codec or the probes changed) or `node tools/editortest/run.mjs --write-generated` (the editor's
generator changed), then both sides' tests. Without Docker, or while the image has no Node,
`test_editor.sh` prints `SKIPPED` and the editor's side has not run (`SMC_EDITOR_TESTS=required`
makes it an error). With Docker available its `host_image.sh` prerequisite can automatically
build a missing image. In a no-download session inspect the required cached fingerprint first;
do not invoke a missing-image build or retag an older image as current.

**State:** both sides run in `make quick` and `make test`: `test_vectors.py` on the host,
`run.mjs` in the host test image's pinned Node ([build.md](build.md)). Initial macOS evidence
dates to 2026-11-13; Linux host-image runs followed on 2026-11-14 and fresh macOS/Linux checks
passed on 2026-11-15. Native Windows has not been certified.

The checks in `tools/editortest/` (run by `make test`; item 4 remains a proposal):

1. **Files**: every file in `config/` and the scenario corpus's configurations come back byte for
   byte; edits made through `Config` produce files that `smcconf validate` and `smcconf format
   --check` accept.
2. **Checks**: a corpus of broken files (`tools/smcconf`'s test cases): the editor names the same
   field and fault as `smcconf validate`.
3. **Sends**: for every type, mode and channel form in the test configuration, and for a set of
   generated configurations, the editor's `messages()` against the core's `out` records for the same
   input in the simulator (`build/host/smcsim`), message for message: gestures, the NRPN selection
   and null, 14-bit pairs, note release forms, relative encoders.
4. **Words**: the help texts and labels (`labels.js`) that state a behaviour carry an ID of the
   core's behaviour they describe (pickup, acceleration, Shift kinds); a test lists the IDs both
   sides know, so a behaviour renamed or removed on one side fails.

Shared test vectors (A-20 option b) are the files these tests read and write; the C and Python
tests read the same ones.

**Seeded differential corpus** (2026-11-15): `tools/editortest/differential.py` and
`differential.mjs` extend the fixed vectors to randomly generated valid files, malformed binary
forms and live edits. Reuse `tools/smcconf/test_smcconf.py`'s generators, compare C/Python/editor
acceptance and canonical bytes, compare the editor's file encoding and edit records to Python,
and apply the edits with C. Four models, 64 mutations and two edit pairs run in the editor's
check wrapper (`make quick`/`make test`); the same smoke run and the harness tests run in
Linux with `SMC_NODE=node`. The host-side launcher computes the cached image fingerprint in
Python rather than invoking a POSIX shell from native Windows Python, and resolves the C tool's
native executable name (including `.exe`). Standalone runs use Node in the existing host image, never install
anything, and refuse to overwrite a diagnostic directory. Harness tests plant a C mismatch,
an editor mismatch, a worker failure and incorrect expected edit/encoding bytes, so agreement
cannot pass merely because a comparison is absent. A failing seeded run retains its corpus and
first discrepancies under `build/differential/` (or `--out`).

**Connection failure tests** (2026-11-15): `tools/editortest/connection.mjs` in `run.mjs` uses
permission stubs and deferred fake clients, with no device APIs. It checks denied permissions,
missing ports, cancelled MIDI/Bluetooth choices, and old replies/errors/queued operations
crossing a reconnection. Poll, edit, Save, Revert, Load, selection and STOCK results belong to
the original client/connection epoch. `link.mjs` also checks local link shutdown: no retry,
queued Save or remaining configuration-upload chunk/commit after close, while an acknowledgement
accepted before close is kept. Send exceptions clear their timer/waiter, and an old transport
cannot detach a replacement listener. The epoch is editor-local; it neither changes the wire
protocol nor resolves P-6's configuration revision decision. These races were reproduced before
the fixes, then checked in Node and a browser; physical fault/race reconnection remains
to test. Normal reconnection in a completed USB wizard is not that cancellation-race test.

### T-2: the link client on the host

**Built (I-2, 2026-11-13): the device's side.** `tools/editortest/transcripts.py` runs a scripted
session over SysEx against `devhost --sysex-fd` (the device's `link.c` and `cfglink.c`): CFG_INFO,
a pull of the factory configuration in 128-byte chunks, ABORT, a push of the test configuration
(ID and revision set), COMMIT, CFG_INFO, then three refusals (a chunk with no transfer, an unknown
command, LOG_SUBSCRIBE); the seq starts at 0xF0 and wraps. It writes
`tools/editortest/vectors/link-v1.json` (one exchange a line: seq, cmd, payload, the request and
reply SysEx in hex, the reply's seq, cmd, status and payload) and checks it in `make quick` (a
change of the device's answers fails until it is written again).

**Built (I-3, 2026-11-13): the editor's side.** `app/editor/js/link.js`: the SysEx frame
(pack7, CRC-16, CRC-32), the replies (CFG_INFO, cfgbin's error), and `LinkClient` over a transport
`{ send(bytes), listen(fn) }` (`webMidiTransport(input, output)` for Web MIDI): one call at a
time, the same frame again after a timeout, a busy device asked again for 2 s, pull and push as
`devlink.py` does them. `tools/editortest/link.mjs` (in `run.mjs`, so in `make quick`): every
request of `link-v1.json` byte for byte and every reply read to the same fields; the whole session
through `LinkClient` against a replay of the transcript (pull, abort, push with its first reply
lost, the three refusals), with a foreign SysEx before each reply; a call with no reply sent
three times with the same seq. At I-3 the page still used only the mock and had no binary codec;
I-5 subsequently added real `MixerDevice` and the owner's chosen JavaScript codec
`js/cfgbin.js`. The mock remains a separate simulation backend.

The editor's link client (`app/editor/js/link.js`) is a pure frame encoder, decoder and state
machine over a transport it is given. The page implements Web MIDI SysEx, not a Web Serial/CDC
fallback. It is tested by transcripts: `devhost` (the device's link and cfglink code
built for the host) answers a script of requests from the Python client (`smcconf`), the
exchange is recorded, and in Node (the build image) the editor's client must produce the same request bytes and
decode the replies to the same results. The fixture is still named `link-v1.json` although later
protocol commands were added; the filename is not a claim that the current configuration
protocol is 1. On 2026-11-15 the finite devhost lifetime was raised to 120 s after the
unchanged 158-exchange reference took about 40 s; no fixture or protocol change was needed.

### T-2L: the editor's device code live against the firmware's on the host

**Built (2026-11-14).** T-2 holds each side to a fixed transcript; T-2L runs the two
together, as the page and the mixer run. `tools/editortest/live.py` starts `devhost --sysex-fd
--flash` (a fresh flash file: the link, `cfglink`, the control path, the core and the stores, built
for the host) and `node tools/editortest/live.mjs` in the host test image, and joins devhost's config
port to the container's stdin and stdout. `live.mjs` stands in for Web MIDI only
(`navigator.requestMIDIAccess` with one port pair named "SMC-Mixer Config"); everything else is the
page's own `MixerDevice` (`js/mixer.js`, `link.js`, `cfgbin.js`). The session includes:

- connect: the port found by asking, the versions agree, the factory configuration read and seen
  as the editor's, storage clean, EVENTS and SENT on;
- a live edit (one SET, the mixer keeps running) read back by a pull; storage unsaved; Save, clean;
- a second edit, then Go back: the saved configuration again, read back, clean;
- an outside upload with the running ID and revision: the editor catches the new revision;
  Go back clears divergence and adopts the reverted configuration's current identity;
- the tabs select preset 3 page 4: the mixer goes there and its EVENT brings the editor's state;
- injection: an encoder turn selects `s0.encoder` in the editor (P-2); the row's Rew pressed and
  released: SENT names `row.rew` (P-5);
- a whole file (`config/test.json`): sent as PUT (the core restarts), read back the same;
- `global.save = "auto"`: the mixer saves by itself after its quiet pause;
- no error reported by the editor.

A configuration read back is compared with the file through `toJson(fromJson(file))`, not through
the editor's encoder, so a fault in the editor's encoding does not cancel out. Checked by planting
a fault on each side (one field's byte flipped in `cfgbin.c`'s decoder, then in `cfgbin.js`'s
encoder): each fails "open a file: the mixer plays it". `SMC_DEVHOST=path` runs it against another
devhost build. In `make test` after the editor's contract tests; SKIPPED without Docker or Node in
the image (`SMC_EDITOR_TESTS=required` makes that an error). What it does not cover: the
performance port (devhost writes it to `--midi`, not to this pipe, so the monitor's joining of
messages to SENT entries is not run), Bluetooth, the page's DOM (T-4).

### T-3: the product on the mixer

**Built (I-4, 2026-11-13): `make e2e PORT=/dev/cu.usbmodemNN`** (`tools/e2e/e2e.py`; talks to the
mixer, so only with the owner's go-ahead in the session). For each of `config/factory.json`,
`config/test.json`, `vectors/gen-1.json`, `vectors/gen-2.json`:

1. push over SysEx on "SMC-Mixer Config" (`--transport cdc` for the CDC); CFG_INFO must show the
   supplied ID and the device-assigned revision (its previous revision plus one, or the supplied
   legacy value on older firmware); pull must match those metadata and the sent content;
2. the probe plan of T-1 (`vectors.plan()`) for four (preset, page, Shift) blocks (`--blocks`,
   default `0:0,0:0s,1:2,3:3s`: preset 1 page 1 with and without Shift, preset 2 page 3, preset 4
   page 4 with Shift; 206 probes), presets and pages selected with the panel's own buttons
   (Shift + a double arrow, Shift + an arrow; the Shift taps a latching preset needs found by
   running the plan in the simulator first), injected in real time over the CDC link;
3. checks: the MIDI the host receives on "SMC-Mixer" equals the simulator's on the same input
   (and so, by T-1, the editor's prediction), message for message; it equals what the mixer logged
   it sent (the USB MIDI path); no real input during the run (a jittering fader changes the core's
   state, so a run with one does not count);
4. before the test, keep both the running and stored configurations (`before.bin`,
   `stored-before.bin`). An existing saved baseline and stable storage (`clean` or `unsaved`)
   are required: the protocol cannot remove a first saved slot to undo a test's initial Save.
   When the working copy is unsaved, temporarily REVERT to read the stored copy, then restore the
   working copy without Save. At the end, compare the actual storage generation, not a phase's
   return flag: even a phase that saved and then failed must restore the original saved content.
   REVERT and read it back, then put the original running copy back; original unsaved edits stay
   unsaved. A restoration can advance the generation, but must not change either copy's content.
   Host tests cover both copies, failures and a run against `devhost --flash` (2026-11-15).
   ✅ On the mixer over USB and BLE MIDI (37 checks each) and in the CDC smoke (14) on
   2026-11-15, and over USB on 2026-11-23, restoring the original running and stored content.

About 75 s a configuration. The report, the device's log and the host's MIDI go to
`logs/e2e/DATE-TIME/` (local). Park the faders at an end first. The MIDI ports are
`tools/hostmidi`'s (`capture()` for channel messages with host times). CoreMIDI is physically
verified; ALSA and WinMM have host tests with API fakes, no native device runs ("Any OS").
`--ble PORT` runs the configuration and the MIDI in over the mixer's Bluetooth MIDI port
(connected in the OS first): there the mixer's logged MIDI must equal the simulator's, and the
host's must be the logged messages with only older values of continuous controls left out (BLE's
last value wins, FR-IO-08, [ble.md](ble.md); `merged_away()`, `tools/e2e/test_e2e.py`).

**Runs on the mixer (controller 31, 2026-11-13; `logs/e2e/2026-11-13-*`, local):**

| Transport | Configurations | Result |
|---|---|---|
| SysEx on USB MIDI ("SMC-Mixer Config"), MIDI in on "SMC-Mixer" | factory, test, gen-1, gen-2: 206 probes each, 1 396–1 795 messages | ✅ every check (gen-1 after fader 6 was parked: a first run caught it at 37 % of its travel, the real-input check named it) |
| The CDC, MIDI in on "SMC-Mixer" | the same | ✅ 22 checks |
| SysEx on BLE MIDI, MIDI in on BLE | the same | ✅ 23 checks; 0–1 older fader values merged away by BLE's queue per configuration (a first run without that rule counted 2–12 such drops as failures) |

The injections came at most 39 ms after their plan time (the comparison is of order, not time).
The first runs were spoiled by a fader that jittered at the top of its travel (fader 3,
16 150–16 383): the check for real input caught it each time.

`make e2e` (`tools/e2e/`): one command, a few minutes, no hands, with the owner's go-ahead in
the session (AGENTS.md). For each configuration (the factory one, the test one, a set generated by
the editor's own code to cover every type and mode):

1. the editor's code (in Node) writes the file and predicts what each control sends;
2. the configuration goes to the mixer over SysEx on USB/BLE MIDI, or the CDC; `pull` gives
   it back, checking content and the running revision reported by CFG_INFO;
3. inputs are injected (the scenario runner's injection) and the MIDI the mixer sends is received
   on the host (the host MIDI backends below, USB; BLE where the run asks for it);
4. what was received is compared with the editor's prediction, message for message;
5. live edits and saving: SET, SAVE, another edit and REVERT, with state and read-back checks;
6. at the end restore both saved and running content as described above, preserving whether the
   running copy was unsaved. The device assigns new revisions to the restoration writes; the
   saved generation may also advance. Restored content and ID, not the old revision bytes, match.

A report like `firstrun.py`'s: pass/fail per check, the session's log and MIDI kept.

### T-4: the real page

The page in Chrome or Edge (Web MIDI with SysEx; the permission prompt is the user's), served by
`app/editor/serve.py`, on the mixer over USB: connect, read, edit a control and hear it,
select by touching (P-2), save, reconnect. A checklist in [app.md](app.md), "The editor's code", run
by hand or with the browser tools; the iPad later (A-13).

**Built (I-5, 2026-11-13).** The page talks to the mixer itself: `app/editor/js/mixer.js` gives
the page the mock's fields and events (`js/mock.js`, kept: `?mock` in the address, and where the
browser has no Web MIDI) from the real link (`js/link.js` over Web MIDI SysEx) and the editor's
own binary form (`js/cfgbin.js`: the owner's choice (a) of 2026-11-13, `codec.py`'s rules in
JavaScript, held to it byte for byte by `vectors/binary.json` in `run.mjs`: the four
configurations with and without an ID and a revision, seven broken binaries, every file of
`faults.json`). Connect asks every MIDI port pair, the configuration port first, with GET_INFO;
checks the capability and the versions (FR-CFG-07, refused with which side to update; `link.mjs`
tests it); reads the configuration; when it is not the editor's (names, ID and revision aside),
the page's "The editor and the mixer differ" asks which wins. In that initial I-5 implementation,
Linked, every edit was pushed whole
(the newest wins while one is under way); CFG_INFO every 1.5 s gives the storage state; the
mixer's MIDI shows in the monitor; "Load from the mixer" reads it again (without names: the binary
form has none). Save, Go back and the saving setting (Q-26) were not present in controller 31;
I-6 added them, I-7 added panel/touch/selection/SENT events. Today small changes use up to eight
SETs, larger or unrepresentable changes use PUT, and EVENTs supplement the 1.5 s INFO poll.
Without Web MIDI the page is an offline file editor; the mock needs an explicit `?mock` (R11).

**Run on the mixer (controller 31, 2026-11-13, Chrome on macOS over USB, the owner allowing MIDI
for localhost):** ✅ connect (the port is "SMC-Mixer hardware test SMC-Mixer Config": macOS keeps
the name it saw first, [usb.md](usb.md)), versions, the factory configuration read and taken as
the editor's; fader 1's CC changed in the inspector reached the mixer (a pull over the CDC) and
was saved by it; fader 1 injected over the CDC showed in the monitor as the mixer's CC 33; the
mixer given `test.json` behind the page's back: on connect the page asked, and "Take the mixer's"
showed it; undo twice sent the editor's copy back, and the mixer's configuration was again the one
it had, byte for byte. A browser without Web MIDI is told so by the page.
At that date the page over Bluetooth had not run (the USB port is asked first). The real page
over BLE passed targeted client checks on 2026-11-15 and the Bluetooth firmware cycle on
2026-11-23. Edge and native mobile routes remain unverified and unbuilt respectively.

**The page against devhost (2026-11-14).** T-4 without the mixer and without Web
MIDI: `serve.py` takes a WebSocket on `/devhost` and starts `build/host/devhost` for it with a fresh
flash file, its config port on a socket pair (`--sysex-fd`) and its performance port on a FIFO
(`--midi`), each WebSocket message the port's number and MIDI bytes. With `?devhost` the page's
`js/devhost.js` defines `navigator.requestMIDIAccess` as two port pairs named as the mixer's,
"SMC-Mixer Config" and "SMC-Mixer"; everything else is the page as it is on the mixer. `&selftest`
(`js/selftest.js`) runs the checklist on the page's own functions, the ones its buttons call, and
its dialogs: 28 checks, about 20 s, **all passed in a browser without Web MIDI on
2026-11-14**, no console error:

- connect (the configuration port, the editor's configuration seen as the mixer's, no question
  asked, storage clean, SENT on);
- an edit made in the editor goes as one live edit; unsaved, the pill says so; Save, clean;
- a second edit, then Go back through its dialog: the editor shows the saved configuration, clean;
- the tabs take the mixer to preset 3 page 4 and the editor stays there;
- an encoder turn injected on the mixer selects `s0.encoder` and marks it on the panel; the
  monitor names the encoder's message and the row's Rew press and release, from the performance
  port joined to SENT (what T-2L does not cover); no message left unnamed;
- a file opened goes whole (the core restarts) and the mixer plays it; the saving setting "by
  itself" saves after the pause; no error.

The self-test keeps the user's working copy and history in the browser's storage as they were.
On a real mixer (without `?devhost`) it asks first, since it changes and saves the mixer's
configuration. Not covered: Web MIDI's own port discovery and permission, Bluetooth, timing on
the mixer, the look of the page. Not in `make test`: it needs a browser, and none is in the
build image; any browser runs it (`python3 -I app/editor/serve.py`, then
`http://localhost:8765/app/editor/?devhost&selftest`).

**Transfer-state regression** (2026-11-15): the current self-test also checks that an edit shows
Sending before it is acknowledged, not Saved; all 29 checks pass against `devhost`. The deferred
client tests distinguish wire transfers from local names-only changes, preserve confirmed
configuration identity, and close an unconfirmed failed transfer rather than compute later
edits against a potentially partial device state. Local validation failures keep the known
connection. No physical browser-to-mixer reconnection is claimed by these host checks.

**Physical page and client checks (2026-11-15):** the full physical self-test gave 26/29: initial
equality/no-divergence and live-edit selection failed, with a whole upload occurring. The
29/29 run of 2026-11-23 started from the factory baseline, which the self-test assumes ("Hardware
results, 2026-11-23"). Targeted native-Web-MIDI client probes passed 10 checks each over USB and
BLE. Browser session lifetime and native permission/confirm handling are infrastructure
boundaries, not automatic evidence of a configurator or mixer crash. Do not reload, replace or
start a competing connection merely to inspect an active device page.

## Any OS

The owner's requirement, 2026-11-13: everything must build and every host check must run on
macOS, Linux and Windows; nothing macOS-only may be required. This is not yet fulfilled.
Where it stands, and the plan:

| Part | Today | Plan |
|---|---|---|
| Device builds (`build_app.sh`, `build_boot.sh`, `check_device.sh`, `build_usbkey.sh`) | Docker, linux/amd64 image; bash launchers on macOS/POSIX, Windows needs WSL/Git Bash. macOS-launched builds and twice-built 4/4 identical images passed (2026-11-15); cached BuildKit preparation was not cold offline provisioning | `run.sh` uses `sha256sum` or `shasum` (R12, fixed). Native Linux/Windows launcher success is not established |
| Host C tests, scenarios, Python tools' tests (`make -C firmware quick/test`) | `cc` and Python 3, standard library | ✅ (2026-11-14) **the plain Python suites on Linux**: `make linux-py` (`tools/env/test_python_linux.sh`) runs the 14 suites that need no program from `build/host` in the host test image's Python 3.11 (Debian; `docker/Dockerfile.host`): all pass, the serial module on a real Linux pseudo-terminal among them. **Everything on Linux** (2026-11-14, the owner put gcc in the images): `make linux` (`tools/env/test_linux.sh`) builds the host programs with gcc 12 in the host test image (the machine's own platform: arm64 on the Mac, no emulation) and runs `make -C firmware test` (the C tests, the scenarios, the fuzzer's short run, every Python suite, the 7 that drive `devhost`, `smcsim` and `cfgtool` among them), the editor's contract tests and T-2L in the image's Node: ✅ all pass (2026-11-14: on arm64 Linux in 2 min 8 s; on amd64 Linux, emulated, in 9 min 39 s). What Linux needed: the host log port reads the GNU build ID (20 bytes; LC_UUID on macOS is 16; the tests take either), `-D_GNU_SOURCE` for glibc's `memmem` and pseudo-terminal calls, and `firstrun.py` skips the frame check for `devhost`'s stand-in crash frame (on macOS its Mach-O gave no functions, so the check was skipped already). **Windows:** `devhost` and the C tests use POSIX (`poll`, pseudo-terminals, `mmap`, `execv`), so a socket pair in the Python suites alone would not bring them to Windows; the plan proposed is WSL for the host tests there (as the device builds' bash scripts already need), the owner's to decide |
| The editor's contract tests (T-1) | vectors in host Python; editor modules in the host test image's Node (`tools/env/test_editor.sh`); macOS and Linux runs pass | native Windows entry paths are not certified; POSIX test/launcher requirements still apply |
| `jlrom` (ROM download mode, [recovery.md](recovery.md)) | Swift on IOUSBHost: macOS only | since 2026-11-13 `make quick`/`test` build it on macOS only and say SKIPPED elsewhere. Later: the ROM protocol in Python with per-OS USB through `ctypes` (Linux usbfs ioctls; Windows WinUSB, which needs the driver bound) |
| `smcmidi` (MIDI ports: the stock firmware's service SysEx, `smclog capture --midi`, `smcsim device`) | Swift on CoreMIDI: macOS only; `tools/smcsim/midiin.py` has its own `ctypes` CoreMIDI input | since I-2 (2026-11-13) `tools/hostmidi/hostmidi.py` (standard library, `ctypes`): one interface (list ports, open by name, send SysEx, receive SysEx/channel messages) over CoreMIDI, ALSA rawmidi through `libasound` and WinMM, plus `fd:N` and `raw:PATH` byte streams; users `smcconf --midi` and `make e2e` (`capture()`, `mark()`, `take()`). ✅ CoreMIDI both ways on physical USB/BLE MIDI. ALSA/WinMM have API-fake tests, no native device runs; ALSA's permanent send errors and WinMM's short input are fixed (R04, R05). Other consumers still use the macOS-specific paths; porting them remains work |
| Serial (the CDC link: `smclog.py`, `firstrun.py`, `smcconf`'s `devlink.py`, `usbkey/console.py`) | `termios`/`tty`: macOS and Linux | since 2026-11-14 `tools/hostserial/hostserial.py` (standard library): finding the port by its USB IDs (`1209:0001`) on Linux (sysfs), macOS (`ioreg`) and Windows (the registry), and a port object over `termios` (POSIX) or the Win32 comm API by `ctypes`. Every tool's `--port auto` uses the finding (through `smclog.open_serial`); `hostserial.py list` prints the ports. ✅ POSIX opening on pseudo-terminals on macOS and Linux (tests); ✅ the macOS finding listed the mixer's CDC port as `1209:0001` from the OS's USB registry (no port opened); ⚠️ the Linux and Windows finding tested on canned sysfs trees and registry contents; ❓ the Win32 port has not run (no Windows host). Next: the tools reading and writing through the port object instead of a file descriptor, so they run on Windows |
| The editor's page (T-4) | a browser granting Web MIDI/SysEx; stock BLE also needs Web Bluetooth. `serve.py` static serving is plain Python; its devhost bridge is POSIX-only | real macOS Chrome routes verified within the limits above; no full cross-OS sign-off, no real device route in iPad Safari |

**Native-platform gaps:** the table describes intended backend coverage, not working Windows
certification. Three host-platform defects found on 2026-11-15 are fixed with host tests: POSIX
`termios`/`tty` imports blocking even offline `smcconf` commands on Windows (R03), permanent ALSA
send errors retried forever (R04) and WinMM dropping ordinary short MIDI input (R05). Existing API fakes and Linux host-container tests are not native ALSA/WinMM device runs. The
browser's Web MIDI is a different path from these Python host backends. Do not add dependencies
or quietly change platform/protocol policy to make the table look complete.

## Steps

Each step ends with `make test` (and `make check` for firmware) and the layer it adds running.

| Step | What | Sides | Needs |
|---|---|---|---|
| I-1 ✅ 2026-11-13 | T-1: `tools/editortest/` in `make test`: files, checks, sends; the NRPN divergence settled; on any OS (vectors in Python, the editor in Node in the build image) | editor | A-20 decided |
| I-2 ✅ host 2026-11-13 | P-4: configuration protocol over SysEx (USB MIDI and BLE MIDI), `smcconf --midi`, transcripts ([link.md](link.md), "Over SysEx"); physical evidence followed in I-4 | firmware, tools | Q-28 decided |
| I-3 ✅ host 2026-11-13 | `js/link.js` and T-2 (`link.mjs` in `run.mjs`); the mock and the client behind one interface moved to I-5 | editor | I-2 |
| I-4 ✅ 2026-11-13 | `make e2e` (T-3) on the mixer: SysEx on USB MIDI, the CDC, BLE MIDI (controller 31) | tools | I-2, the owner's go-ahead per session |
| I-5 ✅ 2026-11-13 | Page-on-mixer checklist: connect, open, push, pull, version checks; editor codec (`js/cfgbin.js`). The full page self-test 29/29 on the mixer on 2026-11-23 | editor | I-3 |
| I-6 ✅ 2026-11-13 | P-1 SET without a restart, then Q-26's SAVE, REVERT and the storage mode, each added to T-1..T-3: below | both | I-4 |
| I-7 2026-11-13 | P-2 events and P-3 SELECT, then P-5 and P-6: P-2, P-3 and P-6's firmware part built on both sides (above); ✅ on the mixer 2026-11-14 (controller 34: `make e2e` over USB MIDI and BLE MIDI, `smcsim device`, the page in Chrome; controller 33's BLE jam fixed); an outside edit keeping the revision not noticed (open, P-6); P-5 built on both sides 2026-11-14 (protocol 4), ✅ on the mixer (controller 35: `make e2e` over USB MIDI and BLE MIDI, the monitor in Chrome over USB) | both | I-6 |
