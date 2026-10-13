# Hardware validation: first-board bring-up (XIAO nRF52840 + Wio-SX1262)

**Feature set frozen.** Nothing here adds controller functionality; the lab build only adds
visibility (boot report, read-only diagnostics, counters) and a dry-run guard. **Nothing in
this package has been run on hardware, and the design is not production-ready.** Every
"expected" value below is a prediction to be confirmed or refuted on the bench. No drone
testing, no drone-specific logic.

Contents: [1 Builds](#1-builds) · [2 Flashing](#2-flashing) · [3 Boot log](#3-first-boot-log) ·
[4 Commands](#4-commands) · [5 Procedures T1–T9, F1–F10](#5-procedures) · [6 Pass/fail table](#6-passfail-table) ·
[7 Size impact](#7-size-impact) · [8 Remaining risks](#8-remaining-hardware-risks)

## 1. Builds

Run from the repository root. Each profile has its own directory; artifacts in each:
`merged.uf2` (drag onto the bootloader drive), `merged.hex` (SWD), `dfu_application.zip` (BLE OTA).

| Purpose | Command | Output directory | Dry run |
|---|---|---|---|
| **Lab** (first boot, radio, BLE, RSSI, A+1 capture) | `./build-meshcore.sh` | `updater/build_xiao_ble_nrf52840_meshcore_lab/` | **ON** |
| **Dry run** (same config as lab, separate directory so the two never get mixed up) | `MC_PROFILE=dryrun ./build-meshcore.sh` | `updater/build_xiao_ble_nrf52840_meshcore_dryrun/` | **ON** |
| **Actual DFU** (lab diagnostics, real flashing) | `MC_DISABLE_DRY_RUN=yes MC_PROFILE=dfu ./build-meshcore.sh` | `updater/build_xiao_ble_nrf52840_meshcore_dfu/` | **OFF** |
| Production candidate (not part of this phase) | `MC_PROFILE=prod ./build-meshcore.sh` | `updater/build_xiao_ble_nrf52840_meshcore_prod/` | OFF |

* `lab` and `dryrun` are the same configuration by design: the first-board image is already
  non-destructive. Use `dryrun` when a second, clearly named image is wanted next to `lab`.
* `CONFIG_MC_DFU_DRY_RUN` defaults to **y**. A build that does not say otherwise cannot flash.
  The `dfu` profile sets `=n` in `boards/meshcore/xiao_ble_wio_sx1262_dfu.conf`, and both
  `build-meshcore.sh` and `CMakeLists.txt` **refuse to build it** unless `MC_DISABLE_DRY_RUN=yes`
  is set. The boot log states which kind of image is running (`dry_run=YES ...` / `dry_run=NO -- REAL DFU ENABLED`).
* What dry run does: ARM, target matching, RSSI qualification, digest checks, token check and
  the one-shot START authorisation all run. In `dfu_runner_start()` the authorisation is consumed,
  then it returns before any scan, connection, erase or write. START replies
  `DRY RUN PASS FW <id> <addr> b=<bundle> avg <dBm>` and the state returns to IDLE with
  `STATE START_AUTHORIZED -> IDLE : dry run complete, nothing flashed`.
* Put the operator key in `boards/meshcore/operator.conf` (`CONFIG_MC_OPERATOR_PUBKEY="<64 hex>"`, git-ignored) before building.
* **A dry-run image never executes the DFU code path** (T9 and F1–F10 need the `dfu` image).

## 2. Flashing

1. Plug the XIAO into USB. **Double-tap RESET**; a `XIAO-SENSE`/`XIAO-BLE` drive appears (LED pulses).
2. Copy the `merged.uf2` of the profile onto the drive. The board reboots when the copy completes.
3. Open the USB serial port at 115200 8N1 (e.g. `picocom -b 115200 /dev/ttyACM0`). Capture the
   whole session to a file (`picocom --logfile`); T9 depends on it.
4. Within a few seconds the boot report appears ([§3](#3-first-boot-log)).
5. Send commands as MeshCore direct messages from the **operator node** (the one whose public
   key is in `operator.conf`). Add the XIAO as a contact from its `advert` (first advert 15–30 s after boot).
   There is no command shell on the USB console; USB is read-only log output.

Recovery: double-tap RESET always returns to the UF2 bootloader (it is not touched by the app).

## 3. First-boot log

Illustrative, **not captured from hardware**; the wording is what the code prints, the values are made up.
Secrets (private key, identity page contents, operator secret) are never printed. The operator
fingerprint is the first 8 bytes of SHA-256 of the operator public key:
`echo -n <64-hex> | xxd -r -p | sha256sum | cut -c1-16`.

```
==== AIRDFU LAB BOOT ====
git 3f9a1c07be52 profile lab dry_run=YES (nothing will be flashed)
reset cause 0x1 (power-on)
hw id 7c1e4a90d25b3308
meshcore pub 9b2d...(64 hex)...e41f
operator key fpr sha256[0..8]=a41c90e2b7d03f58 (set)
identity partition 0xf3000-0xf3fff (4096 B)
identity record: valid, loaded; slot 3/127 seq 3
sx1262 spi/gpio ready: yes
sx1262 pins BUSY=0 DIO1=0 RESET=0 (1=asserted)
sx1262 init: ok rc=0
radio 910525000 Hz BW62500 SF7 CR4/5 22 dBm sync 0x12
ble ready: yes
bundle: fw-4631-1.14.zip 301212 B
bundles: 1
target: exact addr+type from ARM; A+1 allowed, dfu uuid required, dfu name '4631_DFU'
rssi: avg>=-65 floor=-75 hyst=3 hold=5000ms min_samples=8 max_gap=3000ms
AIRDFU LAB READY
```

On a fault the last line is `AIRDFU INIT FAILED: <reason>` with reason one of `identity storage`,
`identity`, `sx1262 spi/gpio not ready`, `sx1262 init`, `ble not ready`. The lines above it show
what was reached. First boot after a fresh flash shows
`identity record: EMPTY page: new identity generated` (expected exactly once; see T3). Verify the actual
frequency/BW/SF/CR/power (defaults 910.525 MHz, 62.5 kHz, SF7, CR 4/5, 22 dBm, sync 0x12) against the MeshCore network the other node is on before T6.
A zero bundle count is not an init failure.

## 4. Commands

All are MeshCore DMs. Non-privileged commands are answered to anyone who can reach the node;
privileged ones only for the configured operator key (and need a single-use token).

| Command | Privileged | Lab only | Purpose |
|---|---|---|---|
| `status` | no | | State, armed FW id, gate line, DFU progress |
| `scan [page]` | no | | BLE advertisers heard, strongest first |
| `rssi` | no | | Live gate line for the armed target |
| `arm <AA:BB:CC:DD:EE:FF> <bundle> [random\|public] [tok]` | yes | | Select target and bundle; records SHA-256 |
| `disarm` | yes | | Clear selection |
| `start [tok]` | yes | | From ARMED only; real DFU, or `DRY RUN PASS` in a dry-run image |
| `abort` | yes | | Cancel (refused once the target app is erased) |
| `version` / `battery` / `log [page]` | no | | As before |
| `diag` | no | yes | `up=<s> rst=<hex> lora=up/DOWN ble=up/DOWN/scan\|idle id=ok/new st=<STATE> q=<queue> lost=<dropped adv samples>` |
| `radio` | no | yes | freq, BW, SF, CR, TX dBm, `rx=` `tx=` counts, last RSSI/SNR, `crc=` `hdr=` `busy=` (BUSY timeouts) `spi=` (SPI faults) `txf=` `txt=` (TX timeouts) `rxf=` (RX-restore failures) `irq=` (DIO1 interrupts) |
| `ble` | no | yes | `scan=on/idle adv=<total adverts> dfuadv=<adverts with DFU UUID> match=<last armed-address advert> n= cur= avg= min= dfu=<last DFU-UUID advertiser> <rssi> rel=same/A+1/other` |
| `storage` | no | yes | Re-reads the identity page: `id=valid,key matches` or `INVALID`, slot use, bundle count, selected bundle, its SHA-256 prefix |

In a non-lab build these four answer `<cmd>: lab build only`. Every state change is also written to the
USB console as `STATE <old> -> <new> : <reason>` and kept in the `log` ring. Reasons seen on the happy
path: `ARM accepted`, `MAC matched`, `hold complete`, `bundle digest verified`, `valid operator token`,
`permit consumed`, `success` (or `dry run complete, nothing flashed`). The state names are the existing
ones (IDLE, WAITING, TARGET_DETECTED, RSSI_QUALIFIED, ARMED, START_AUTHORIZED, DFU_ACTIVE).
Replies are limited to 150 characters; if a reply is cut off, record the console line instead.

Operator workflow for privileged commands: send `status` to get `tok=xxxxxx`, then send the command
with the token; or send it bare and resend with the token the node asks for. Tokens are single-use.

## 5. Procedures

Use the **lab** image for T1–T8; the **dfu** image for T9 and F1–F10. Record
every result in [§6](#6-passfail-table). Stop at the first unexplained failure; do not "tune around" it.

### T1. XIAO boots (item 1)
1. Flash `lab`. Capture the console. 2. Expect the full report ending `AIRDFU LAB READY`.
3. Repeat 10 power cycles (unplug/replug) and 10 double-tap RESET to bootloader then RESET. Note the reset cause each time.
4. `diag` after each: `lora=up ble=up id=ok`.
**Pass:** READY every boot, no WATCHDOG/BROWNOUT/LOCKUP cause, same `meshcore pub` every time.
**Fail/stop:** any `INIT FAILED`; record the reason line and the three pin states.

### T2. SX1262 initialisation (item 2)
1. In the boot log: `spi/gpio ready: yes`, `sx1262 init: ok`, radio parameters equal the intended network's.
2. `radio` on a fresh boot: `busy=0 spi=0 txf=0 txt=0 rxf=0`. 3. Leave idle 10 min; `radio` again.
**Pass:** error counters stay 0; `irq=` grows only with real traffic or noise. Wrong pin states or a
`sync word read-back` error in the log mean wiring/pins; stop and fix before T4.

### T3. Identity persistence over UF2 (item 5)
Goal: prove the UF2 bootloader never clears `mc_identity_partition` (0xF3000-0xF3FFF).
1. Flash `lab`. Record `meshcore pub` (call it K) and the `identity record` line. First boot says "new identity generated".
2. Power-cycle. Key = K and the line says "valid, loaded". Run `storage`: `id=valid,key matches`.
3. Double-tap RESET, copy the **exact same** `merged.uf2` again. Key = K.
4. Build and flash a *different* lab image (e.g. `dryrun`, or any rebuild after a one-line change). Key = K.
5. Enter the bootloader and leave it (double-tap, then RESET) 10 times. Key = K each time. Also: flash `dfu`; key = K.
6. Repeat 3 with the board connected via a different USB port and after a full unplug.
**Pass:** K identical at every step; no "new identity generated" after step 1.
**If the key ever changes: STOP. Report "mc_identity_partition is not safe"** with the step number and the
boot-log lines before and after. Do not continue to T9.

### T4. Two-node LoRa smoke test (item 6)
Needs a known-good MeshCore node (the *operator node*, same frequency/BW/SF/CR/sync word).
1. Record `radio` (counters before). 2. From the operator: `version` -> reply received (outbound-from-XIAO and inbound).
Watch the console: `lora rx ... rssi= snr=` then `lora tx ... rc=0`.
3. Repeat `status`, `version`, `battery` alternately, 100 messages total, one every 10-20 s (about 25 min). Never two within 5 s.
4. After every 10: `radio`. After 100: `radio` and `diag`.
5. Also wait for one unsolicited XIAO advert and confirm the operator node sees the contact.
**Pass:** all 100 get a reply; `rx` and `tx` each grew by about 100; `busy=0 spi=0 txf=0 txt=0 rxf=0`; the next
command after each reply is still received (RX resumes after TX); `lost=0`; no reboot (`up=` monotonic).
Record the RSSI/SNR range seen. **Fail:** any non-zero error counter or an unanswered command: save the console and counters.

### T5. BLE + LoRa coexistence (item 7, no DFU)
1. `lab` image (idle BLE scan is kept alive by `MC_LAB_KEEP_SCANNING`). Put at least one BLE advertiser nearby (phone or the RAK).
2. `ble` at t=0 (`scan=on`, `adv=` counting). 3. For 10 min, every 15-30 s send a command from the operator node (rotate `status`, `scan`, `ble`, `radio`).
4. Every 2 min record `ble` and `radio`. 5. At the end `diag`.
**Pass:** `adv=` rises continuously; every command answered; `radio` errors stay 0; `lost=0` (non-zero = BLE samples dropped;
record how many); `rx`/`tx` consistent with messages sent; no reset (`rst` unchanged, `up=` ≈ 600+). Count any command
that needed a resend. Do not arm or start anything.

### T6. RAK4631/OTAFIX identification (item 8)
Spare RAK4631 running OTAFIX, application mode. The goal is to **capture** real behaviour, not to change code.
1. Find the RAK's application address A (`scan`; or the RAK's own tooling). Note its address type.
2. Arm it (dry-run image, any bundle; the sole purpose is to put A under watch):
   `arm A <bundle> <random|public>` (with token). Wait for `WAITING -> TARGET_DETECTED : MAC matched`.
3. Record `ble` (`match=A`) and the console line for A.
4. Put the RAK into DFU mode (`start ota` on the RAK's own console/serial, or the "enter bootloader" action it supports).
5. Watch the console for `DFU-UUID advert <addr> rssi <r> name '<name>' vs armed <A>: ...`. The printed verdict is one of
   `SAME address`, `armed+1`, `DIFFERENT (not A, not A+1)`. Also record `ble` (`dfu=<addr> rel=`).
6. Record: application address A, observed DFU address, address type for both, advertised name, whether the DFU UUID is
   present, and the RSSI.
7. Repeat 3 times, power-cycling the RAK between runs, to see whether the DFU address is stable.
8. Also check the negative case: a second BLE device (phone) advertising something at A+1 without the DFU UUID must not be accepted
   (it never produces a `DFU-UUID advert` line).
**Pass:** the observed DFU address is A+1 (`armed+1`), same address type, name `4631_DFU` or empty, every run.
**Anything else (`SAME`, `DIFFERENT`, or varying):** record it and stop. **Do not change the target-identity code until
this capture is reviewed** (item 8's rule); the controller fails closed (refuses START) on a mismatch.

### T7. RSSI qualification (item 9; collect data, do not tune)
At ~3, 10, 20 and 30 ft line-of-sight between the XIAO and the armed RAK (application mode; same antenna orientation each time).
Per distance:
1. `disarm`, then `arm` (fresh gate). Start a stopwatch at `WAITING -> TARGET_DETECTED`.
2. Poll `rssi` every ~2 s (the lab console also prints a `gate` line every 2 s while armed). Record instantaneous RSSI, rolling average,
   minimum, and the advert count (`ble`: `n=`).
3. Record `TARGET_DETECTED -> RSSI_QUALIFIED : hold complete` (qualification time) and, if
   it never qualifies, the reported reason.
Then, at a distance where it qualified:
4. **Five-second hold:** qualification must not come earlier than ~5 s after the first accepted advert.
5. **Hard floor:** move or shield the RAK so a single advert reads below the floor (default -75 dBm); expect an immediate drop out of the
   qualified state and a restart of the hold.
6. **Hysteresis:** with the average hovering within 3 dB of the threshold (default avg -65), qualification must not chatter; note how
   many transitions you see in 60 s.
7. Make the RAK disappear (power off): expect the gate to go stale within `max_gap` (3 s) and the state to fall back.
**Do not change `CONFIG_MC_AVG_RSSI_MIN` etc.** The table is the deliverable.

### T8. Dry run end to end (before any real flash)
Lab image, RAK in application mode and a bundle on the XIAO's littlefs (load it the same way as stock AirDFU, via the web uploader).
1. `arm A <bundle>` -> `ARMED` after qualification; confirm `FW <id>` equals the first 8 hex of the bundle's SHA-256
   (`sha256sum <file> | cut -c1-8`) and `storage` shows the same.
2. `start` (with token) -> `DRY RUN PASS FW <id> <addr> ...` and state IDLE. 3. `start` again -> refused (not armed).
4. Repeat arm/start 5 times. 5. Confirm on the RAK that nothing changed (no reboot, no DFU mode entered, same firmware version).
6. Negative gates in dry run: START before qualified, a wrong bundle name, a stale/used token, a bundle replaced after ARM
   (`DRY RUN` must not be reachable; the reply names the reason).
**Pass:** DRY RUN PASS only when every gate holds; RAK untouched. This must pass before T9.

### T9. First real DFU (item 10) — `dfu` image
Preconditions: T3, T6, T7, T8 passed; T6 confirmed the DFU address rule on this hardware.
1. Build and flash the `dfu` image (§1). Boot log must say `dry_run=NO -- REAL DFU ENABLED`. Keep the XIAO on USB serial with logging on.
2. Keep the RAK physically accessible (reset button, USB, a way to reflash by cable). The RAK runs a **known-good older** MeshCore firmware; a
   **known-good newer Nordic DFU ZIP** is loaded on the XIAO. Record both versions and the ZIP's SHA-256.
3. Put the RAK into DFU mode. Confirm target identity: the `DFU-UUID advert ... armed+1` line (or arm the DFU address directly).
4. `arm`; confirm the `FW` id equals the ZIP's SHA-256 prefix. Wait for ARMED.
5. `start` (token). Capture the full console.
6. Expect MeshCore reports at start, 25 %, 50 %, 75 %, VERIFYING and a final result (reports are rate-limited to one per 4 s;
   a reply can be missed on air, so also record the console `dfu state=` lines).
7. The RAK reboots; MeshCore returns on the RAK; verify the new firmware version on the RAK.
8. Afterwards: the XIAO state returns to IDLE, `radio` counters show no errors, `storage` still `valid,key matches`.
**Pass:** all of 6-8. **Fail:** record where it stopped; recover the RAK by cable (bootloader/DFU by USB) if needed. No drone testing.

### F1–F10. Failure tests (item 11) — only after T9 passed
Each row: do it, then check the **expected safe behaviour**. Record the console and the RAK's state.

| # | Test | Expected safe behaviour |
|---|---|---|
| F1 | Move the updater out of BLE range **before** START | Gate goes stale/not-seen; state falls from ARMED back (to WAITING/TARGET); `start` is refused with the gate reason; RAK untouched. |
| F2 | Move the updater out of range **during** DFU | The upload stops; runner retries per upstream policy then fails; XIAO reports failure and returns to IDLE; the RAK (still in bootloader, app erased or not) remains recoverable: when back in range a new ARM/START completes, or the RAK is reflashed by cable. No hang, no auto-restart of DFU. |
| F3 | Power-cycle the updater **before** START | Reboots to IDLE (nothing armed, no token); `status` shows IDLE; `start` refused; same `meshcore pub`. |
| F4 | Power-cycle the updater **during** DFU | RAK stays in bootloader with a partial image (not bootable if app was erased). XIAO boots IDLE and does **not** resume by itself. Recovery: re-arm and START again (needs a fresh token), or cable reflash. Record whether the RAK recovered. |
| F5 | Wrong BLE target (arm A, but a different device is in DFU mode / A+1 absent) | START refused or no match; never connects to the other device (check it is untouched). Record the refusal reason shown. |
| F6 | Stale START token (wait past `CONFIG_MC_TOKEN_TTL_S` (120 s), then use it) | Refused: `needs token: resend with <new>`; no authorisation granted. |
| F7 | Second START (resend the same token, or START while DFU_ACTIVE) | Refused; the running DFU is not restarted; used token never accepted twice. |
| F8 | Bundle changed after ARM (replace/overwrite/rename the ZIP, same name) | Armed bundle is write/delete/rename-locked (blocked at the SMP/fs entry points); if the content differs anyway, START refused on digest mismatch. DFU never uploads a different file than armed. |
| F9 | Corrupted bundle (flip bytes in a copy, arm it) | `arm` records that file's digest and the runner's own ZIP validation must reject an invalid package before erasing; record exactly where (arm vs start vs runner) and that the RAK is untouched. |
| F10 | RAK loses power during DFU | The upload fails; XIAO reports failure and returns IDLE; on RAK power-up the bootloader is intact (Nordic bootloader keeps itself) and a new run works; record the RAK's state. |

## 6. Pass/fail table

Copy per session. Build = the line from the boot log (`git ... profile ...`).

```
Date/time:        Tester:           Board (hw id):         RAK fw before/after:
Build / git:      Profile:          dry_run:               operator fpr:
```

| Test | Step / measurement | Expected | Observed | P/F | Log file / notes |
|---|---|---|---|---|---|
| T1 | 10x power cycle: READY, same key | READY, no bad reset cause | | | |
| T2 | pins, SPI, init | `init: ok`, errors 0 | | | |
| T3 | key after: power cycle / same UF2 / other build / 10x bootloader | K unchanged | | | |
| T4 | 100 messages | 100 replies, errors 0, RX resumes | | | |
| T5 | 10 min BLE+LoRa | scan alive, all answered, no reset | | | |
| T6 | application A vs DFU address | A+1 (record actual) | | | |
| T7 | 3 ft | inst / avg / min / qual. time / adverts | | | |
| T7 | 10 ft | | | | |
| T7 | 20 ft | | | | |
| T7 | 30 ft | | | | |
| T7 | hard floor, hysteresis, 5 s hold | each behaves as specified | | | |
| T8 | dry run PASS + gates | PASS only when all gates hold | | | |
| T9 | real DFU 0/25/50/75/VERIFY/SUCCESS | all seen; RAK new version | | | |
| F1-F10 | one row each | see §5 F1–F10 | | | |

## 7. Size impact

Slot0 is 388,970 B; MCUboot unchanged (33,892 of 40 KB). Measured with the real Zephyr link, `xiao_ble/nrf52840`.

| Build | Flash | Spare | RAM | Notes |
|---|---|---|---|---|
| Hardening pass, lab (previous) | 380,940 B | 8,030 B | 130,096 B | before this phase |
| **lab / dryrun (dry run ON)** | **379,756 B** | **9,214 B** | 121,648 B | smaller: the whole DFU runner is unreachable and dropped |
| **dfu (dry run OFF, lab diagnostics)** | **386,980 B** | **1,990 B** | 130,288 B | **the tight one**: only ~2 KB left |
| prod (dry run OFF) | 369,288 B | 19,682 B | 130,160 B | previous 368,328 B: +960 B (state-reason strings, radio counters) |

Net cost of this phase in the real-DFU lab image: +6,040 B vs the previous lab build (diagnostic report,
four commands, counters, reasons). If the `dfu` image must grow, drop `CONFIG_MC_LAB` logging first. The lab
image is deliberately not the same binary as the dfu image: the DFU code path is only exercised in `dfu`.

## 8. Remaining hardware risks

* **No hardware run of any of this**, including the SX1262 driver (TCXO via DIO3, DIO2 RF switch, RX/TX enable, DIO1 interrupt, LBT),
  USB CDC logging, and everything in the boot report. Wrong BUSY/DIO1 pins, TCXO voltage or sync word show up in T1/T2.
* **UF2 bootloader vs the identity page** is unproven (T3). A UF2 that erases 0xF3000 would change the node key (operator contact breaks, and
  adverts change identity).
* **A+1 is documented behaviour, not observed** on this RAK/OTAFIX build (T6). The code fails closed, so the worst case is "never starts".
* **Dry-run builds do not exercise the DFU runner.** Only the `dfu` image does; its 2 KB flash margin leaves no room for fixes without a trim.
* **LoRa/BLE coexistence**: the nRF52840 BLE controller and SPI/DIO1 handling share one CPU; missed DIO1 events or `lost>0` are plausible under scan load (T5).
* **TX current at 22 dBm** on USB power or a small battery (brownout resets, T4); `rst` cause will show BROWNOUT.
* **Reply truncation (150 chars) and rate limiting** (one report per 4 s) mean progress reports can be missed over the air; the console is the record.
* **Radio mid-DFU**: an advert is also a flash write for the identity page; the node stays quiet during DFU, but a 10-hour idle soak was never run.
* **Token/replay protections are not security-reviewed**; a jammer can still block commands.
* Identity-page rollover has a power-loss window that loses the key (decades away at the current advert rate).
* Console is USB CDC only: if USB enumeration fails there is no log. The bootloader double-tap still works.
