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

**Caveat for tamper testing specifically:** when reflashing a file that
differs from what's already in flash by only a handful of bytes (e.g. one
flipped byte inside a signature, for a tamper test), the write can silently
leave the *old* content in place — no `Error: verify failed` is printed, but
the board still boots the previous (valid) image, giving a false "tamper
test failed to catch corruption" result. Confirmed directly: a single-byte
flip inside an ML-DSA signature appeared to boot successfully once, but
reflashing the exact same tampered file right after a genuinely different
valid image (forcing a full rewrite) then correctly rejected it every time,
across nine different byte offsets tested. Always flash a substantially
different image (or re-flash the *valid* image first) immediately before
flashing a tampered variant, rather than reflashing a near-identical file
over itself.

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

## 4. Root-caused: `--pad-header` corrupts the vector table offset, not a size or PQC bug

**Status: resolved.** What was originally reported here as an ML-DSA-specific
"crashes above a ~40KB size threshold" bug was actually a signing-command bug
that reproduces on **any** signed image — any size, any signature type,
including plain ECDSA. It was only ever *observed* on larger ML-DSA/hybrid
images because those happened to be what got tested most; a plain, tiny
(20KB) ECDSA-signed `hello_world`, signed with the exact command this doc
used to recommend, reproduces the identical crash.

### The actual bug

`imgtool sign --pad-header` tells imgtool "this input has no reserved header
space, please prepend one." But with `CONFIG_ROM_START_OFFSET` set (as it is
for this board), Zephyr's linker already reserves `--header-size` bytes of
zero padding at the start of `zephyr.bin` itself, for imgtool's header to
overwrite in place. Passing `--pad-header` on top of that inserts a
*second*, redundant header-sized gap. The resulting file still has a
syntactically valid MCUboot header (correct magic, correct `hdr_size`,
correct `img_size`) at the start of the partition, so MCUboot happily parses
it, verifies the signature, and decides to boot — but the real vector table
now sits `--header-size` bytes further into the file than MCUboot's jump
target (`partition_base + header_size`) expects. MCUboot jumps straight into
what is still zero padding, and the CPU faults trying to execute/fetch
through a null vector table — the exact `clearing lockup after double fault`
symptom below.

Confirmed directly: dumping the signed `.bin` showed the real vector table
(`90 10 00 20 c1 0f 01 08 ...`, matching the ELF's actual reset vector) at
file offset `0x800`, not `0x400`, when signed with `--header-size 0x400
--pad-header`. Removing `--pad-header` moved it to exactly `0x400`, and the
board booted cleanly (`Hello World! arduino_uno_q/stm32u585xx` printed over
the console) with no other changes.

**Fix: never pass `--pad-header` when `CONFIG_ROM_START_OFFSET` is set for
the app build** (true for every config in this repo's `samples/zephyr/`).
`--header-size` must still match `CONFIG_ROM_START_OFFSET` exactly, as
already noted below, but that check alone doesn't catch this — a
size-matched `--header-size` with `--pad-header` still corrupts the offset.

Two other things were tried and ruled out while chasing this (kept here since
they're real, worthwhile fixes even though they weren't the cause):
`CONFIG_BOOT_INTR_VEC_RELOC` was off for this board (its default depends on
`CPU_HAS_ICACHE`/`CPU_HAS_DCACHE`, which STM32U5 doesn't select even though
it has a real I-cache under `CONFIG_ICACHE`/`CONFIG_DCACHE` — same gap
affected `BOOT_DISABLE_CACHES`'s default). Both are now fixed in
`boot/zephyr/Kconfig` and worth keeping enabled, but neither one, alone or
together, fixed this crash — only removing `--pad-header` did.

### Symptom (kept for reference)

Signing and booting an application image with `--pad-header` set (ML-DSA,
hybrid, or plain ECDSA) causes a hardware lockup (`clearing lockup after
double fault` in OpenOCD, CPU parked in Zephyr's `arch_system_halt`)
immediately after MCUboot hands off to the app — before the app's own first
line of code runs.

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
# Reproduces the crash, on ANY image, size, or signature type -- the bug is
# --pad-header, not ML-DSA or size:
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

# Fix: drop --pad-header. Boots cleanly, any size/signature type tested so far.
python3 scripts/imgtool.py sign --header-size 0x400 --align 4 \
  --slot-size 0x68000 --version 1.0.0 \
  -k root-ec-p256.pem --pqc-key root-mldsa44.pem \
  build-x/zephyr/zephyr.bin build-x/zephyr/signed.bin
```

### Practical workaround

None needed — see above. Drop `--pad-header` from every `imgtool sign`
invocation in this repo's docs/scripts when `CONFIG_ROM_START_OFFSET` is set
for the app build (true everywhere in `samples/zephyr/`). The `>40KB`
threshold this section used to describe was never actually about size —
re-verify any previously-"too big" image with `--pad-header` removed before
assuming it still fails.

**Re-verified directly:** a `hello_world` padded with a 25KB `volatile
const` array (46868 bytes signed, hybrid ECDSA+ML-DSA-44, well past the old
~40KB threshold) boots and verifies correctly with the fixes in this
document applied (no `--pad-header`, `CONFIG_MAIN_STACK_SIZE=65536`), and a
tamper test against it is correctly rejected. Image size is not an
independent factor once `--pad-header` is removed and the stack is sized
per §5 below.

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

## 5. ML-DSA stack size: measured across all three parameter sets (44/65/87)

Once the `--pad-header` bug above was fixed, the ML-DSA-44 hybrid build
still crashed — this time genuinely mid-verify, not at handoff. Debug
logging (`CONFIG_MCUBOOT_LOG_LEVEL_DBG=y`) pinpointed it: the log cut off
mid-line right after `bootutil_verify_sig_mldsa: ML-DSA-44 key_id 1`, then
the board rebooted into MCUboot's banner again — a stack overflow during
the actual ML-DSA verify call, not a signature mismatch (which would have
printed a clean rejection message and kept running).

The Kconfig help text for `BOOT_SIGNATURE_MLDSA_KEY_FILE` estimated
"roughly 9-13KB" of extra stack for this. That estimate was wrong by a
large margin in practice:

| `CONFIG_MAIN_STACK_SIZE` | ML-DSA-44 | ML-DSA-65 | ML-DSA-87 |
|---|---|---|---|
| 10240 (Zephyr default) | crashes | not tested (already known bad) | not tested |
| 32768 | crashes mid-verify | crashes mid-verify | not tested (already known bad) |
| 65536 | **works** | **works** | **works** |

All three parameter sets were tested the same way: build the hybrid
bootloader (`overlay-mldsa44-hybrid-ecdsa.conf` as a base, with
`CONFIG_BOOT_SIGNATURE_TYPE_MLDSA_{44,65,87}` and
`CONFIG_BOOT_SIGNATURE_MLDSA_KEY_FILE` overridden per level), sign
`hello_world` with the matching `root-mldsa{44,65,87}.pem` key plus
`root-ec-p256.pem`, flash, and watch the debug log either cut off
mid-verify (crash) or reach `Left boot_go with success == 1` followed by
`Hello World!` on the console (success). A tamper test (flip one byte in
the ML-DSA signature TLV) was also re-run at each level that succeeded, to
confirm rejection still works.

This was **not** a search for each level's exact minimum stack — just two
sample sizes (32768, 65536) to see whether the requirement scales enough
with parameter set to need a different overlay per level. It doesn't, at
least not within this range: 65536 covers all three on this exact
toolchain/board/application. Ready-made overlays exist for all three at
this stack size:
`samples/zephyr/overlay-mldsa{44,65,87}-{hybrid-ecdsa,pqconly}.conf`.

**This number is specific to this toolchain, optimization level, board,
and application** (a single `printf` in `hello_world`) — it is not a
portable constant. A different compiler, a `-O0` build, or a call chain
that itself uses more stack before reaching the verify call could all
require more. Re-verify empirically (the debug-log-cutoff method above,
or the GDB `bootutil_verify_sig_mldsa` breakpoint from the commands
reference in `readme-zephyr.md`) rather than assuming 65536 is universal.
