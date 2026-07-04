/*
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MCUBOOT_MLDSA_CONFIG_H
#define MCUBOOT_MLDSA_CONFIG_H

/*
 * mldsa-native configuration for MCUboot's bootloader-side signature
 * verification. The bootloader only ever verifies signatures, so signing
 * and key generation (which happen off-device in imgtool) are stripped out
 * to reduce code size.
 */
#define MLD_CONFIG_NO_SIGN_API
#define MLD_CONFIG_NO_KEYPAIR_API

/*
 * Bootloader stacks are small; prefer mldsa-native's reduced-RAM allocation
 * strategy over its default (which trades stack for speed).
 */
#define MLD_CONFIG_REDUCE_RAM

/*
 * MLD_CONFIG_PARAMETER_SET (44/65/87) is injected by the build system
 * (boot/zephyr/CMakeLists.txt, driven by Kconfig) via a compile definition,
 * since MCUboot only ever compiles in one ML-DSA parameter set per build.
 */
#ifndef MLD_CONFIG_PARAMETER_SET
#error "MLD_CONFIG_PARAMETER_SET must be defined by the build system (44, 65, or 87)"
#endif

#endif /* MCUBOOT_MLDSA_CONFIG_H */
