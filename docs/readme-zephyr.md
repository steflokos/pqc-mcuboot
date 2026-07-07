# Building and using MCUboot with Zephyr

MCUboot began its life as the bootloader for Mynewt.  It has since
acquired the ability to be used as a bootloader for Zephyr as well.
There are some pretty significant differences in how apps are built
for Zephyr, and these are documented here.

Please see the [design document](design.md) for documentation on the design
and operation of the bootloader itself. This functionality should be the same
on all supported RTOSs.

The first step required for Zephyr is making sure your board has flash
partitions defined in its device tree. These partitions are:

- `boot_partition`: for MCUboot itself
- `slot0_partition`: the primary slot of Image 0
- `slot1_partition`: the secondary slot of Image 0

It is not recommended to use the swap-using-scratch algorithm of MCUboot, but
if this operating mode is desired then the following flash partition is also
needed (see end of this help file for details on creating a scratch partition
and how to use the swap-using-scratch algorithm):

- `scratch_partition`: the scratch slot

Currently, the two image slots must be contiguous. If you are running
MCUboot as your stage 1 bootloader, `boot_partition` must be configured
so your SoC runs it out of reset. If there are multiple updateable images
then the corresponding primary and secondary partitions must be defined for
the rest of the images too (for example, `slot2_partition` and
`slot3_partition` for Image 1).

The flash partitions are typically defined in the Zephyr boards folder, in a
file named `boards/<arch>/<board>/<board>.dts`. An example `.dts` file with
flash partitions defined is the frdm_k64f's in
`boards/nxp/frdm_k64f/frdm_k64f.dts`. Make sure the DT node labels in your board's
`.dts` file match the ones used there.

## Installing requirements and dependencies

Install additional packages required for development with MCUboot:

```
  cd ~/mcuboot  # or to your directory where MCUboot is cloned
  pip3 install --user -r scripts/requirements.txt
```

## Building the bootloader itself

The bootloader is an ordinary Zephyr application, at least from
Zephyr's point of view.  There is a bit of configuration that needs to
be made before building it.  Most of this can be done as documented in
the `CMakeLists.txt` file in boot/zephyr.  There are comments there for
guidance.  It is important to select a signature algorithm, and decide
if the primary slot should be validated on every boot.

To build MCUboot, create a build directory in boot/zephyr, and build
it as usual:

```
  cd boot/zephyr
  west build -b <board>
```

In addition to the partitions defined in DTS, some additional
information about the flash layout is currently required to build
MCUboot itself. All the needed configuration is collected in
`boot/zephyr/include/target.h`. Depending on the board, this information
may come from board-specific headers, Device Tree, or be configured by
MCUboot on a per-SoC family basis.

After building the bootloader, the binaries should reside in
`build/zephyr/zephyr.{bin,hex,elf}`, where `build` is the build
directory you chose when running `west build`. Use `west flash`
to flash these binaries from the build directory. Depending
on the target and flash tool used, this might erase the whole of the flash
memory (mass erase) or only the sectors where the bootloader resides prior to
programming the bootloader image itself.

## Building applications for the bootloader

In addition to flash partitions in DTS, some additional configuration
is required to build applications for MCUboot.

This is handled internally by the Zephyr configuration system and is wrapped
in the `CONFIG_BOOTLOADER_MCUBOOT` Kconfig variable, which must be enabled in
the application's `prj.conf` file.

The directory `samples/zephyr/hello-world` in the MCUboot tree contains
a simple application with everything you need. You can try it on your
board and then just make a copy of it to get started on your own
application; see samples/zephyr/README.md for a tutorial.

The Zephyr `CONFIG_BOOTLOADER_MCUBOOT` configuration option
[documentation](https://docs.zephyrproject.org/latest/kconfig.html#CONFIG_BOOTLOADER_MCUBOOT)
provides additional details regarding the changes it makes to the image
placement and generation in order for an application to be bootable by MCUboot.

With this, build the application as your normally would.

### Signing the application

In order to upgrade to an image (or even boot it, if
`MCUBOOT_VALIDATE_PRIMARY_SLOT` is enabled), the images must be signed.
To make development easier, MCUboot is distributed with some example
keys.  It is important to stress that these should never be used for
production, since the private key is publicly available in this
repository.  See below on how to make your own signatures.

Images can be signed with the `scripts/imgtool.py` script.  It is best
to look at `samples/zephyr/Makefile` for examples on how to use this.

### Flashing the application

The application itself can flashed with regular flash tools, but will
need to be programmed at the offset of the primary slot for this particular
target. Depending on the platform and flash tool you might need to manually
specify a flash offset corresponding to the primary slot starting address. This
is usually not relevant for flash tools that use Intel Hex images (.hex) instead
of raw binary images (.bin) since the former include destination address
information. Additionally you will need to make sure that the flash tool does
not perform a mass erase (erasing the whole of the flash) or else you would be
deleting MCUboot.
These images can also be marked for upgrade, and loaded into the secondary slot,
at which point the bootloader should perform an upgrade.  It is up to
the image to mark the primary slot as "image ok" before the next reboot,
otherwise the bootloader will revert the application.

## Managing signing keys

The signing keys used by MCUboot are represented in standard formats,
and can be generated and processed using conventional tools.  However,
`scripts/imgtool.py` is able to generate key pairs in all of the
supported formats.  See [the docs](imgtool.md) for more details on
this tool.

### Generating a new keypair

Generating a keypair with imgtool is a matter of running the keygen
subcommand:

```
    $ ./scripts/imgtool.py keygen -k mykey.pem -t rsa-2048
```

The argument to `-t` should be the desired key type.  See the
[the docs](imgtool.md) for more details on the possible key types.

### Extracting the public key

The generated keypair above contains both the public and the private
key.  It is necessary to extract the public key and insert it into the
bootloader.  Use the ``CONFIG_BOOT_SIGNATURE_KEY_FILE`` Kconfig option to
provide the path to the key file so the build system can extract
the public key in a format usable by the C compiler.
The generated public key is saved in `build/zephyr/autogen-pubkey.h`, which is included
by the `boot/zephyr/keys.c`.

Currently, the Zephyr RTOS port limits its support to one classical keypair at
a time, although MCUboot's key management infrastructure supports multiple
keypairs. The exception is ML-DSA (see below), which can be layered on top of
a classical keypair for hybrid post-quantum signing.

Once MCUboot is built, this new keypair file (`mykey.pem` in this
example) can be used to sign images.

### ML-DSA (post-quantum) and hybrid signing

In addition to the classical signature types above, MCUboot can verify
ML-DSA (FIPS 204) post-quantum signatures, using the `mldsa-native`
reference implementation vendored as the `ext/mldsa-native` submodule
(run `git submodule update --init ext/mldsa-native` if it is empty).

Enable `CONFIG_BOOT_SIGNATURE_TYPE_MLDSA_ENABLE` and pick a parameter set
(`CONFIG_BOOT_SIGNATURE_TYPE_MLDSA_44/65/87`, ML-DSA-44 being the smallest
and fastest, ML-DSA-87 the largest and slowest). By default this runs in
**hybrid mode**: both the classical signature selected via
`BOOT_SIGNATURE_TYPE` (RSA/ECDSA/ED25519) and the ML-DSA signature must
independently verify for boot to succeed. Set
`CONFIG_BOOT_SIGNATURE_TYPE_MLDSA_PQC_ONLY=y` together with
`CONFIG_BOOT_SIGNATURE_TYPE_NONE=y` to use ML-DSA alone instead.

Generate an ML-DSA keypair the same way as any other key type:

```
    $ ./scripts/imgtool.py keygen -k my-mldsa-key.pem -t mldsa44
```

and point `CONFIG_BOOT_SIGNATURE_MLDSA_KEY_FILE` at it (mirrors
`CONFIG_BOOT_SIGNATURE_KEY_FILE` for the classical key). When signing an
image for hybrid mode, pass both keys:

```
    $ ./scripts/imgtool.py sign ... -k my-classical-key.pem --pqc-key my-mldsa-key.pem app.bin signed-app.bin
```

For PQC-only mode, pass only the ML-DSA key with `-k`.

ML-DSA signature verification uses considerably more stack than the
classical algorithms and much more than the roughly 9-13KB once assumed
here. Measured directly on real hardware
(`arduino_uno_q/stm32u585xx`, GNU Arm toolchain, reduced-RAM mode):
`CONFIG_MAIN_STACK_SIZE=32768` reliably crashes mid-verify (stack overflow)
for **all three** parameter sets (44/65/87); `65536` works for all three.
This was a "does it work" check at two sizes, not a per-level minimum
search -- treat 65536 as a known-good starting point for this
toolchain/board, not a hard requirement, and re-verify empirically for any
other toolchain, optimization level, or board (see
`samples/zephyr/overlay-mldsa{44,65,87}-{hybrid-ecdsa,pqconly}.conf` for
ready-made overlays, and
[arduino-uno-q-hardware-notes.md](arduino-uno-q-hardware-notes.md) for the
debug-log-based methodology used to find this). The embedded keys/signatures
are also much larger than classical ones (1312-2592 byte public keys,
2420-4627 byte signatures, depending on parameter set) -- ensure the
target's flash budget is sized accordingly too.

## Testing on real hardware (Arduino UNO Q)

This section is a fully reproducible, copy-pasteable walkthrough for
building and testing the ML-DSA/hybrid signature verification above on the
Arduino UNO Q's STM32U585 (Cortex-M33) MCU side. It assumes a Debian/Ubuntu
environment (e.g. WSL) with no prior Zephyr setup.

### One-time environment setup

Everything is scoped to a single `~/zephyr-pqc` folder, so removal later is
just `rm -rf ~/zephyr-pqc`.

The manual steps below (`west init` against upstream Zephyr, then swapping
this fork in for `bootloader/mcuboot`) can also be replaced with a single
`west init -m https://github.com/steflokos/pqc-zephyr-manifest --mr main .`
-- see [pqc-zephyr-manifest](https://github.com/steflokos/pqc-zephyr-manifest).
That same workspace also picks up
[pqc-zephyr-crypto](https://github.com/steflokos/pqc-zephyr-crypto), a
companion module exposing ML-KEM/ML-DSA to Zephyr *applications* themselves
(independent of the bootloader-side verification documented in this file).

```bash
# System build dependencies
sudo apt update && sudo apt install -y --no-install-recommends git cmake ninja-build \
    gperf ccache dfu-util device-tree-compiler wget python3-dev python3-pip \
    python3-setuptools python3-tk python3-wheel xz-utils file make gcc \
    libsdl2-dev libmagic1

# Isolated Python venv + west
mkdir -p ~/zephyr-pqc && cd ~/zephyr-pqc
python3 -m venv .venv
source .venv/bin/activate
pip install west

# West workspace from upstream Zephyr (>= v4.3.0 for arduino_uno_q board support;
# v4.4.1 is the latest stable release at the time of writing)
west init -m https://github.com/zephyrproject-rtos/zephyr --mr v4.4.1 .
west update

# Use requirements-base.txt, NOT the umbrella requirements.txt: the full
# requirements.txt also pulls in requirements-compliance.txt (CI lint
# tooling, irrelevant here), which needs hidapi/ruamel.yaml.clib -- packages
# that may not have prebuilt wheels for very new Python versions and fail
# to build from source. requirements-base.txt alone already has everything
# actually needed to build/flash (including west, requests, patool).
pip install -r zephyr/scripts/requirements-base.txt

# Swap in this PQC fork in place of upstream mcuboot
rm -rf bootloader/mcuboot
git clone https://github.com/steflokos/pqc-mcuboot.git bootloader/mcuboot
cd bootloader/mcuboot
git submodule update --init ext/mldsa-native
pip install -r scripts/requirements.txt
cd ~/zephyr-pqc

# Zephyr SDK, self-contained inside the same folder
west sdk install --install-dir ~/zephyr-pqc/zephyr-sdk
```

If `west sdk install` fails for any reason, install the SDK manually instead
(no Python involved; check
[sdk-ng releases](https://github.com/zephyrproject-rtos/sdk-ng/releases)
for the current version if `v1.0.1` below is no longer latest):

```bash
wget https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v1.0.1/zephyr-sdk-1.0.1_linux-x86_64_gnu.tar.xz
tar xf zephyr-sdk-1.0.1_linux-x86_64_gnu.tar.xz
cd zephyr-sdk-1.0.1 && ./setup.sh && cd ..
```

### Build and flash the bootloader

Start with **PQC-only ML-DSA-44** (simplest case to reason about). The
`overlay-mldsa44-pqconly.conf` file in `samples/zephyr/` in this repo
carries the exact Kconfig settings, matching the existing
`overlay-ecdsa-p256.conf`/`overlay-rsa.conf` convention:

```bash
cd ~/zephyr-pqc/bootloader/mcuboot/boot/zephyr
west build -b arduino_uno_q -- \
  -DEXTRA_CONF_FILE=$PWD/../../samples/zephyr/overlay-mldsa44-pqconly.conf
```

Confirm the build actually worked and ML-DSA got compiled in (this is the
first-ever compile of `image_mldsa.c` and the mldsa-native amalgamation for
Cortex-M33):

```bash
ls -la build/zephyr/zephyr.{elf,bin,hex}          # exist, non-zero size
grep MLDSA build/zephyr/.config                    # options took effect
find build -name "image_mldsa.c.obj" -o -name "mldsa_native.c.obj"
$ZEPHYR_SDK_INSTALL_DIR/arm-zephyr-eabi/bin/arm-zephyr-eabi-nm build/zephyr/zephyr.elf | grep -i mldsa
```

**`west flash` does not work on this board** — it has no externally-reachable
SWD debug probe. See
[arduino-uno-q-hardware-notes.md](arduino-uno-q-hardware-notes.md#1-west-flash-does-not-work-on-this-board)
for the actual flashing procedure (installing `arduino-cli` and calling its
bundled `remoteocd` tool directly), and for known issues (console wiring,
Zephyr version compatibility fixes, and an unresolved ML-DSA image-size bug)
found while getting this running on real hardware.

### Build, sign, and boot a test application

MCUboot's own `samples/zephyr/hello-world` was removed upstream some time
ago; use Zephyr's own `samples/hello_world` instead, with the
`hello-mcuboot.conf` overlay in this repo (just sets
`CONFIG_BOOTLOADER_MCUBOOT=y`):

```bash
cd ~/zephyr-pqc
west build -b arduino_uno_q -d build-hello zephyr/samples/hello_world -- \
  -DEXTRA_CONF_FILE=$PWD/bootloader/mcuboot/samples/zephyr/hello-mcuboot.conf

cd bootloader/mcuboot
python3 scripts/imgtool.py sign \
  --header-size 0x400 --align 4 \
  --slot-size <slot0_partition_size_from_board_dts> \
  --version 1.0.0 \
  -k root-mldsa44.pem \
  ../../build-hello/zephyr/zephyr.bin ../../build-hello/zephyr/signed-hello.bin
```

(Find `<slot0_partition_size_from_board_dts>` from the `arduino_uno_q` board's
devicetree partition layout in the Zephyr checkout, or read it back out of
`build-hello/zephyr/zephyr.dts` after building — look for the `slot0_partition`
node's `reg` size. `--header-size` must match the app build's own
`CONFIG_ROM_START_OFFSET` (`grep CONFIG_ROM_START_OFFSET build-hello/zephyr/.config`
-- currently `0x400` for this board/config; a mismatch here makes MCUboot
jump to the wrong offset and hard-fault immediately.)

**Do not add `--pad-header` here.** With `CONFIG_ROM_START_OFFSET` set (as it
is for this board), Zephyr's own linker script already reserves
`--header-size` bytes of leading zero padding inside `zephyr.bin` itself for
the header to overwrite. `--pad-header` tells imgtool the *opposite* — that
the input has *no* reserved header space and one must be prepended — which
inserts a second, redundant header-sized gap in front of the real one. The
resulting file still has a syntactically valid header (so MCUboot happily
parses it and decides to boot), but the vector table ends up
`--header-size` bytes further into the file than MCUboot's jump target
`partition_base + header_size` expects. MCUboot jumps into what is actually
still-reserved zero padding and hard-faults immediately — this reproduces
identically on real hardware regardless of image size or signature type,
and was the actual cause of every "MCUboot jumps then nothing happens" crash
we hit while bringing this board up, not a size- or PQC-specific bug (see
[arduino-uno-q-hardware-notes.md §4](arduino-uno-q-hardware-notes.md#4-known-bug-ml-dsa-bootloader-verification-crashes-above-a-size-threshold)
for how this was finally root-caused).

Flash `signed-hello.bin` to the primary slot address — see
[arduino-uno-q-hardware-notes.md](arduino-uno-q-hardware-notes.md#1-west-flash-does-not-work-on-this-board)
for the actual procedure on this board (`west flash` does not work here at
all, for either the bootloader or the app). Then open the board's serial
console (see
[arduino-uno-q-hardware-notes.md](arduino-uno-q-hardware-notes.md#2-seeing-console-output-requires-external-wiring--and-a-specific-tool)
for wiring) and confirm you see Zephyr's normal `hello_world` output. **This
is the sign of a successful boot.**

**Previously-reported issue, now root-caused:** what looked like an ML-DSA-
specific "images above roughly 40KB crash the bootloader" bug was actually
the `--pad-header` bug described above — see
[arduino-uno-q-hardware-notes.md §4](arduino-uno-q-hardware-notes.md#4-known-bug-ml-dsa-bootloader-verification-crashes-above-a-size-threshold)
for the full account, including why it only *appeared* size-correlated.
Re-verify with `--pad-header` removed before assuming any remaining crash is
new.

### Tamper test (proves verification is actually enforced)

Flip one byte inside the `MLDSA44` signature TLV of `signed-hello.bin` (the
last ~2420 bytes of the TLV area, found after the `KEYHASH` TLV that
precedes it — or just corrupt the very last byte of the file, which falls
inside the signature for a minimal image) and reflash. Confirm the board no
longer boots the app (bootloader should log a validation failure to the
serial console and refuse to jump to it).

### Hybrid mode

Rebuild the bootloader with `overlay-mldsa44-hybrid-ecdsa.conf` instead
(requires both `root-ec-p256.pem` and `root-mldsa44.pem`):

```bash
cd ~/zephyr-pqc/bootloader/mcuboot/boot/zephyr
west build -p always -b arduino_uno_q -- \
  -DEXTRA_CONF_FILE=$PWD/../../samples/zephyr/overlay-mldsa44-hybrid-ecdsa.conf
```
Flash per [arduino-uno-q-hardware-notes.md](arduino-uno-q-hardware-notes.md#1-west-flash-does-not-work-on-this-board)
(`west flash` does not work on this board).

Sign the test app with both keys and repeat the tamper test against each
signature independently (this is the proof that both must pass -- not
either/or):

```bash
python3 scripts/imgtool.py sign --header-size 0x400 --align 4 \
  --slot-size <slot0_partition_size> --version 1.0.0 \
  -k root-ec-p256.pem --pqc-key root-mldsa44.pem \
  ../../build-hello/zephyr/zephyr.bin ../../build-hello/zephyr/signed-hello-hybrid.bin
```
(`--header-size` must still match this build's own `CONFIG_ROM_START_OFFSET`
-- do not assume `0x200` without checking; see the note above about why
`--pad-header` must not be used here either.)

Run the 4-way matrix: (valid classical, valid ML-DSA) boots; corrupting
*either one alone* must reject the image; corrupting both must also reject.

### Recovery

The Arduino UNO Q ships with a recovery path if the STM32 image gets
overwritten by something broken:

```bash
adb shell arduino-cli burn-bootloader -b arduino:zephyr:unoq -P jlink
```

This restores the factory sketch-loader, so experimenting here carries no
real risk of bricking the board.

## Reproducible commands: build, sign, flash, and debug all three modes

This section is a copy-pasteable command reference for the three secure-boot
configurations exercised on the Arduino UNO Q (`arduino_uno_q/stm32u585xx`):
**plain ECDSA-P256**, **hybrid ECDSA-P256 + ML-DSA-44**, and **PQC-only
ML-DSA-44**. All three were rebuilt from scratch and independently verified
(boot + tamper test) on real hardware; see
[arduino-uno-q-hardware-notes.md](arduino-uno-q-hardware-notes.md) for the
root-caused bugs found along the way (`--pad-header`, insufficient
`CONFIG_MAIN_STACK_SIZE`, the `BOOT_INTR_VEC_RELOC`/`BOOT_DISABLE_CACHES`
Kconfig gaps) — all fixes are already reflected in the commands below.

Run everything from the west workspace root (`~/zephyr-pqc` per the setup
section above), with the venv activated and `ZEPHYR_SDK_INSTALL_DIR` set.

### Shared setup (once per session)

```bash
source .venv/bin/activate
export ZEPHYR_SDK_INSTALL_DIR=$PWD/zephyr-sdk

# Flashing bridge (this board has no external SWD probe; see
# arduino-uno-q-hardware-notes.md #1 for why this is necessary)
REMOTEOCD=$(ls ~/Library/Arduino15/packages/arduino/tools/remoteocd/*/remoteocd | head -1)
ADB=$(ls ~/Library/Arduino15/packages/arduino/tools/adb/*/adb | head -1)
BOOTCFG=$(ls -d ~/Library/Arduino15/packages/arduino/hardware/zephyr/*/variants/arduino_uno_q_stm32u585xx | head -1)/flash_bootloader.cfg
SERIAL=$($ADB devices | awk 'NR==2{print $1}')

# Custom OpenOCD recipe for flashing a raw signed app binary to slot0
# (0x08010000, size 0x68000 -- read back from <build>/zephyr/zephyr.dts if
# your partition layout differs)
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

# Serial console (see arduino-uno-q-hardware-notes.md #2 for wiring --
# an external USB-TTL adapter on D0/D1, separate from the adb/USB link)
screen -L -dmS pqcconsole /dev/cu.usbmodemXXXX 115200   # find the right /dev/cu.usbmodem* first
```

**Tamper-test caveat:** reflashing a file that differs from what's already in
flash by only a few bytes can silently leave the old content in place (no
error printed) -- always flash a genuinely different image (or the valid
image) immediately before flashing a tampered variant. See
[arduino-uno-q-hardware-notes.md §4](arduino-uno-q-hardware-notes.md#4-root-caused---pad-header-corrupts-the-vector-table-offset-not-a-size-or-pqc-bug).

### Stage 0: plain ECDSA-P256 (baseline)

```bash
# Build the bootloader
west build -b arduino_uno_q -d build-boot-ecdsa bootloader/mcuboot/boot/zephyr -- \
  -DEXTRA_CONF_FILE=$PWD/bootloader/mcuboot/samples/zephyr/overlay-ecdsa-p256.conf \
  -DCONFIG_BOOT_INTR_VEC_RELOC=y

# Build the test app
west build -b arduino_uno_q -d build-hello-ecdsa zephyr/samples/hello_world -- \
  -DEXTRA_CONF_FILE=$PWD/bootloader/mcuboot/samples/zephyr/hello-mcuboot.conf

# Sign (no --pad-header -- see the caveat earlier in this doc)
cd bootloader/mcuboot
python3 scripts/imgtool.py sign --header-size 0x400 --align 4 \
  --slot-size 0x68000 --version 1.0.0 \
  -k root-ec-p256.pem \
  ../../build-hello-ecdsa/zephyr/zephyr.bin ../../build-hello-ecdsa/zephyr/signed-hello-ecdsa.bin
cd ../..

# Flash bootloader, then app
"$REMOTEOCD" upload --adb-path "$ADB" -s "$SERIAL" -f "$BOOTCFG" build-boot-ecdsa/zephyr/zephyr.elf
"$REMOTEOCD" upload --adb-path "$ADB" -s "$SERIAL" -f /tmp/flash_app.cfg build-hello-ecdsa/zephyr/signed-hello-ecdsa.bin

# Confirm: console should show MCUboot's banner, "Jumping to the first image
# slot", then "Hello World! arduino_uno_q/stm32u585xx"
cat screenlog.0
```

### Stage 1: hybrid ECDSA-P256 + ML-DSA-44

```bash
# Build the bootloader (CONFIG_MAIN_STACK_SIZE=65536 comes from the overlay)
west build -b arduino_uno_q -d build-boot-hybrid bootloader/mcuboot/boot/zephyr -- \
  -DEXTRA_CONF_FILE=$PWD/bootloader/mcuboot/samples/zephyr/overlay-mldsa44-hybrid-ecdsa.conf \
  -DCONFIG_BOOT_INTR_VEC_RELOC=y

# Build the test app (same app works for every mode)
west build -b arduino_uno_q -d build-hello-hybrid zephyr/samples/hello_world -- \
  -DEXTRA_CONF_FILE=$PWD/bootloader/mcuboot/samples/zephyr/hello-mcuboot.conf

# Sign with both keys
cd bootloader/mcuboot
python3 scripts/imgtool.py sign --header-size 0x400 --align 4 \
  --slot-size 0x68000 --version 1.0.0 \
  -k root-ec-p256.pem --pqc-key root-mldsa44.pem \
  ../../build-hello-hybrid/zephyr/zephyr.bin ../../build-hello-hybrid/zephyr/signed-hello-hybrid.bin
cd ../..

# Flash bootloader, then app
"$REMOTEOCD" upload --adb-path "$ADB" -s "$SERIAL" -f "$BOOTCFG" build-boot-hybrid/zephyr/zephyr.elf
"$REMOTEOCD" upload --adb-path "$ADB" -s "$SERIAL" -f /tmp/flash_app.cfg build-hello-hybrid/zephyr/signed-hello-hybrid.bin
cat screenlog.0   # expect "Hello World! ..." after MCUboot's log

# Tamper test: dump the TLV layout to find each signature's byte range,
# flip one byte in it, reflash, and confirm rejection -- repeat once for
# ECDSA (TLV type 0x22) and once for ML-DSA (TLV type 0x26)
python3 bootloader/mcuboot/scripts/imgtool.py dumpinfo build-hello-hybrid/zephyr/signed-hello-hybrid.bin
python3 - << 'EOF'
with open('build-hello-hybrid/zephyr/signed-hello-hybrid.bin', 'rb') as f:
    data = bytearray(f.read())
data[<offset_inside_one_signature>] ^= 0xFF
with open('/tmp/tampered.bin', 'wb') as f:
    f.write(data)
EOF
"$REMOTEOCD" upload --adb-path "$ADB" -s "$SERIAL" -f /tmp/flash_app.cfg /tmp/tampered.bin
cat screenlog.0   # expect "E: Image in the primary slot is not valid!"
```

### Stage 2: PQC-only ML-DSA-44

```bash
# Build the bootloader
west build -b arduino_uno_q -d build-boot-pqconly bootloader/mcuboot/boot/zephyr -- \
  -DEXTRA_CONF_FILE=$PWD/bootloader/mcuboot/samples/zephyr/overlay-mldsa44-pqconly.conf \
  -DCONFIG_BOOT_INTR_VEC_RELOC=y

# Build the test app
west build -b arduino_uno_q -d build-hello-pqconly zephyr/samples/hello_world -- \
  -DEXTRA_CONF_FILE=$PWD/bootloader/mcuboot/samples/zephyr/hello-mcuboot.conf

# Sign with only the ML-DSA key (no classical key, no --pqc-key flag)
cd bootloader/mcuboot
python3 scripts/imgtool.py sign --header-size 0x400 --align 4 \
  --slot-size 0x68000 --version 1.0.0 \
  -k root-mldsa44.pem \
  ../../build-hello-pqconly/zephyr/zephyr.bin ../../build-hello-pqconly/zephyr/signed-hello-pqconly.bin
cd ../..

# Flash bootloader, then app
"$REMOTEOCD" upload --adb-path "$ADB" -s "$SERIAL" -f "$BOOTCFG" build-boot-pqconly/zephyr/zephyr.elf
"$REMOTEOCD" upload --adb-path "$ADB" -s "$SERIAL" -f /tmp/flash_app.cfg build-hello-pqconly/zephyr/signed-hello-pqconly.bin
cat screenlog.0   # expect "Hello World! ..." after MCUboot's log

# Tamper test (flip a byte inside the MLDSA44 TLV, reflash a genuinely
# different image first per the caveat above, then reflash the tampered one)
cat screenlog.0   # expect "E: Image in the primary slot is not valid!"
```

### ML-DSA-65 and ML-DSA-87 (same recipe, different level)

Ready-made overlays exist for all three levels:
`overlay-mldsa{44,65,87}-{hybrid-ecdsa,pqconly}.conf`. Swap the overlay file
and the signing key (`root-mldsa65.pem` / `root-mldsa87.pem`) into any of the
three stage recipes above -- everything else (flashing, console check,
tamper test) is identical. Example for hybrid mode at ML-DSA-65:

```bash
west build -b arduino_uno_q -d build-boot-mldsa65 bootloader/mcuboot/boot/zephyr -- \
  -DEXTRA_CONF_FILE=$PWD/bootloader/mcuboot/samples/zephyr/overlay-mldsa65-hybrid-ecdsa.conf \
  -DCONFIG_BOOT_INTR_VEC_RELOC=y

cd bootloader/mcuboot
python3 scripts/imgtool.py sign --header-size 0x400 --align 4 \
  --slot-size 0x68000 --version 1.0.0 \
  -k root-ec-p256.pem --pqc-key root-mldsa65.pem \
  ../../build-hello-hybrid/zephyr/zephyr.bin ../../build-hello-hybrid/zephyr/signed-hello-mldsa65.bin
cd ../..
```

`CONFIG_MAIN_STACK_SIZE=65536` (already set in these overlays) was verified
sufficient for all three parameter sets on this board/toolchain -- see
[arduino-uno-q-hardware-notes.md §5](arduino-uno-q-hardware-notes.md#5-ml-dsa-stack-size-measured-across-all-three-parameter-sets-446587)
for how that was determined and why it isn't a portable constant.

### Debugging with GDB (no external probe needed)

The Qualcomm side of the board (QRB2210) already exposes a CMSIS-DAP-style
debug bridge over `adb` -- no SWD wiring required:

```bash
# One-time per session: start the on-board debug server and forward its port
adb forward tcp:3333 tcp:3333
adb shell arduino-debug &

# Attach GDB (from the Zephyr SDK), using whichever bootloader ELF you're
# currently debugging for symbols
arm-zephyr-eabi-gdb -q build-boot-hybrid/zephyr/zephyr.elf
(gdb) target extended-remote localhost:3333
(gdb) monitor reset halt
(gdb) break do_boot          # or bootutil_verify_sig_mldsa, image_validate.c lines, etc.
(gdb) continue
(gdb) print/x $pc
(gdb) x/8xw 0x08010400        # inspect the app's vector table in flash directly
```

If a later `adb shell arduino-debug` fails with "Error requesting gpio line
swdio", a previous OpenOCD instance is still holding the debug GPIO --
`adb shell "pkill -9 openocd"` and restart it. Each new GDB connection also
needs the previous one to have exited cleanly (add `quit` at the end of any
scripted GDB session), or the board's OpenOCD rejects new connections with
"no more connections allowed".

## Using swap-using-scratch flash algorithm

To use the swap-using-scratch flash algorithm, a scratch partition needs to be
present for the target board which is used for holding the data being swapped
from both slots, this section must be at least as big as the largest sector
size of the 2 partitions (e.g. if a device has a primary slot in main flash
with a sector size of 512 bytes and secondar slot in external off-chip flash
with a sector size of 4KB then the scratch area must be at least 4KB in size).
The number of sectors must also be evenly divisable by this sector size, e.g.
4KB, 8KB, 12KB, 16KB are allowed, 7KB, 7.5KB are not. This scratch partition
needs adding to the .dts file for the board, e.g. for the nrf52dk_nrf52832
board thus would involve updating
`<zephyr>/boards/nordic/nrf52dk/nrf52dk_nrf52832.dts` with:

```
    boot_partition: partition@0 {
        label = "mcuboot";
        reg = <0x00000000 0xc000>;
    };
    slot0_partition: partition@c000 {
        label = "image-0";
        reg = <0x0000C000 0x37000>;
    };
    slot1_partition: partition@43000 {
        label = "image-1";
        reg = <0x00043000 0x37000>;
    };
    scratch_partition: partition@7a000 {
        label = "image-scratch";
        reg = <0x0007a000 0x00006000>;
    };
```

Which would make the application size 220KB and scratch size 24KB (the nRF52832
has a 4KB sector size so the size of the scratch partition can be reduced at
the cost of vastly reducing flash lifespan, e.g. for a 32KB firmware update
with an 8KB scratch area, the scratch area would be erased and programmed 8
times per image upgrade/revert). To configure MCUboot to work in
swap-using-scratch mode, the Kconfig value must be set when building it:
`CONFIG_BOOT_SWAP_USING_SCRATCH=y`.

Note that it is possible for an application to get into a stuck state when
swap-using-scratch is used whereby an application has loaded a firmware update
and marked it as test/confirmed but MCUboot will not swap the images and
erasing the secondary slot from the zephyr application returns an error
because the slot is marked for upgrade.

## Serial recovery

### Interface selection

A serial recovery protocol is available over either a hardware serial port or a USB CDC ACM virtual serial port.
The SMP server implementation can be enabled by the ``CONFIG_MCUBOOT_SERIAL=y`` Kconfig option.
To set a type of an interface, use the ``BOOT_SERIAL_DEVICE`` Kconfig choice, and select either the ``CONFIG_BOOT_SERIAL_UART`` or the ``CONFIG_BOOT_SERIAL_CDC_ACM`` value.
Which interface belongs to the protocol shall be set by the devicetree-chosen node:
- `zephyr,console` - If a hardware serial port is used.
- `zephyr,cdc-acm-uart` - If a virtual serial port is used.

### Entering the serial recovery mode

To enter the serial recovery mode, the device has to initiate rebooting, and a triggering event has to occur (for example, pressing a button).

By default, the serial recovery GPIO pin active state enters the serial recovery mode.
Use the ``mcuboot_button0`` devicetree button alias to assign the GPIO pin to the MCUboot.

Alternatively, MCUboot can wait for a limited time to check if DFU is invoked by receiving an MCUmgr command.
Select ``CONFIG_BOOT_SERIAL_WAIT_FOR_DFU=y`` to use this mode. ``CONFIG_BOOT_SERIAL_WAIT_FOR_DFU_TIMEOUT`` option defines
the amount of time in milliseconds the device will wait for the trigger.

### Direct image upload

By default, the SMP server implementation will only use the first slot.
To change it, invoke the `image upload` MCUmgr command with a selected image number, and make sure the ``CONFIG_MCUBOOT_SERIAL_DIRECT_IMAGE_UPLOAD=y`` Kconfig option is enabled.
Note that the ``CONFIG_UPDATEABLE_IMAGE_NUMBER`` Kconfig option adjusts the number of image-pairs supported by the MCUboot.

The mapping of image number to partition is as follows:
* 0 and 1 - image-0, the primary slot of the first image.
* 2 - image-1, the secondary slot of the first image.
* 3 - image-2.
* 4 - image-3.

0 is a default upload target when no explicit selection is done.

### System-specific commands

Use the ``CONFIG_ENABLE_MGMT_PERUSER=y`` Kconfig option to enable the following additional commands:
* Storage erase - This command allows erasing the storage partition (enable with ``CONFIG_BOOT_MGMT_CUSTOM_STORAGE_ERASE=y``).

### More configuration

For details on other available configuration options for the serial recovery protocol, check the Kconfig options  (for example by using ``menuconfig``).
