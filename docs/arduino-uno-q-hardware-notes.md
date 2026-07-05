# Arduino UNO Q hardware notes: flashing, console, and a known ML-DSA bug

This documents everything discovered getting the ML-DSA/ML-KEM PQC work
actually running on real Arduino UNO Q hardware (STM32U585, paired with a
Qualcomm SoC) that isn't obvious from the standard Zephyr/MCUboot docs. It
supplements the walkthrough in [readme-zephyr.md](readme-zephyr.md).

## 1. `west flash` does not work on this board

The board exposes no external SWD/JTAG debug probe over USB. `ioreg`/`lsusb`
shows only one composite USB device (the Qualcomm SoC side, `vid=0x2341
pid=0x0078`) — no ST-Link, no J-Link. `west flash` (via any of the board's
configured runners: `stm32cubeprogrammer`, `openocd`, `jlink`) will fail with
"No debug probe detected", no matter what tools are installed on the host,
because the actual SWD connection to the STM32 is internal GPIO bit-banging
on the Qualcomm SoC's Linux side, only reachable through Arduino's own
`remoteocd` bridge tool (itself invoked over `adb`).

**Do not chase `west flash` errors on this board — install `arduino-cli` and
its `arduino:zephyr` core on the host instead** (`brew install arduino-cli`,
then `arduino-cli core install arduino:zephyr`). That pulls in
`arduino:remoteocd`, the tool that actually bridges flash commands to the
board over `adb`.

### Flashing the bootloader

`arduino-cli burn-bootloader`/`upload` don't expose a way to point at an
arbitrary custom-built binary for the *bootloader* region — only at the
Arduino core's own bundled factory firmware, or (for `upload`) at a
proprietary `-zsk.bin` "sketch" packaging format meant for application
images, not raw bootloaders. Call `remoteocd` directly instead, using its
bootloader-specific OpenOCD recipe:

```bash
# Get the board's USB serial number
adb devices -l

REMOTEOCD=$(ls ~/Library/Arduino15/packages/arduino/tools/remoteocd/*/remoteocd | head -1)
ADB=$(ls ~/Library/Arduino15/packages/arduino/tools/adb/*/adb | head -1)
CFG=$(ls -d ~/Library/Arduino15/packages/arduino/hardware/zephyr/*/variants/arduino_uno_q_stm32u585xx | head -1)/flash_bootloader.cfg

"$REMOTEOCD" upload --adb-path "$ADB" -s "<serial>" -f "$CFG" \
  boot/zephyr/build/zephyr/zephyr.elf
```

This works because `flash_bootloader.cfg` does a plain
`flash write_image erase ${filename0}` using the ELF's own embedded load
address — no packaging step needed, unlike app "sketches".

### Flashing a signed application image

There is no equivalent stock recipe for writing an arbitrary **raw** signed
`.bin` at a fixed flash offset (slot0), since Arduino's own "sketch" flow
always goes through their `-zsk.bin` wrapper. Use a small custom OpenOCD
config modeled on `flash_bootloader.cfg`, parameterized with slot0's address
(from the board's devicetree, e.g. `grep -A3 slot0_partition
build/zephyr/zephyr.dts` — it's `0x08010000` for the current
`arduino_uno_q` partition layout):

```tcl
# /tmp/flash_app.cfg
reset_config srst_only srst_nogate srst_push_pull connect_assert_srst
init
reset
halt
flash erase_address 0x08010000 0x68000
flash write_image ${filename0} 0x08010000 bin
flash verify_image ${filename0} 0x08010000 bin
reset
shutdown
```

```bash
"$REMOTEOCD" upload --adb-path "$ADB" -s "<serial>" -f /tmp/flash_app.cfg \
  path/to/signed-app.bin
```

Running the exact same command a second time and confirming no
`Error: verify failed` line appears is a reliable way to check a flash
actually persisted.

## 2. Seeing console output requires external wiring — and a specific tool

The board's `zephyr,console` is `usart1` (see
`zephyr/boards/arduino/uno_q/arduino_uno_q-common.dtsi`), routed to pins
`PB6`/`PB7`, which map to the classic Arduino header pins **D1 (TX)** /
**D0 (RX)** (`arduino_r3_connector.dtsi`). This UART is **not** bridged to
the Qualcomm/Linux side in any way reachable via `adb shell` — it only goes
to the external header, same as a classic Arduino Uno's hardware serial.

(Separately, the manual describes a **JCTL** connector as a 1.8V debug UART
for the *Qualcomm SoC's own Linux console* — a completely different UART on
a different chip. It's not useful for seeing MCUboot/Zephyr STM32-side
output, and its 1.8V logic level would need a level shifter if used with a
generic 3.3V/5V USB-TTL adapter.)

To see the STM32 side's console:
- Wire a 3.3V-logic USB-TTL adapter: adapter GND → board GND, adapter RXD →
  board D1, adapter TXD → board D0 (optional, only for sending input). Leave
  the adapter's power pin disconnected — the board is already powered.
- **Use `screen`, not `stty` + `cat`.** On at least one common
  CH340-family adapter, `stty -f /dev/cu.usbmodemXXXX 115200` followed by a
  separate `cat` process silently resets the baud back to 9600 before `cat`
  opens the port. `screen` sets the baud atomically as part of opening the
  device:
  ```bash
  screen -L -dmS pqcconsole /dev/cu.usbmodemXXXX 115200   # logs to screenlog.0
  # ... trigger a reset ...
  screen -S pqcconsole -X quit
  cat screenlog.0
  ```

## 3. Zephyr v4.3.0 → v4.4.1 compatibility fixes (already applied, commit `073dce16`)

Building this fork's `boot/zephyr` port against Zephyr v4.4.1 (the latest
stable release; the walkthrough originally targeted v4.3.0, the minimum for
`arduino_uno_q` board support) surfaced two real upstream compatibility
breaks, unrelated to the PQC work:

1. Zephyr renamed `zephyr/devicetree/partitions.h` → `fixed-partitions.h`,
   and `PARTITION_EXISTS()`/`PARTITION_ID()` → `FIXED_PARTITION_EXISTS()`/
   `FIXED_PARTITION_ID()`. Fixed across `boot/zephyr/main.c`,
   `boot_serial_extension_zephyr_basic.c`, `flash_map_extended.c`,
   `include/target.h`, `include/sysflash/sysflash.h`.
2. `boot/zephyr/Kconfig`'s `MBEDTLS_CONFIG_FILE` entry was missing its
   `string` type declaration — newer Kconfig tooling treats this as a fatal
   error rather than a warning.

## 4. Known bug: ML-DSA bootloader verification crashes above a size threshold

**Status: unresolved, real, reproducible. Not caused by application code.**

Signing and booting an application image with ML-DSA (either PQC-only or
hybrid mode) works up to a point, then causes a hardware lockup
(`clearing lockup after double fault` in OpenOCD, CPU parked in Zephyr's
`arch_system_halt`) immediately after MCUboot hands off to the app — before
the app's own first line of code runs.

### What's confirmed

- **Not a bug in application code.** Reproduced with `pqc_selftest`'s
  self-test logic fully disabled (just a banner `printk`), with
  `pqc-zephyr-crypto` linked into the *unmodified* `zephyr/samples/hello_world`
  (which never calls any of its functions), and with zero PQC code at all —
  a plain `hello_world` padded with an inert `static const` array to a
  comparable size crashes identically.
- **Not a stack-size issue in the simple sense.** The bootloader's own
  `CONFIG_MAIN_STACK_SIZE` needed to go from the default 10240 to 32768 to
  reliably verify *any* real signed image (see `overlay-mldsa44-pqconly.conf`
  and `overlay-mldsa44-hybrid-ecdsa.conf` for that fix, which is real and
  necessary on its own). But once past that floor, further increases (tested
  up to 65536) do not raise the size at which larger images crash — ruling
  out a simple "not enough stack, scaling with image size" explanation.
- **Not a flash-write reliability issue.** Explicit `flash verify_image`
  after every write confirms the flashed content exactly matches what was
  signed before observing the crash.
- **Not a stale-instruction-cache-after-debug-reset issue.** A full board
  power cycle (`adb reboot`, which restarts the whole board, not just a
  debug-probe soft reset) does not change the outcome.
- **Not specific to PQC-only mode.** Hybrid mode (ECDSA + ML-DSA, both
  required) has a *higher* size threshold before crashing, but still
  crashes eventually. Observed boundary (hybrid mode, `arduino_uno_q`,
  ML-DSA-44 + ECDSA-P256, current bootloader build): a 39672-byte signed
  image boots successfully; a 40171-byte signed image (499 bytes larger)
  crashes. This tight, non-round-number margin suggests a specific
  boundary-condition bug (e.g. in TLV-walking, image-data buffering, or
  flash-region-crossing logic) rather than smooth resource exhaustion.
- **Not header-size/`CONFIG_ROM_START_OFFSET` mismatch** (a real, separate
  bug that was found and fixed during this investigation — see below — but
  is not the cause of this one; confirmed matching in every crashing case).

### What's still open

The exact mechanism hasn't been root-caused. Getting further requires
proper register-level debugging (CFSR/HFSR, a real backtrace at the fault
site) that wasn't reliably obtainable through this board's `adb`-bridged
OpenOCD access during this investigation — GDB connections to the OpenOCD
gdbserver it exposes (port 3333, `adb forward`-able) dropped immediately,
likely because the hardware lockup state itself is too unstable for normal
DAP register access via this bridge.

### Reproduction

```bash
# Known-good baseline (works): plain hello_world, ECDSA-signed, any size tested.
# Known-bad: same image content, PQC-only or hybrid ML-DSA signed, above ~40KB.

west build -b arduino_uno_q -d build-x zephyr/samples/hello_world -- \
  -DEXTRA_CONF_FILE=path/to/hello-mcuboot.conf
python3 scripts/imgtool.py sign --header-size 0x400 --pad-header --align 4 \
  --slot-size 0x68000 --version 1.0.0 \
  -k root-ec-p256.pem --pqc-key root-mldsa44.pem \
  build-x/zephyr/zephyr.bin build-x/zephyr/signed.bin
# Flash per section 1 above, observe console per section 2.
# MCUboot logs "Jumping to the first image slot", then nothing further --
# the app never prints anything, and a debug connect shows the CPU parked
# in a lockup.
```

### Practical workaround

Keep signed application images under the working threshold (empirically
~39.7KB in hybrid mode with the current bootloader build) until this is
root-caused. For `pqc_selftest`, this means testing ML-DSA and ML-KEM
self-tests separately (smaller binaries) rather than together, or trimming
logging/test vectors to reduce image size.

### Fixed along the way (real bugs, not related to the above)

While investigating this, several genuine, separate bugs were found and
fixed:

- **`pqc-zephyr-crypto`'s `module.yml` was at the repo root; Zephyr's
  `zephyr_module.py` only scans `<module>/zephyr/module.yml`.** A root-level
  file is silently ignored — no error, just undefined Kconfig symbols
  wherever the module's options were referenced. Fixed by moving the file
  under `zephyr/`.
- **`zephyr_library_include_directories()` vs `zephyr_include_directories()`
  scoping.** The former only exposes a directory to the *current library's
  own* sources; the public API headers meant for consuming applications
  need the global `zephyr_include_directories()`.
- **Two comments containing a literal `*/` mid-sentence**
  (`pqc_mldsa_*/pqc_mlkem_*` and `ext/{mldsa,mlkem}-native/*/src/...`) closed
  their C block comments early, leaving the rest as invalid code. Both fixed
  by rewording.
- **`imgtool sign --header-size` must match the app's own
  `CONFIG_ROM_START_OFFSET`**, not an arbitrary docs example value. A
  mismatch here causes MCUboot to jump to the wrong offset and hard-fault
  immediately — a different, well-understood failure mode from the size bug
  above (confirmed via `grep CONFIG_ROM_START_OFFSET build/zephyr/.config`
  matching the `--header-size` argument, in every build since).
