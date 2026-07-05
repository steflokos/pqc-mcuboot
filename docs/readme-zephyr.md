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
classical algorithms (roughly 9-13KB for the reduced-RAM configuration
MCUboot defaults to, more without it), and the embedded keys/signatures
are much larger (1312-2592 byte public keys, 2420-4627 byte signatures,
depending on parameter set). Ensure the bootloader's stack size
(`CONFIG_MAIN_STACK_SIZE` or equivalent) and the target's flash budget are
sized accordingly, and validate empirically on real hardware -- do not
assume the defaults are sufficient for every board/toolchain combination.

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

# West workspace from upstream Zephyr (>= v4.3.0 for arduino_uno_q board support)
west init -m https://github.com/zephyrproject-rtos/zephyr --mr v4.3.0 .
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

Flash it (board connected over USB, `adb` reachable):

```bash
west flash
```

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
  --header-size 0x200 --pad-header --align 4 \
  --slot-size <slot0_partition_size_from_board_dts> \
  --version 1.0.0 \
  -k root-mldsa44.pem \
  ../../build-hello/zephyr/zephyr.bin ../../build-hello/zephyr/signed-hello.bin
```

(Find `<slot0_partition_size_from_board_dts>` from the `arduino_uno_q` board's
devicetree partition layout in the Zephyr checkout, or read it back out of
`build-hello/zephyr/zephyr.dts` after building — look for the `slot0_partition`
node's `reg` size.)

Flash `signed-hello.bin` to the primary slot address (not `west flash`,
which targets the *bootloader* build directory — use your flash tool's
address-specific write, e.g. `west flash --skip-rebuild` from a build
configured against `build-hello`, or `STM32_Programmer_CLI`/`openocd`
directly at the slot0 address). Then open the board's serial console and
confirm you see Zephyr's normal `hello_world` output. **This is the
sign of a successful boot.**

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
west flash
```

Sign the test app with both keys and repeat the tamper test against each
signature independently (this is the proof that both must pass -- not
either/or):

```bash
python3 scripts/imgtool.py sign --header-size 0x200 --pad-header --align 4 \
  --slot-size <slot0_partition_size> --version 1.0.0 \
  -k root-ec-p256.pem --pqc-key root-mldsa44.pem \
  ../../build-hello/zephyr/zephyr.bin ../../build-hello/zephyr/signed-hello-hybrid.bin
```

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
