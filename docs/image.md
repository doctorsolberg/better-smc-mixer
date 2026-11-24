# Flash image format

How the SMC-Mixer's flash is organised as the boot ROM, the factory uboot and the stock firmware
see it: sector 0 and its boot pointer, the application area's JLFS directory, the SFC cipher, and
the vendor's update containers. It is what our installer must produce (SPEC stage 2, §3.4
approach). The flash map itself is in [hardware.md](hardware.md).

Sources, all on the mixer's own files (local, never published, SPEC C-7):

- the V012 dump (`materials/dumps/smc-mixer_012_dump1.bin`), read by the stock firmware (SINCO
  `0x23`), i.e. **as the CPU sees flash**;
- the V013 vendor update (`materials/SMC-Mixer_013.fwsc`), whose `flash.bin` turns out to be **the
  physical bytes** of flash `0x0..0x24000`.

open-smc-pad's notes on the SMC-PAD were the hints; everything marked ✅ below was checked on the
mixer's files with [`tools/image/flashimg.py`](../tools/image/flashimg.py). ✅ Two raw ROM reads
on 2026-11-10 confirmed the physical model (`tools/jlrom/compare.py`, [recovery.md](recovery.md),
"Rehearsal 1"). Later image starts, configuration writes and STOCK/browser installations have
their own dated evidence below, in [bringup.md](bringup.md) and in [app.md](app.md), "Firmware".
On 2026-11-23 browser install/update/Go back cycles over USB and Bluetooth completed with
physical off/on and configuration read-back ([integration.md](integration.md), "Hardware
results, 2026-11-23").

## The check

```sh
python3 -I tools/image/flashimg.py check materials/dumps/smc-mixer_012_dump1.bin materials/SMC-Mixer_013.fwsc
```

It parses both stock application areas into their parts (directory entries, files, resource tail)
and rebuilds them with the same code that builds ours ✅:

- **V013**: flash `0x1000..0x239FC` rebuilt byte for byte; SFC-encrypted with chip key `0x980F` and
  erased up to the VM start it equals `flash.bin` from `0x1000` to `0x24000`, byte for byte;
- **V012**: flash `0x1000..0x2340C` rebuilt byte for byte; from there to `0x24000` the dump holds
  exactly what erased flash reads as through the SFC, so the whole dump below the VM is explained;
- sector 0 of the V012 dump, put through the 256-byte keystream (below), equals sector 0 of V013's
  `flash.bin` byte for byte: the factory sector did not change between versions.

`test_flashimg.py` runs this check when the vendor files are present (`make -C firmware test`), and
synthetic tests of every part everywhere.

## Ciphers

**ENC** is JieLi's byte stream cipher: a 16-bit LFSR (polynomial `0x1021`, shifted left once per
byte) started from a key; each data byte is XORed with the LFSR's low byte. The same operation
encrypts and decrypts.

**SFC** is ENC applied per 32-byte block of flash with a key that depends on the address. ✅ The
mixer's key for the block at flash address `a` of the application area is

    0x980F ^ ((cpu(a) >> 2) & 0xFFFF),   cpu(a) = 0x1E00000 + a − 0x1000

i.e. the chip key `0x980F` XOR bits 2..17 of the CPU address (`0x1E00000 >> 2` has no low 16 bits,
so this is also `0x980F ^ ((a − 0x1000) >> 2)`). Verified: with it, `app.bin`'s data CRC matches
over the whole V013 area (0x223BC bytes), and the rebuild above is exact.

The mixer's chip key is therefore the **same as the SMC-PAD's**. Our earlier notes said `0x9C0F`
with the key taken from the flash offset (`0x9C0F ^ (a >> 2)`): the two formulas agree exactly on
the odd 4 KB sectors (`0x1000..0x1FFF`, `0x3000..0x3FFF`, ...), where subtracting `0x400` from
`a >> 2` only clears bit 10, and disagree on the even ones. The directory, `cfg_tool.bin` and the
zero blocks used to find the key all happen to lie in odd sectors; the CRC of `app.bin` over the
whole area tells them apart.

Where SFC applies:

- from flash `0x1000` up to the VM start (the directory's `VM` entry; stock `0x24000`). Above it
  flash is stored as is (✅ hardware.md);
- only the image's own bytes are encrypted: V013's encrypted data ends at `0x239FC`, in the middle
  of a 32-byte block, and the rest is erased (`0xFF`) ✅. Erased flash below the VM reads through
  the SFC as the keystream itself, which is what the V012 dump shows after its area ✅;
- because the key uses only address bits 2..17, an area placed `0x40000` further up
  (`0x41000`) encrypts identically, whichever address the hardware keys on (test
  `test_sfc_slots_on_a_256k_grid_share_the_cipher`).

## Sector 0

Physical layout ✅ (V013 `flash.bin`): 32-byte entries from offset 0, **each ENC-encrypted with
key `0xFFFF` on its own**:

| Offset | Entry | Offset field | Size | Notes |
|---|---|---|---|---|
| `0x00` | flash header | — | — | Shares the entry CRC (bytes 2..31); its other fields are not decoded |
| `0x20` | `uboot.boot` | `0xA0` | `0xE4C` | The factory second-stage loader, inside sector 0 |
| `0x40` | `isd_config.ini` | `0xEEC` | `0x63` | Data CRC is over the stored bytes, which are not plain text |
| `0x60` | `app_dir_head` | **`0x1000`** | `0xFFFFFFFF` | **The boot pointer**: where the uboot looks for the application directory |
| `0x80` | `key_mac` | `0x7F000` | `0x1000` | Last entry (index ≠ 0) |

**Reading it through the stock firmware** (SINCO `0x23`) returns sector 0 XORed with the ENC
keystream of `0xFFFF` **restarting every 256 bytes** ✅ (all 4096 bytes of the V012 dump agree with
V013's physical sector this way). That is why the dump shows the flash header in clear (its entry
starts at a restart) and the file list scrambled.

Changing the boot pointer means rewriting the `app_dir_head` entry: new offset, new header CRC,
ENC `0xFFFF` again; the rest of the sector stays byte for byte (`flashimg.set_boot_pointer`, tested
on a synthetic sector). This is the one write the §3.4 install needs outside free flash. The stock
firmware's write command (`0x22`) stores sector 0 as given, with no keystream (⚠️ static analysis:
[hardware.md](hardware.md), "Stock service protocol"): it takes the stored bytes, as `jlrom` does.
The keystream is on the read side only, applied per read call. ✅ 2026-11-10: sector 0 rewritten
this way with its own contents, read back equal, and the chip starts from it ([bringup.md](bringup.md),
"Step 2b").

## JLFS entries

32 bytes, little-endian ✅:

| Bytes | Field |
|---|---|
| 0–1 | header CRC: CRC-16/XMODEM (poly `0x1021`, init 0, not reflected) over bytes 2–31 |
| 2–3 | data CRC (same CRC) over the entry's data, or `0xFFFF` |
| 4–7 | offset |
| 8–11 | size |
| 12 | attr |
| 13 | res |
| 14–15 | index: 0, except on the last entry of a list |
| 16–31 | name: the name, a NUL, `0xFF` padding; some entries carry a 4-byte tag in the last bytes |

## Application area (flash `0x1000`, CPU `0x1E00000`)

The directory has 9 slots (`0x120` bytes); the stock one uses 8 and leaves the ninth erased. The
first file starts right after it, at `0x120`: that is the stock entry point `0x1E00120`.

| Entry | Offset | Size | attr | res | Tag | Data CRC |
|---|---|---|---|---|---|---|
| `app_area_head` | **the entry point** (`0x1E00120`), not an offset | end of the last file, relative to the directory | `0x83` | `0xFF` | | over the area from `0x20` (after this entry) to its size |
| `app.bin` | `0x120` | V012 `0x21DCC`, V013 `0x223BC` | `0x82` | `0xFF` | | over the plaintext |
| `cfg_tool.bin` | right after `app.bin`, no alignment | `0x360` | `0x82` | `0xFF` | | over the plaintext |
| `VM` | `0x24000` (absolute) | `0x57000` | `0x12` | V012 `0x81`, V013 `0x80` (❓ meaning) | `00 10 00 00` | `0xFFFF` |
| `PRCT` | `0` | `0x24000` (= VM start) | `0x92` | `0x82` | `CODE` | `0xFFFF` |
| `BTIF` | `0x7B000` | `0x1000` | `0x92` | `0x81` | `AUTO` | `0xFFFF` |
| `EXIF` | `0x7C000` | `0x1000` | `0x92` | `0x81` | `AUTO` | `0xFFFF` |
| `USRFLASH` | `0x7D000` | `0x2000` | `0x92` | `0x81` | | `0xFFFF`; last entry (index 1) |

Files have offsets relative to the directory; regions have absolute flash offsets. `PRCT` (the
protected range) ends where the VM starts, and the VM start is where the SFC window ends.

**Resource tail** ✅, right after the last file (at `app_area_head.size`), outside the area CRC:

1. a JLFS entry `config.dat`: offset `0x20` (relative to the entry itself), size `0x180`, attr
   `0x82`. Its data CRC covers only the **first `0x160` bytes**; the last `0x20` differ between
   V012 and V013 in four bytes. What the SDK reads from it is ❓;
2. the `0x180` bytes of `config.dat`;
3. the MD5 of the plaintext `app.bin` as 32 lowercase hex digits.

## Update containers

**`.fwsc`** (the vendor's file) ✅: the first 20 blocks of 0x30 bytes each carry 0x2F bytes of the
UFW and one metadata byte; metadata byte *i* (1-based) is a character plus *i*: the name, `_`, a
3-digit version, then a pad value repeated to the end (`0x40` on the mixer; it happens to equal
the encoding of the version's last digit, so the parser takes 3 digits). Other M-Vave files use 36
blocks.

**UFW** ✅: a 0x40-byte header (ENC `0xFFFF`; CRC16 over bytes 2..63 in bytes 0..1; total length at
4; chip name `AC636N` at 16), then 0xA0-byte file records, each ENC `0xFFFF` on its own: data CRC
at 4, offset at 8, size at 12, flash address at 0x1C (only `USRFLASH`), name at 0x40.

| File | What it is |
|---|---|
| `flash.bin` | Flash `0x0..0x24000` in physical form: sector 0 as above, the application area SFC-encrypted ✅ |
| `USRFLASH` | The control tables for flash `0x7D000`, stored plain in flash. In the UFW it is ENC-encrypted for transport with key `0x980F ^ ((position in the UFW) >> 2)` per 32-byte block ✅ (decrypts to the device's DAW table); its record CRC does not match the stored bytes (❓ over what) |
| `ota.bin`, `flash2.bin`, `tail.bin` | Update loaders, a second application image, a service tail (SPEC §3.2) |

## How the uboot starts the application

The mechanism comes from open-smc-pad's reading of the uboot; its relevant outcome is now
✅ on this mixer (first relocated-image start, 2026-11-10; STOCK return, 2026-11-14). The
uboot reads `app_dir_head` from sector 0, loads the directory there, takes `app_area_head`'s offset
as the entry point, maps the CPU address `entry & ~0xFFFF` onto the directory's flash address
through the SFC, and jumps to the entry. The application finds its files and regions through the
same directory. Consequences for us:

- an area elsewhere in flash must still be linked at `0x1E00120`, and only an area at `0x1000 +
  k × 0x40000` keeps the SFC keys of the stock position (on 512 KB: `0x41000`);
- the two images need directories/regions of their own, with sector 0 selecting one. The pad's
  failed attempt was a warning, not proof that our chosen coexistence layout cannot work:
  ✅ ours and stock have run in turn with this layout ([bringup.md](bringup.md); [app.md](app.md), "Firmware");
- each sector-0 rewrite still has a power-loss risk: demonstrated starts do not settle R8
  (ROM recovery with sector 0 deliberately left erased).

## Building our area

```sh
python3 -I tools/image/flashimg.py build --app build/app/app.bin --cfg CFG.bin -o area.bin --physical area-phys.bin
```

`area.bin` is the plaintext area (what `0x23` would read back), `area-phys.bin` what goes into flash
from `0x1000` up to the VM start (`--vm START:SIZE`, stock `0x24000:0x57000`). The directory is the
stock one with our files; the resource tail is added only when given (`--tail`). Normal working
installs omit `config.dat`; its contents/purpose remain open. The SDK's own packer (`isd_download`) does not run in our
container (missing X11 libraries); this replaces it for the application area.

## Installing: the dry run

[`tools/image/installplan.py`](../tools/image/installplan.py) works out the §3.4 installation from
an image of the flash and our `app.bin`. It writes nothing to the device and does not talk to one:

```sh
python3 -I tools/image/installplan.py materials/dumps/smc-mixer_012_dump1.bin --app build/hwtest/app.bin \
    --cfg deps/fw-AC63_BT_SDK/cpu/br25/tools/cfg_tool.bin --out materials/install/hwtest
```

`--cfg` (or `--no-cfg`, on purpose) is required: ✅ without `cfg_tool.bin` every start of our SDK
build ends in the SDK's first `syscfg_read()` and the chip resets (bringup.md, "Step 4: boot
marks", runs 3 and 4). It happened again on 2026-11-11/12, when controller 19 and seven bisect images
were planned without it ([core.md](core.md), "The MIDI queue under load").

The image is the stock firmware's read or a raw ROM read; sector 0 tells which.

### The layout

The tool builds this layout ✅ (V012 dump):

| Range | What |
|---|---|
| sector 0 | Unchanged except `app_dir_head`: its offset `0x1000` → `0x41000` (bytes `0x60..0x66`: the header CRC and the offset) |
| `0x1000..0x24FFF` | The stock area and the stock VM's data: untouched, so a rollback only has to restore sector 0 |
| `0x25000..0x34FFF` | The configuration's two flash slots (`firmware/store/cfgstore.h`; [config.md](config.md), "Storage"), written by our firmware; an installation leaves them alone. Here since 2026-11-12 (SPEC Q-23; ✅ saves and reads run here the same day, [config.md](config.md), "At the new place") |
| `0x35000..0x36FFF` | The panel's state, two sectors (`firmware/store/statestore.h`; [config.md](config.md), "The panel's state"), written by our firmware; an installation leaves them alone, the rollback erases them with the slots |
| `0x41000` up | Our area: our 9-slot directory, `app.bin` at `0x120` (entry `0x1E00120` = flash `0x41120`), SFC-encrypted |
| after it, up to `0x6E000` | Our VM (the SDK's settings; at least 8 KB, our board's `CONFIG_VM_LEAST_SIZE`) |
| `0x6E000..0x7AFFF` | The stock update loader: kept |
| `0x7B000..0x7EFFF` | `BTIF`, `EXIF`, `USRFLASH`: declared as in the stock directory, untouched |

Our directory holds:

- `app.bin`, plus `cfg_tool.bin` and the resource tail when given;
- `VM`: right after the area, where the SDK's packer puts it (`VM_ADR` AUTO), up to the stock
  update loader (`installplan.py`'s `VM_END`); the configuration's slots lie below the area
  (`CFG_AT`, which a test keeps equal to `CFGSTORE_FLASH_AT`) and are in no directory region;
- `PRCT`: from 0 up to our VM, the way the stock one ends at its VM (`PRCT_LEN` = `CODE_LEN`);
- `BTIF`, `EXIF` and `USRFLASH`, copied from the stock directory.

Historical early-build sizes (not the current controller/BLE image sizes):

| Image | Area | VM |
|---|---|---|
| `hwtest` | `0x41000..0x51C8F` | `0x52000..0x6DFFF` (112 KB) |
| `app` | `0x41000..0x53CEB` | `0x54000..0x6DFFF` (104 KB) |

Why `0x41000`: the SFC keys each block on address bits 2..17. They are counted either from the CPU
address the SFC maps (the area's own flash offset) or from flash `0x1000`; the stock area cannot
tell the two apart. Only at `0x1000 + k × 0x40000` do both give the same bytes, and on 512 KB that
is `0x41000` alone. The tool encrypts under both and refuses a slot where they differ.

### What it checks

- **The flash now.**
  - Sector 0 parses and its boot pointer is the stock `0x1000`.
  - The stock area reads as the uboot reads it: CRCs, and the entry point on `app.bin`'s first
    byte.
  - It lists the sectors in use.
- **Our area.**
  - The area and our VM fit.
  - Both key models agree.
  - `BTIF`/`EXIF`/`USRFLASH` lie outside our ranges, and the loader outside our VM.
  - All our sectors are erased in this flash.
- **The writes.** They are applied to the image as NOR flash takes them: an erase sets `0xFF`, and
  programming only clears bits. In order:
  1. Check by CRC-16 (`jlrom crc`) that the device holds this flash.
  2. Write our area into erased sectors.
  3. Change the boot pointer: erase sector 0 and write it back with the one entry changed.
  4. Reset.
- **The flash after**, followed the way the ROM and the uboot follow it, under both key models:
  - sector 0 → `0x41000` → our directory (all CRCs) → `0x1E00120` = flash `0x41120`, the first byte
    of our `app.bin`;
  - only sector 0 bytes `0x60..0x66` and our sectors change;
  - the stock area, `BTIF`, `EXIF`, `USRFLASH` and every other sector in use stay byte for byte the
    same.
- **Our firmware's first run.** Its VM is ours. `BTIF` is written only into an empty slot (below).
- **The rollback.** Write sector 0 back (the stock area was never touched), then erase our sectors.
  Applied to the flash after, this gives the flash before byte for byte.

### What it found on the V012 dump ✅

- **Sector 0 must be erased.** The boot pointer entry cannot be changed by programming alone: 4 of
  its bits go from 0 to 1. So sector 0, with the uboot in it, has to be erased and rewritten. A
  power loss in between leaves no uboot, and then only the ROM download mode (the RP2040 key) gets
  the chip back. That is C-6.
- **BTIF stays as it is.** Our build registers two SDK items in `BTIF`: `CFG_BT_MAC_ADDR` (102) at
  `0x7B000` and `CFG_BT_FRE_OFFSET` (110) at `0x7B00A`, 6 bytes each. The table is the hid
  example's `user_cfg.c`; from our image's disassembly:
  - an item has a 4-byte header: length << 20 | id << 8 | the low byte of its CRC-16;
  - the SDK writes an item only into an empty slot.

  On the dump the MAC item is valid, so our firmware reads the stock MAC address and writes nothing
  to `BTIF`. Item 110 is empty, and our firmware only reads it. The rest of `BTIF` holds data the
  SDK's table does not cover.
- **`compare.py` agrees.** Its independent SFC code confirms the tool's model of the dump (base
  `0x1000`, all 4480 blocks; sector 0 and the plain area equal). It also decodes our `app.bin`
  from the image after, under both key bases.

### What `--out` writes

- `area-phys.bin`: what goes to `0x41000`;
- `sector0-after.bin` and `sector0-before.bin`: the change and its rollback;
- `flash-before.bin` and `flash-after.bin`: the whole flash before and after;
- `plan.json` and `plan.txt`.

They hold vendor code and Bluetooth data. The tool refuses an `--out` inside the repository unless it
is under `materials/`, `logs/` or `build/` (C-7).

### Installing again

The plan also takes a flash where our area is already there (`Plan.found`, in the plan's JSON as
`found`):

- **`earlier`**: our directory at `0x41000` parses (an earlier installation, after going back
  below): the sectors from `0x41000` up to `VM_END` are ours to rewrite. Sectors of the old area or
  its VM that the new one does not cover are erased first ("our earlier sectors"), so nothing stale
  is left for the uboot or the SDK's VM. The configuration's slots and the panel's state are kept:
  the new version starts with the configuration saved before. This is the update's second half.
- **`stopped`**: the area's sectors hold only this installation's bytes or erased pages, a page at
  a time (a run cut before sector 0 was written): they are rewritten. A page that is neither stops
  the plan ("our sectors are in use").
- `''`: a fresh flash, every sector of ours erased.

The installer writes the area's sectors from the second on and the first (the directory) last,
then sector 0; a cut anywhere before sector 0 leaves M-Vave's firmware starting, and a second run
finishes the job. `tools/image/test_installplan.py` and `tools/editortest/run.mjs` cover both
(the latter simulates cable cuts at nine points of a run and runs again). ✅ Normal `earlier`
reinstallation/Update ran in the browser on 2026-11-14 and 2026-11-23, over USB and BLE, with
physical off/on and configuration read-back. Physical cut-and-rerun (`stopped`) recovery is not
verified. The editor's firmware job belongs to the connected device, so a reopened wizard cannot
start a second job during a write (R09, fixed; [app.md](app.md), "Firmware").

## Going back

Since the installation leaves M-Vave's area at `0x1000` untouched, going back is sector 0 with
`app_dir_head` pointing at `0x1000` again: one entry, the step 3 above undone. Our firmware does it
on request (link command STOCK, [link.md](link.md); `firmware/store/bootsel.c`):

1. **Check** (writes nothing): M-Vave's area reads as the uboot reads it (its `app_area_head`, the
   CRC of the area after it, the entry point inside one of its files), and sector 0 parses with
   `app_dir_head` at `0x41000`. Any other answer leaves the flash alone (statuses: link.md).
2. **Write**: sector 0 as read, with the entry's offset at `0x1000` and its header CRC redone,
   checked by parsing again; then erase, program its non-blank pages, read back; up to three tries.
3. **Reset**, about 100 ms after the reply. M-Vave's firmware starts. Our area, VM, slots and
   state stay in flash unused, and installing again finds them (`earlier`, above).

On BR25 sector 0 and M-Vave's area are read through `sfc_read`, which below `0x41000` returns the
raw flash XORed with the `0xFFFF` keystream (undone by `enc_ffff`, `flash_br25.c`), and written with
`sfc_write`, which stores the bytes as given. Only sector 0 can be erased or programmed through
these operations (`flash_br25_boot_ops`). The sector buffer (4 KB) is allocated for the command
only.

The risk is the installation's: between the erase and the end of the write (about 100 ms) a power
loss leaves no uboot, and the recovery key is the way back ([recovery.md](recovery.md)).

✅ On the host: `firmware/tests/test_bootsel.c` (a NOR model with write protection, failed
programs and failed reads) on a synthetic stock area, and locally on the real dumps (the mixer as
installed, read by ROM, against the stock read): the sector written equals the stock one byte for
byte, and pointing it forward again gives the installed one. devhost `--boot` runs it behind the
link for `tools/smcconf/test_device.py`, the transcripts and the editor's live test (T-2L).
✅ The check on the mixer (controller 36, 2026-11-14, `smclog cmd stock check`): ok, pointer
`0x41000`: sector 0 and M-Vave's whole area come back through `sfc_read` as stored, and the area
checks. ✅ The write too (`smclog cmd stock back`, the same day): ok at the first write, pointer
`0x1000`; the mixer restarted on M-Vave's firmware (`SINCO`, `SMC-Mixer_012`). So our build's flash
driver erases and writes sector 0, and the uboot follows the changed pointer.

An update is the two together: our firmware goes back, M-Vave's firmware starts, and the editor
installs the new version through its service protocol (`earlier`, above): SysEx on USB MIDI,
or raw packets over Web Bluetooth AE40, not stock BLE-MIDI SysEx. See [app.md](app.md),
"Firmware".

### Still open

- The stock write (`0x22`) works on the mixer and takes sector 0 as stored (✅ 2026-11-10,
  bringup.md "Step 2b"); its driver checks only the end of the flash, not `PRCT` (⚠️ static
  analysis). ✅ Our area was written/read-checked through stock in step 3 the same day; browser
  installations followed on 2026-11-14/15. Arbitrary-sector protection is not characterized.
- The selected write path is the editor in the browser over stock USB SysEx or BLE AE40 (the owner, 2026-11-14;
  SPEC Q-18). Its plan is this tool's in JavaScript (`app/editor/js/installplan.js`, held to it by
  `tools/editortest/run.mjs`): [app.md](app.md), "Firmware".
- The uboot's complete internal behavior is not independently traced; the relocated-image start
  and changed-pointer return are confirmed (I1–I3 in [bringup.md](bringup.md)).
- ✅ `cfg_tool.bin` is needed: omitting it reset every start in `syscfg_read(CFG_BT_NAME)` (run 3).
  The SDK's default file passed that stage (run 4) and is used by subsequent working builds;
  unrelated later startup failures remain in the run record. `config.dat` is not supplied by
  the normal plan/release path, which has run; its contents/purpose remain undecoded.
- Whether the stock firmware writes past its VM's first sector while our area is being written.
  Its data sits in sector `0x24000` only. ✅ So far it appends there: three mode switches added
  24 bytes at `0x240A1` and changed nothing else ([hardware.md](hardware.md), "Stock firmware internals").
  When the sector fills and how it is then compacted is not known.

## Still unknown

- The flash header's fields and `uboot.boot`'s data CRC.
- What `config.dat` holds. The tested normal path omits it; `cfg_tool.bin` is required (above).
- What `res` means on `VM` (`0x81` on V012, `0x80` on V013).
- ~~That the physical flash equals this model~~ ✅ It does (2026-11-10, two raw reads through the
  ROM mode, recovery.md): the loader reports the chip key `0x980F`; the whole area below
  `0x24000` is the stock read SFC-encrypted with that key from base `0x1000` (4480 of 4480
  blocks); sector 0 is the stock read through the `0xFFFF` keystream; the rest is equal.
