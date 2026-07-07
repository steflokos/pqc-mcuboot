# Quickstart: PQC secure boot on the Arduino UNO Q

This guide takes a new checkout to a signed `hello_world` image booting
through post-quantum secure boot on real hardware, with the ML-KEM/ML-DSA
on-device self-test passing. It covers the direct path only. For the full
reference — every Kconfig option, every ML-DSA parameter set, tamper-test
recipes, and the hardware bring-up methodology behind the fixes this guide
already incorporates — see [readme-zephyr.md](readme-zephyr.md) and
[arduino-uno-q-hardware-notes.md](arduino-uno-q-hardware-notes.md).

## Overview

This project adds post-quantum signature verification (ML-DSA, FIPS 204) to
MCUboot's Zephyr port, alongside classical signatures, targeting the
STM32U585 (Cortex-M33) side of an Arduino UNO Q. Three verification modes
are supported and have been verified end-to-end on physical hardware:

- **Conventional** — classical ECDSA-P256 signature only.
- **Hybrid** — an ECDSA-P256 signature *and* an ML-DSA signature must both
  independently verify; a failure in either is rejected. This is the
  recommended configuration for production use, since it does not depend on
  the long-term security of either algorithm alone.
- **PQC-only** — ML-DSA alone, no classical signature.

All three ML-DSA parameter sets (44/65/87) are supported in hybrid and
PQC-only mode. A companion module, `pqc-zephyr-crypto`, separately exposes
ML-KEM (FIPS 203) and ML-DSA to applications (independent of the bootloader),
with Known-Answer Tests verified against independently generated reference
vectors, not just internal self-consistency.

## Prerequisites

- Python 3 with a working `python3 -m venv`.
- [`west`](https://docs.zephyrproject.org/latest/develop/west/index.html)
  (installed via pip below).
- The Zephyr SDK (installed via `west sdk install` below).
- [`arduino-cli`](https://arduino.github.io/arduino-cli/) with the
  `arduino:zephyr` core installed (`arduino-cli core install
  arduino:zephyr`). This board has no external SWD debug probe; flashing and
  debugging go through a bridge (`remoteocd`) that `arduino-cli` provides,
  tunneled over `adb`. `west flash` is not supported on this board.
- `adb` (bundled with `arduino-cli`'s toolchain, or installed separately).
- A 3.3V-logic USB-TTL serial adapter for console output. The STM32 console
  UART is not reachable over the `adb`/USB link; it is routed to the
  external header pins D0(RX)/D1(TX) only. See
  [arduino-uno-q-hardware-notes.md §2](arduino-uno-q-hardware-notes.md#2-seeing-console-output-requires-external-wiring--and-a-specific-tool)
  for wiring instructions.

## Fresh checkout

```bash
mkdir -p ~/zephyr-pqc && cd ~/zephyr-pqc
python3 -m venv .venv && source .venv/bin/activate && pip install west

west init -m https://github.com/steflokos/pqc-zephyr-manifest --mr main .
west update

pip install -r zephyr/scripts/requirements-base.txt
pip install -r bootloader/mcuboot/scripts/requirements.txt

west sdk install --install-dir ~/zephyr-pqc/zephyr-sdk

# Vendored crypto libraries: both the bootloader-side and app-level modules
# require their submodules to be initialized.
git -C bootloader/mcuboot submodule update --init --recursive
git -C modules/lib/pqc-zephyr-crypto submodule update --init --recursive
```

Every command below assumes an active shell in `~/zephyr-pqc` with the venv
active:

```bash
source .venv/bin/activate
export ZEPHYR_SDK_INSTALL_DIR=$PWD/zephyr-sdk
```

## One-time flashing and console setup

```bash
REMOTEOCD=$(ls ~/Library/Arduino15/packages/arduino/tools/remoteocd/*/remoteocd | head -1)
ADB=$(ls ~/Library/Arduino15/packages/arduino/tools/adb/*/adb | head -1)
BOOTCFG=$(ls -d ~/Library/Arduino15/packages/arduino/hardware/zephyr/*/variants/arduino_uno_q_stm32u585xx | head -1)/flash_bootloader.cfg
SERIAL=$($ADB devices | awk 'NR==2{print $1}')

# OpenOCD recipe for flashing a raw signed application image to slot0.
# (0x08010000 / 0x68000 match this board's default partition layout; read
# them back from <build>/zephyr/zephyr.dts's slot0_partition node if that
# layout has been changed.)
cat > /tmp/flash_app.cfg << 'EOF'
reset_config srst_only srst_nogate srst_push_pull connect_assert_srst
init
reset
halt
flash erase_address 0x08010000 0x68000
flash write_image ${filename0} 0x08010000 bin
flash verify_image ${filename0} 0x08010000 bin
reset
shutdown
EOF

# Serial console: identify the adapter's device path first
# (ls /dev/cu.usbmodem* on macOS), then:
screen -L -dmS pqcconsole /dev/cu.usbmodemXXXX 115200
```

Use `screen`, not `stty`+`cat`: on common USB-TTL adapters, setting the baud
rate with `stty` in one process and reading with a separate `cat` process can
silently reset the baud rate before `cat` opens the port. `screen` sets it
atomically.

## Build, sign, and boot: hybrid mode

Hybrid mode is the recommended starting point, since it is the configuration
most representative of production deployment — a compromise in either the
classical or the post-quantum algorithm alone does not compromise the
device.

```bash
# Bootloader
west build -b arduino_uno_q -d build-boot-hybrid bootloader/mcuboot/boot/zephyr -- \
  -DEXTRA_CONF_FILE=$PWD/bootloader/mcuboot/samples/zephyr/overlay-mldsa44-hybrid-ecdsa.conf \
  -DCONFIG_BOOT_INTR_VEC_RELOC=y

# Application
west build -b arduino_uno_q -d build-hello-hybrid zephyr/samples/hello_world -- \
  -DEXTRA_CONF_FILE=$PWD/bootloader/mcuboot/samples/zephyr/hello-mcuboot.conf

# Sign with both keys. Do not add --pad-header -- see Troubleshooting below
# for why this flag silently breaks boot on this target.
cd bootloader/mcuboot
python3 scripts/imgtool.py sign --header-size 0x400 --align 4 \
  --slot-size 0x68000 --version 1.0.0 \
  -k root-ec-p256.pem --pqc-key root-mldsa44.pem \
  ../../build-hello-hybrid/zephyr/zephyr.bin ../../build-hello-hybrid/zephyr/signed-hello-hybrid.bin
cd ../..

# Flash the bootloader, then the signed application
"$REMOTEOCD" upload --adb-path "$ADB" -s "$SERIAL" -f "$BOOTCFG" build-boot-hybrid/zephyr/zephyr.elf
"$REMOTEOCD" upload --adb-path "$ADB" -s "$SERIAL" -f /tmp/flash_app.cfg build-hello-hybrid/zephyr/signed-hello-hybrid.bin

cat screenlog.0
```

Expected output is MCUboot's banner, signature-verification log lines, and
then `Hello World! arduino_uno_q/stm32u585xx`. See the verification
checklist below for the exact output.

**Conventional and PQC-only modes** use the identical procedure with a
different overlay and key: substitute
`overlay-mldsa44-hybrid-ecdsa.conf`/`-k root-ec-p256.pem --pqc-key
root-mldsa44.pem` with `overlay-ecdsa-p256.conf`/`-k root-ec-p256.pem`
(conventional) or `overlay-mldsa44-pqconly.conf`/`-k root-mldsa44.pem`
(PQC-only, no `--pqc-key`). ML-DSA-65/87 follow the same pattern with
`overlay-mldsa{65,87}-*.conf` and `root-mldsa{65,87}.pem`. Worked examples
for every combination, plus the tamper-test procedure (corrupt one byte of a
signature, reflash, confirm MCUboot rejects it), are in
[readme-zephyr.md's "Reproducible commands"
section](readme-zephyr.md#reproducible-commands-build-sign-flash-and-debug-all-three-modes).

## Debugging

No external SWD probe is required. The board's Qualcomm-side processor
exposes a CMSIS-DAP-style debug bridge over `adb`:

```bash
adb forward tcp:3333 tcp:3333
adb shell arduino-debug &

arm-zephyr-eabi-gdb -q build-boot-hybrid/zephyr/zephyr.elf
(gdb) target extended-remote localhost:3333
(gdb) monitor reset halt
(gdb) break do_boot
(gdb) continue
```

(`arm-zephyr-eabi-gdb` is under `zephyr-sdk/gnu/arm-zephyr-eabi/bin/`.) See
[readme-zephyr.md's GDB section](readme-zephyr.md#debugging-with-gdb-no-external-probe-needed)
for further detail, including the known issues covered under Troubleshooting
below.

## Running the ML-KEM + ML-DSA self-test (KATs)

The `pqc-zephyr-crypto` module's `pqc_selftest` sample verifies both
algorithms on-device: self-consistency round trips (keygen/sign/verify,
keygen/encaps/decaps, tamper rejection) and genuine Known-Answer Tests
against vectors generated independently on the host, rather than checked
only against themselves. Standalone build, no bootloader required:

```bash
west build -b arduino_uno_q -d build-pqc-selftest modules/lib/pqc-zephyr-crypto/samples/pqc_selftest
"$REMOTEOCD" upload --adb-path "$ADB" -s "$SERIAL" -f "$BOOTCFG" build-pqc-selftest/zephyr/zephyr.elf
cat screenlog.0
```

Expected output is a PASS line for every check and a final
`PQC_SELFTEST: ALL PASS`. To run this as an MCUboot-signed image instead —
verifying the whole chain together — add
`-DEXTRA_CONF_FILE=modules/lib/pqc-zephyr-crypto/samples/pqc_selftest/mcuboot.conf`
to the build and sign/flash it following the `hello_world` procedure above.

## Verification checklist

**Bootloader + application (any mode):**
```
*** Booting MCUboot ... ***
I: Starting bootloader
...
I: Jumping to the first image slot
*** Booting Zephyr OS build v4.4.1 ***
Hello World! arduino_uno_q/stm32u585xx
```

**Tamper test (any mode):** flash a valid image and confirm the output
above, then flash a variant with one signature byte flipped:
```
E: Image in the primary slot is not valid!
E: Unable to find bootable image
```

**KAT self-test:**
```
ML-DSA round trip (pubkey=1312 priv=2560 sig=2420 bytes)
  PASS: keygen
  PASS: sign
  PASS: verify valid signature
  PASS: reject tampered signature
ML-DSA-44 KAT#0 (empty message, from gen_KAT44)
  PASS: verify KAT signature over empty message
ML-KEM round trip (pubkey=1184 priv=2400 ct=1088 ss=32 bytes)
  PASS: keygen
  PASS: encaps
  PASS: decaps
  PASS: shared secrets match
ML-KEM-768 KAT#0 (from gen_KAT768)
  PASS: decapsulated shared secret matches KAT expected value
PQC_SELFTEST: ALL PASS
```

## Troubleshooting

**Symptom:** the board hard-faults immediately after `I: Jumping to the
first image slot`; the application never prints, on any image size or
signature type.
**Cause:** `--pad-header` was passed to `imgtool sign`. This board's
applications are built with `CONFIG_ROM_START_OFFSET` set, meaning
`zephyr.bin` already reserves header space with leading zero padding for
`imgtool` to fill in place. `--pad-header` instructs `imgtool` to insert an
*additional* header-sized gap on top of that reservation, so MCUboot's jump
target lands on unused padding instead of the application's vector table.
**Fix:** never pass `--pad-header` for this target (the commands in this
guide already omit it). Full root-cause analysis:
[arduino-uno-q-hardware-notes.md §4](arduino-uno-q-hardware-notes.md#4-root-caused---pad-header-corrupts-the-vector-table-offset-not-a-size-or-pqc-bug).

**Symptom:** the bootloader crashes specifically during ML-DSA verification
(debug logging shows execution stopping mid-line immediately after
`bootutil_verify_sig_mldsa`).
**Cause:** stack overflow — `CONFIG_MAIN_STACK_SIZE` set too low.
**Fix:** all overlays referenced in this guide already set `65536`, which is
confirmed sufficient for all three ML-DSA parameter sets on this board and
toolchain; apply the same value in any custom overlay. Details:
[arduino-uno-q-hardware-notes.md §5](arduino-uno-q-hardware-notes.md#5-ml-dsa-stack-size-measured-across-all-three-parameter-sets-446587).

**Symptom:** a tamper test appears to boot successfully despite a corrupted
signature.
**Cause:** the flash write silently skipped rewriting a sector nearly
identical to what was already present, leaving valid content in place.
**Fix:** flash a genuinely different image (or the known-valid image)
immediately before flashing the tampered variant, then retest.

**Symptom:** `adb shell arduino-debug` fails with `Error requesting gpio
line swdio`, or a GDB session reports `no more connections allowed`.
**Cause:** a previous OpenOCD process is still holding the debug GPIO, or
the single debug-server connection slot is occupied.
**Fix:**
```bash
adb shell pkill -9 openocd
adb shell arduino-debug &
```
Ensure any GDB script ends with `quit` so the connection is released
cleanly — the board's OpenOCD build accepts only one GDB client at a time.

## Roadmap

The following are identified next steps, not covered by this guide:

- **TrustZone-M**: running PQC signing/decapsulation inside the Cortex-M33
  Secure world, via a custom TF-M Secure Partition wrapping
  `pqc-zephyr-crypto`'s ML-DSA/ML-KEM implementation, so that key material
  and the cryptographic code itself are never executable from Non-secure
  state. This requires a new `arduino_uno_q_stm32u585xx_ns` board port —
  Trusted Firmware-M is the only in-tree, working mechanism for the
  Secure/Non-secure split and handoff on this SoC — and enabling `TZEN`, a
  board-wide option-byte change that triggers a full flash mass-erase.
  Scoped as a separate follow-on project.
- **Debug-access lockout (RDP)**: raising the STM32's readout-protection
  level so physical/SWD access can no longer read out flash or reflash the
  device. This is the logical next hardening step after secure boot itself,
  and is deliberately not applied here: doing so disables the `adb`/`remoteocd`
  bridge this guide depends on, and requires a documented recovery path
  before being applied to any board still in active use for development.
- **Anti-rollback / security counter**: MCUboot's
  `MCUBOOT_DOWNGRADE_PREVENTION_SECURITY_COUNTER` is present in this fork's
  Kconfig but is not yet enabled or exercised. Without it, nothing currently
  prevents reflashing an older, previously valid signed image.
- **Application-level demonstrations beyond KATs**: an ML-KEM-based key
  exchange (TLS-style handshake) between two peers, and an ML-DSA signing
  demonstration relevant to automotive use cases — for example, signing a
  firmware-update manifest or a simulated UDS/CAN payload.
