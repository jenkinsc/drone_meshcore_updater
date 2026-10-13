# MeshCore command interface (XIAO nRF52840 + Wio-SX1262)

An optional layer that lets an operator drive the updater over LoRa using
MeshCore direct messages. The Nordic BLE DFU implementation is unchanged; this
layer only decides *when* a DFU may start and reports what it does.

> **Status: hardened first pass. Host-tested, builds and links, has NOT been run
> on hardware. Not production-ready.** Treat everything below as a design that
> still has to survive a bench. See [Known risks](#known-risks).

## Build

Run from the repository root (a west workspace with the Zephyr SDK installed):

```sh
./build-meshcore.sh                    # lab profile (default)
MC_PROFILE=prod ./build-meshcore.sh    # production-candidate profile
```

| Profile | Output directory | Adds |
|---|---|---|
| `lab` | `updater/build_xiao_ble_nrf52840_meshcore_lab/` | `boards/meshcore/xiao_ble_wio_sx1262_lab.conf`: `CONFIG_MC_LAB=y` (verbose LoRa / scan / gate / DFU logging). USB console and the littlefs log backend stay on. |
| `prod` | `updater/build_xiao_ble_nrf52840_meshcore_prod/` | `..._prod.conf`: compile-time log level capped at WARNING. USB console kept (see below). |

Both start from `prj.conf` + `boards/xiao_ble.conf` + `boards/meshcore/xiao_ble_wio_sx1262.{conf,overlay}`
(+ git-ignored `boards/meshcore/operator.conf`). Without `MC_WIO_SX1262` the
build is the stock one and none of this is compiled in.

Artifacts per profile: `merged.uf2` (flash this), `merged.hex` (SWD probe),
`dfu_application.zip` (OTA over BLE).

### Resource use (measured, `xiao_ble/nrf52840`, slot0 = 388,970 B)

| Build | Flash (slot0) | Spare | RAM (of 256 KB) |
|---|---|---|---|
| First pass (commit 91fb630) | 387,844 B (99.71 %) | 1,126 B | 130,992 B (49.97 %) |
| **Hardened, lab** | **380,940 B (97.94 %)** | **8,030 B** | **130,096 B (49.63 %)** |
| **Hardened, prod** | **368,328 B (94.69 %)** | **20,642 B** | 130,096 B (49.63 %) |
| MCUboot (unchanged) | 33,892 B of 40 KB (82.74 %) | | 16,512 B |

What moved (by archive, from the link map; first pass → lab): CryptoCell-310
platform library −9.4 KB, Bluetooth host −2.1 KB (GATT caching, verbose assert
text, read-multiple off), libzephyr −0.8 KB, PSA core −0.3 KB; Oberon software
driver +0.5 KB (the DRBG now runs on it); MeshCore controller + glue +3.2 KB
(the new state machine, token flow, digest, target rules, path guard, lab
logging). Trims are in `boards/meshcore/xiao_ble_wio_sx1262.conf`, each with a
comment. The largest remaining items are the SoftDevice Controller (84 KB), BT
host (62 KB), littlefs (16 KB), MPSL (15 KB), Nordic legacy DFU (10 KB) and the
USB device stack plus UDC driver (18.5 KB).

Entropy: the MeshCore build drops the CryptoCell driver
(`HW_CC3XX=n`, `ENTROPY_CC3XX=n`) and reads the nRF RNG peripheral directly
(`ENTROPY_NRF5_RNG`, bias correction on; the DT overlay selects `&rng` and disables
`&cryptocell`). The identity seed and START tokens come from that hardware RNG.
This is **only** in the MeshCore build; the stock build is unchanged.

Not done: switching USB off (≈ −18 KB) needs changes in upstream files (console
choice, `usb_dfu_touch.c`); it is left as the next lever and is why the lab
build keeps USB as the recovery/debug tether.

## Flash map (generated, not assumed)

From the devicetree this image is built with. `mc_zephyr.c` carries
`BUILD_ASSERT`s derived from the same devicetree: the identity partition is exactly
one 4 KB page, page-aligned, inside flash, and overlaps none of the partitions
below. Move or grow any of them and the firmware build fails.

| Start | End (incl.) | Size | Name | Notes |
|---|---|---|---|---|
| 0x000000 | 0x026FFF | 156 KB | `reserved_partition_0` | MBR + SoftDevice S140 (read-only) |
| 0x027000 | 0x030FFF | 40 KB | `boot_partition` | MCUboot (image 33.9 KB) |
| 0x031000 | 0x091FFF | 388 KB | `slot0_partition` | application (signed image ends 0x8FB9C) |
| 0x092000 | 0x0F2FFF | 388 KB | `slot1_partition` | MCUboot secondary slot; swap-using-move, **no scratch partition** |
| **0x0F3000** | **0x0F3FFF** | **4 KB** | **`mc_identity_partition`** | **MeshCore identity + advert clock (this layer)** |
| 0x0F4000 | 0x0FFFFF | 48 KB | `uf2_boot_partition` | Adafruit UF2 bootloader, its settings and MBR params (read-only) |
| external QSPI | 2 MB | | littlefs | bundles, config, logs; Zephyr settings backend |

There is no NVS/ZMS/FDS partition on this board (`storage_partition` is deleted
upstream), so the identity page is a real, named devicetree partition rather than
a magic address. 0xF3000 was previously the unallocated 4 KB between slot1 and
the UF2 bootloader. MCUboot swaps only touch slot0/slot1; a UF2 drag only writes
the pages it contains (0x27000–0x8FB9C for `merged.uf2`); both leave 0xF3000 alone.
**Unverified:** that the Adafruit bootloader erases only pages it writes. The
bench checklist includes the test (reflash, node public key must not change).

The identity page holds the node's private key, public key and one 16-byte record
per update of the advert-clock high-water mark. Records are append-only (a rewrite
would wear one cell and tear on power loss); the clock is written at most once per
day of uptime, so the page's 248 records last decades. A power cut while the
page rolls over (once, decades away) can lose the identity; it is regenerated on
the next boot, which changes the node key (log warns at 80 % use).

## Wiring (Wio-SX1262 on XIAO nRF52840)

| Signal | Pin   | Signal | Pin   |
|--------|-------|--------|-------|
| NSS    | P0.04 | BUSY   | P0.29 |
| RESET  | P0.28 | DIO1   | P0.03 |
| RXEN   | P0.05 | SPI    | spi2 (P1.13/14/15) |

`i2c1` is disabled by the overlay because it shares P0.04/P0.05.
**Never transmit without the antenna attached.**

## Provisioning

1. Put the operator's 64-hex-character MeshCore public key in
   `updater/boards/meshcore/operator.conf` (git-ignored):
   `CONFIG_MC_OPERATOR_PUBKEY="<64 hex>"`
2. Build and flash. On first boot the node generates its own identity and logs
   `MeshCore node public key: <hex>`. Add that contact in your MeshCore client.
3. Send direct messages to the node. Radio defaults are the MeshCore US preset
   (910.525 MHz, SF7, BW 62.5 kHz, CR 4/5); change them in Kconfig (`MC_*`).

With an empty operator key every privileged command is refused (and a warning is
logged at boot).

## Commands

One line per message; the command word is case-insensitive; extra words are an
error, not ignored.

| Command | Privileged | Meaning |
|---|---|---|
| `status` | no | State, armed FW id, gate line, DFU progress |
| `scan [page]` | no | BLE advertisers heard, strongest first |
| `rssi` | no | Live gate line for the armed target |
| `arm <AA:BB:CC:DD:EE:FF> <bundle> [random\|public] [tok]` | **yes** | Select target and bundle; records the bundle's SHA-256 |
| `disarm` | **yes** | Clear the selection |
| `start [tok]` | **yes** | Begin DFU, only from ARMED |
| `abort` | **yes** | Cancel — see [Abort policy](#abort-policy) |
| `version` | no | Firmware version |
| `battery` | no | Battery voltage |
| `log [page]` | no | Recent events, newest first (RAM ring) |

### Operator workflow (tokens)

`arm` and `start` need a **single-use token** the node issues. Ask for it, then
use it:

```
you:  status                         node: IDLE tok=3fa91c
you:  arm EA:7C:11:0B:55:90 fw.zip 3fa91c
node: ARM ok FW 9c41ab07 EA:7C:11:0B:55:90 r b=fw.zip WAITING tok=71d0e2
...
you:  rssi                           node: TARGET -61 AVG -63 MIN -68 5.1/5s n12
you:  status                         node: ARMED FW 9c41ab07 EA:7C:11:0B:55:90 TARGET -61 ... exp43s tok=88b1c4
you:  start 88b1c4                   node: STARTED FW 9c41ab07 EA:7C:11:0B:55:90 b=fw.zip avg -62
```

A command sent without a token (or with a stale or wrong one) is not executed;
the reply is `... needs token: resend with xxxxxx`. A token is 24 random bits from
the hardware RNG, is spent the moment it is presented correctly, expires after
`MC_TOKEN_TTL_S` (120 s), and is never stored (a reboot discards it).
`disarm`, `abort` and the read-only commands take no token: they only remove
capability, so replaying them is harmless, and an emergency stop must not wait for a
round trip.

## State machine

```
IDLE ─arm→ WAITING ─target heard→ TARGET_DETECTED ─gate ready→ RSSI_QUALIFIED
     ─digest re-checked→ ARMED ─start→ START_AUTHORIZED ─runner takes it→ DFU_ACTIVE ─┐
 ▲                                                                                     │
 └──────────── done / failed / aborted / expired / disarmed / bundle changed ◄─────────┘
```

* `START_AUTHORIZED` exists only inside the `start` call (see below); it is never
  observable from outside and is never time-based.
* Backwards edges: the gate losing the target (silence, a sample under the hard
  floor, the mean collapsing) drops ARMED / RSSI_QUALIFIED to TARGET_DETECTED.
* Every transition is logged (`A -> B`) to the Zephyr log and the `log` ring.
* Nothing is persisted: a reboot always comes back **IDLE**. Nothing starts a DFU
  on its own; there is no timer, auto-start flag, or path from tick/advert to
  `dfu_start`.

## One-shot START authorisation (`mc_authz`)

The earlier 2-second permit is gone. `start`, with every check passed, performs
`IDLE → START_AUTHORIZED` (compare-and-swap) recording **who** may consume it (the
mc thread) and **what** it is for (the exact bundle path and pin string), then
calls the DFU runner on the same thread. `dfu_runner_start()` consumes it with a
single CAS to `DFU_ACTIVE` iff state, thread, path and pin all match. A second
consume — from any thread, with any arguments — finds `DFU_ACTIVE` and fails;
there is no window, and a failed attempt by another caller changes nothing (it
cannot steal or cancel the grant). The controller ends the authorisation when the
run finishes, fails, is aborted, or the runner refused to start.

All three DFU entry paths are gated by it with `CONFIG_MC_CMD_ENFORCE` (default
y): the `auto_flash` block in `main`, the SMP `TRIGGER_DFU` handler, and the
direct `dfu_runner_start()` API all end at the same check in `dfu_runner.c`.

## Bundle binding

`arm` streams the whole bundle through SHA-256 and stores the digest; the first
8 hex characters are the **FW id** shown everywhere (`FW 9c41ab07`). It is
re-computed when the gate completes (RSSI_QUALIFIED → ARMED) and again at
`start`; any difference cancels the arm (`last=bundle changed`). The id is fixed
for the arm session. The name is not trusted: same name, same size, different
content is caught (tested). While armed or running, the upstream write / delete /
rename entry points (SMP `fs_mgmt` file access, `fsx_mgmt` delete+move, `fsx_stream`
start) refuse to touch that file name or its directory (`mc_pathguard`).

## Target identity

`arm` takes the **physical** device address A and type T. Accepted advertisers,
for both the START gate and the DFU runner's pinned scan, are exactly:

| Seen | Accepted when |
|---|---|
| `A`, type `T` | always — this is the address the operator typed |
| `A + 1`, type `T` | only if the armed device was seen in **application** mode (never advertised the DFU UUID), `MC_TARGET_ALLOW_PLUS1=y`, **and** the packet carries the Nordic Legacy DFU service UUID `00001530-1212-EFDE-1523-785FEABCD123` (`MC_TARGET_REQUIRE_DFU_UUID=y`), **and** its name, if this packet carries one, equals `MC_TARGET_DFU_NAME` (`4631_DFU`) |

`A + 1` means the last octet incremented with carry across all six octets (the
Nordic bootloader / OTAFIX behaviour for a buttonless jump; the upstream scanner
uses the same rule). Everything else — another address, A + 2, A − 1, the wrong
address type, A + 1 without the DFU UUID, A + 1 with a different name — is
refused, and a name never makes a wrong address acceptable. Advertising data and
scan-response arrive as separate packets, so an absent name does not reject while
a wrong one does; the DFU client's GATT discovery of the exact service UUID after
connecting is the last check. If the armed address already advertises the DFU UUID
(device left in DFU mode), A *is* the DFU address and A + 1 is somebody else, so
it is refused.

**After a failed transfer** the target typically sits in its bootloader at A + 1
with no application. Run `scan`, find the DFU-named device, and `arm` **that**
address.

## RSSI qualification

All values are Kconfig, lab defaults shown:

| Option | Default | Meaning |
|---|---|---|
| `MC_GATE_HOLD_MS` | 5000 (min 5000) | Acquisition duration |
| `MC_AVG_RSSI_MIN` | −65 dBm | Rolling mean (last 16 adverts) must be ≥ this |
| `MC_RSSI_FLOOR` | −75 dBm | Any single advert below this throws the acquisition away |
| `MC_GATE_MIN_SAMPLES` | 8 | Adverts needed in the acquisition |
| `MC_GATE_MAX_GAP_MS` | 3000 | Silence longer than this restarts it |
| `MC_GATE_HYST_DB` | 3 | Once ready, the mean may sag to `avg_min − 3` before ready is lost |

`rssi` / `status` show, for example, `TARGET -61 AVG -63 MIN -68 4.2/5s n12` and,
when not ready, the reason (`acquiring`, `avg too weak`, `too few adverts`, `rssi under floor`,
`target went quiet`). `start` re-evaluates the gate at its own instant — a stored
verdict is never enough. **An active DFU is never aborted because RSSI later
drops**; the gate applies only to arming and starting.

## Abort policy

| When | `abort` does |
|---|---|
| Armed / qualifying | Clears the selection → IDLE |
| DFU active, before the DFU START packet (scanning, connecting, enabling, cooldown, awaiting PIN) | Stops the runner → IDLE (`last=aborted`) |
| DFU active, from the START packet on (starting, uploading, validating, disconnecting, verifying) | **Refused**: `ABORT refused: upload under way, target app already erased; it will finish or fail`. A single-bank Nordic bootloader has already erased the application; cancelling buys nothing and risks a worse state, and the runner has no clean mid-transfer cancel. |

## Fail-safe behaviour

| Situation | Result |
|---|---|
| Target disappears before `start` | Gate drops to TARGET_DETECTED; `START refused: ... target went quiet`; must re-qualify for the full hold |
| RSSI drops during qualification | Under-floor sample restarts the hold; weak mean never qualifies |
| ARM times out (`MC_ARM_TIMEOUT_S`, 60 s; 0 = never) | `ARM expired` report, IDLE, bundle unlocked |
| `start` while unarmed | `START refused: not armed`; no authorisation created |
| `start` with the armed target not (yet) qualified | refused with the gate line |
| Bundle changed after ARM | `START refused: bundle changed since ARM; IDLE` (or the arm is cancelled at qualification) |
| DFU connection fails / transfer drops / remote error | One `DFU FAILED <addr>: <reason>` message, IDLE, authorisation cleared |
| Runner refuses to start (busy) | `START failed (rc N); still ARMED`; authorisation cleared |
| Updater reboots during DFU | Comes back IDLE and inert; ~20 s after boot it sends `BOOT reset=0x<cause> IDLE`; the target is left in whatever state its bootloader reached (see above) |
| `abort` before / during DFU | See [Abort policy](#abort-policy) |
| After success or failure | IDLE; a new run needs a fresh `arm` + token |

## Reports and radio scheduling

During a DFU the only unsolicited messages are: the `STARTED` reply, **25 %,
50 %, 75 %**, **verifying** (if the runner reaches it), and **SUCCESS** or **FAILED**
(at most 5–6 messages per run, queued and at least `MC_REPORT_GAP_MS` (4 s) apart;
a newer milestone replaces a still-queued older one; terminal messages are never
dropped). There is no 0 %/100 % message (start and success cover them), and no
per-packet or per-second progress. Adverts are suppressed while a run is active.

Concurrency review of the LoRa side (`mc_radio.c`, `mc_zephyr.c`):

* One thread (`mc`, preemptible priority 8, below the DFU runner at 7) owns the
  SX1262, SPI2 and all MeshCore state. Nothing from the radio runs in BLE timing
  context: the BLE scan callback only posts a tiny event (a non-blocking
  `k_msgq_put`); the DIO1 interrupt only posts an event; DFU status goes through a
  latest-value slot.
* SPI transfers are ≤ 258 bytes at 4 MHz (≈ 0.5 ms), performed in thread context
  with the synchronous driver. The BUSY wait spins at most 2 ms, then sleeps in 1 ms
  steps, so a slow SX1262 never holds the CPU against the BLE threads.
* TX waits for completion by polling every 5 ms with `k_msleep` (≤ 8 s cap), listen-
  before-talk uses `k_msleep` — the thread yields throughout. RX is restored after
  every TX (and an error is logged and returned if it cannot be).
* If the mc thread is busy transmitting, advert samples queue (64 deep); an
  overflow resets the gate window (it can never make the gate *more* permissive).
* Not verifiable on a desk: RF/supply interaction of a +22 dBm 910 MHz transmission
  with a running BLE central on the same board — see the bench checklist.

## Replay protection

The earlier persisted "newest timestamp" rule let one wrong operator clock lock the
operator out, so it is gone. MeshCore messages carry only a sender timestamp, which
standard clients cannot make monotonic, so the authority is the **node-issued
single-use token** above:

* A recorded `arm`/`start` can be replayed, but its token is already spent (or
  expired, or issued before a reboot) — the node just answers with a fresh demand.
* A guessed token needs 2^24 luck *and* a valid MAC from the operator's key.
* `disarm` / `abort` / read-only commands are replayable by design; replay only
  removes capability or repeats a read.
* A small ring suppresses exact duplicates (repeater copies, retries) so they are
  ACKed but not re-run.
* Nothing is written to flash per command: no wear, no power-loss window. The only
  persisted value is an advert clock (≤ 1 write per day of uptime).
* Operator timestamps are not trusted for ordering. Ancient and far-future values
  are both processed (tested).

## Security model

* Privileged commands: operator public key only (MeshCore ECDH + AES-128 / HMAC),
  token-gated for arm/start. Unprivileged commands are read-only. Frames from any
  other key cannot be decrypted at all and get no reply.
* The identity lives in a dedicated flash partition, not on littlefs (the SMP file
  service can read/write any littlefs path).
* The operator key is compiled in; rotating it means rebuilding.
* LoRa is a shared, unauthenticated medium: anyone can jam it. Jamming blocks
  commands; it cannot start a DFU.

## Code layout

* `src/mc/` portable core (no Zephyr): `mc_gate`, `mc_target`, `mc_authz`, `mc_ctl`
  (controller), `mc_progress`, `mc_cmd`, `mc_replay`, `mc_pathguard`, `mc_layout`,
  wire/crypto/identity/store/node. Glue: `mc_zephyr.c`, `mc_radio.c`, `mc_sx126x.c`.
* `src/mc/ed25519/` vendored Ed25519 (size-reduced variants, cross-checked).
* Upstream edits (all under `CONFIG_MC_CMD[_ENFORCE]`, none in the Nordic DFU
  client): a status observer (`dfu_status.*`), an advert observer and a pinned-scan
  filter hook (`ble_scanner.*`), the authorisation check in `dfu_runner.c`, path
  guards in `fsx_mgmt.c`, `fsx_stream.c`, `upload_hook.c`, init and auto-flash gate in
  `main.c`, conditional wiring in `CMakeLists.txt` / `Kconfig`.
* Host tests: `cd updater/mc-tests && make test` — 18,695 checks, 0 failed (includes
  MeshCore-generated crypto/wire vectors, the layout arithmetic with the real map,
  one-shot authorisation, target identity, gate, progress throttling, tokens, every
  fail-safe row above, and store power-cut sweeps).

## Flashing the XIAO nRF52840

1. Build (above). Plug the board into USB.
2. **Double-tap RESET** quickly; the board shows up as a `XIAO-SENSE` (or `XIAO-BLE`)
   drive and its LED pulses.
3. Copy `updater/build_xiao_ble_nrf52840_meshcore_lab/merged.uf2` onto that drive. It
   reboots by itself when the copy finishes.
4. Open the USB serial port (115200 8N1, any terminal) — the lab build logs there.
   Expect `MeshCore node public key: ...` within a few seconds.

Alternatives: SWD probe with `merged.hex`; or, once any build is running, OTA over
BLE with `dfu_application.zip` (MCUboot swap; leaves the identity page alone).
The first flash after a bootloader change is the only time you need the double-tap.

## Bench-test checklist: first hardware session (XIAO + Wio, no drone)

Have the antenna on before anything transmits. Use a spare second MeshCore node
or phone app as the operator. Keep the board on USB with the serial log open.

1. [ ] Boots; log shows build, `MeshCore node public key: <hex>`, `radio up: 910525000 Hz SF7 ...`,
       `MeshCore interface up: avg>=-65 floor=-75 hold=5000ms min=8 arm=60s reset=0x...`.
2. [ ] Power-cycle: **same public key** (identity persisted). Repeat after re-dragging
       `merged.uf2` (verifies the UF2 bootloader leaves 0xF3000 alone) and after an OTA
       swap.
3. [ ] Add the node as a contact in the operator app; `status` → `IDLE tok=...`
       (`lora rx` / `lora tx` lines in the log).
4. [ ] Unauthorised sender (second key): no reply to anything.
5. [ ] `version`, `battery`, `log`, `scan` work. `scan` lists a known BLE device
       (BLE scan rows in the log).
6. [ ] `arm` a nearby BLE advertiser (phone running a BLE beacon app, or a spare RAK4631)
       with a **test bundle** copied to littlefs: without a token → "needs token"; with → `ARM ok FW xxxxxxxx`.
       Reuse the same command → "needs token" (token spent).
7. [ ] Watch the `gate` log lines every 2 s. At close range → `ARMED` after ~5 s. Move away /
       cover the beacon: drops to `TARGET_DETECTED`; a −80 dBm blip resets the hold.
       Note the real RSSI range of your setup and set `MC_AVG_RSSI_MIN` / `MC_RSSI_FLOOR`.
8. [ ] `start` without token, with a wrong token, with the right token while the target is
       far away → refused each time, **no DFU starts** (`dfu_runner`: "not in START_AUTHORIZED").
9. [ ] Replace/modify the test bundle (rename it, overwrite via the web client) after `arm`:
       write refused; if changed another way, `start` → "bundle changed".
10. [ ] Let ARM expire (60 s) → `ARM expired` message, IDLE.
11. [ ] From the web client / SMP, try "trigger DFU" and auto-flash with a bundle present:
       refused while IDLE.
12. [ ] Reboot with USB attached while ARMED: comes back IDLE; `BOOT reset=0x...` message after ~20 s.
13. [ ] Range/coexistence: while the BLE central is scanning (armed) send repeated `status` from the
       operator; no log errors (`TX did not complete`, `BUSY stuck`, stack/queue messages).
14. [ ] Record: LoRa TX current peak vs. USB supply (brown-out?), BLE adverts seen during a LoRa
       TX, any `queue overflow` / `gate reset: samples lost` lines.

Do not proceed to a live DFU until all of the above pass.

## Checklist: first live BLE DFU of a spare RAK4631

Use a **spare** RAK4631, never the drone. A bench power supply or USB, antenna on both radios.

Before:
1. [ ] Bundle is a known-good Nordic Legacy DFU zip for the RAK4631 (`fw.zip`), the same one that has
       already updated this RAK via the stock, un-gated updater. Check its SHA-256 on a PC.
2. [ ] RAK4631 runs its application and advertises; `scan` shows its address/name. Note address A
       and type (public/random). Confirm the OTAFIX bootloader's DFU name is `4631_DFU` (or set
       `MC_TARGET_DFU_NAME`) and that the DFU address is A + 1 (use a phone BLE scanner while it
       is in DFU mode, or accept that the first run proves it).
3. [ ] RAK recovery path ready: USB-serial/UF2 double-reset into the bootloader, known-good firmware
       on the PC.
4. [ ] Lab-build serial log open on the updater; second phone/radio as operator.

Run:
5. [ ] `status` → token; `arm A fw.zip <tok>` → note **FW id**; compare with the PC's SHA-256 prefix
       (first 8 hex chars).
6. [ ] Place the RAK within working range; wait for `ARMED`; confirm `rssi` line values are sensible.
7. [ ] `start <tok>` → `STARTED FW ...`. Log shows the authorisation consumed exactly once, then the
       runner scanning with `mc-id` rejections for anything but A / A + 1.
8. [ ] Before the upload begins (`connecting` / `enabling`) test `abort` → ABORTED and IDLE, RAK intact; re-arm.
9. [ ] Second attempt, let it run. Expect `DFU 25%`, `50%`, `75%`, `verifying`, `SUCCESS` messages
       only. Note times and the RAK's behaviour. No other LoRa traffic should appear in the log
       during the transfer except those reports.
10. [ ] During the upload try `abort` → must be refused with the "target app already erased" message
        and the run must continue.
11. [ ] After SUCCESS: controller is IDLE; RAK boots the new firmware. A second `start` → "not armed".
12. [ ] Failure drill (separate run): switch the RAK off mid-transfer → exactly one `DFU FAILED` message,
        IDLE; re-power the RAK, `scan`, find the `..._DFU` device, `arm` that address, finish the update.
13. [ ] Updater reboot drill: pull the updater's power mid-transfer, power it back: IDLE,
        `BOOT reset=...`; recover the RAK as in step 12.
14. [ ] Only after all of this, consider a drone, and then only with the RAK-tested procedure and a
        recovery plan.

## Known risks

* **No hardware run.** The SX1262 driver (SPI, TCXO, TX/RX, LBT), the real LoRa↔BLE↔DFU
  interaction, supply behaviour at +22 dBm, and the entire flow are untested on a board.
* The Adafruit UF2 bootloader's erase behaviour (does a UF2 drag ever clear 0xF3000?) is
  unverified; bench step 2 covers it. If it does, the identity regenerates (key change).
* Identity-page rollover has a power-loss window that loses the key (decades away).
* `A + 1` is the documented Nordic/OTAFIX behaviour; a bootloader that derives its DFU address
  differently will not be accepted (it fails closed, with an `mc-id` log line).
* Tokens protect against replay but ride on the same unauthenticated medium: a jammer can stop
  commands. Tokens live in RAM only.
* Abort during upload is deliberately refused; there is no clean cancel in the legacy DFU client.
* The RAM event log (`log`) is lost on reboot; the Zephyr log backend to littlefs keeps more.
* CC3XX was removed from the MeshCore build only; PSA crypto now uses the Oberon software driver
  (a different code path from the stock image — covered by host vectors but not by hardware).
* Flash margin: lab 8.0 KB, prod 20.6 KB; any further feature needs another trim (USB off is the
  big one).
* Nothing here has been security-reviewed by anyone other than its author.
