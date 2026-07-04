/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * ML-DSA (FIPS 204) post-quantum signature verification, using the
 * mldsa-native reference implementation (ext/mldsa-native).
 *
 * Modeled on image_ed25519.c: the public key is embedded as a
 * SubjectPublicKeyInfo DER structure, unwrapped here to the raw ML-DSA
 * public key bytes expected by mldsa-native's verify() API.
 *
 * Deliberately exported under a name distinct from the classical
 * algorithms' bootutil_verify_sig(), since hybrid builds compile both a
 * classical image_*.c file and this file into the same bootloader image;
 * image_validate.c calls this one directly for the ML-DSA signature TLV
 * (and, in PQC-only builds with no classical algorithm compiled,
 * bootutil_verify_sig is #define'd to this function -- see
 * image_validate.c).
 */

#include <string.h>

#include "mcuboot_config/mcuboot_config.h"

#ifdef MCUBOOT_SIGN_MLDSA
#include "bootutil/sign_key.h"

#if !defined(MCUBOOT_KEY_IMPORT_BYPASS_ASN)
/* We are not really using the MBEDTLS but need the ASN.1 parsing functions */
#define MBEDTLS_ASN1_PARSE_C
#include "mbedtls/oid.h"
#include "mbedtls/asn1.h"
#include "bootutil/crypto/common.h"
#endif

#include "bootutil/bootutil_log.h"
#include "bootutil/crypto/sha.h"
#include "bootutil_priv.h"

#include "mldsa/mldsa_native.h"

BOOT_LOG_MODULE_DECLARE(mcuboot);

#if MCUBOOT_MLDSA_LEVEL == 44
#define MLDSA_PUBKEY_LEN MLDSA44_PUBLICKEYBYTES
#define MLDSA_SIGNATURE_LEN MLDSA44_BYTES
/* DER content bytes (excluding tag/length) of OID 2.16.840.1.101.3.4.3.17
 * (id-ml-dsa-44). */
static const uint8_t mldsa_pubkey_oid[] = { 0x60, 0x86, 0x48, 0x01, 0x65,
                                             0x03, 0x04, 0x03, 0x11 };
#elif MCUBOOT_MLDSA_LEVEL == 65
#define MLDSA_PUBKEY_LEN MLDSA65_PUBLICKEYBYTES
#define MLDSA_SIGNATURE_LEN MLDSA65_BYTES
/* OID 2.16.840.1.101.3.4.3.18 (id-ml-dsa-65). */
static const uint8_t mldsa_pubkey_oid[] = { 0x60, 0x86, 0x48, 0x01, 0x65,
                                             0x03, 0x04, 0x03, 0x12 };
#elif MCUBOOT_MLDSA_LEVEL == 87
#define MLDSA_PUBKEY_LEN MLDSA87_PUBLICKEYBYTES
#define MLDSA_SIGNATURE_LEN MLDSA87_BYTES
/* OID 2.16.840.1.101.3.4.3.19 (id-ml-dsa-87). */
static const uint8_t mldsa_pubkey_oid[] = { 0x60, 0x86, 0x48, 0x01, 0x65,
                                             0x03, 0x04, 0x03, 0x13 };
#else
#error "Unsupported MCUBOOT_MLDSA_LEVEL (must be 44, 65, or 87)"
#endif

#if !defined(MCUBOOT_KEY_IMPORT_BYPASS_ASN)
/*
 * Parse the public key used for signing.
 */
static int
bootutil_import_key_mldsa(uint8_t **cp, uint8_t *end)
{
    size_t len;
    mbedtls_asn1_buf alg;
    mbedtls_asn1_buf param;

    if (mbedtls_asn1_get_tag(cp, end, &len,
        MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE)) {
        return -1;
    }
    end = *cp + len;

    if (mbedtls_asn1_get_alg(cp, end, &alg, &param)) {
        return -2;
    }

    if (alg.ASN1_CONTEXT_MEMBER(len) != sizeof(mldsa_pubkey_oid) ||
        memcmp(alg.ASN1_CONTEXT_MEMBER(p), mldsa_pubkey_oid, sizeof(mldsa_pubkey_oid))) {
        return -3;
    }

    if (mbedtls_asn1_get_bitstring_null(cp, end, &len)) {
        return -4;
    }
    if (*cp + len != end) {
        return -5;
    }

    if (len != MLDSA_PUBKEY_LEN) {
        return -6;
    }

    return 0;
}
#endif /* !defined(MCUBOOT_KEY_IMPORT_BYPASS_ASN) */

/* Signature verification base function.
 * The function takes buffer of specified length and tries to verify
 * it against provided signature.
 * The function does key import and checks whether signature is
 * of expected length.
 */
fih_ret
bootutil_verify_sig_mldsa(uint8_t *msg, uint32_t mlen, uint8_t *sig, size_t slen,
                          uint8_t key_id)
{
    int rc;
    FIH_DECLARE(fih_rc, FIH_FAILURE);
    uint8_t *pubkey;
    uint8_t *end;

    BOOT_LOG_DBG("bootutil_verify_sig_mldsa: ML-DSA-%d key_id %d", MCUBOOT_MLDSA_LEVEL, (int)key_id);

#if !defined(MCUBOOT_SIGN_PURE)
    if (mlen != IMAGE_HASH_SIZE) {
        BOOT_LOG_DBG("bootutil_verify_sig_mldsa: expected hash len %d, got %d",
                     IMAGE_HASH_SIZE, mlen);
        goto out;
    }
#endif

    if (slen != MLDSA_SIGNATURE_LEN) {
        BOOT_LOG_DBG("bootutil_verify_sig_mldsa: expected slen %d, got %u",
                     MLDSA_SIGNATURE_LEN, (unsigned int)slen);
        FIH_SET(fih_rc, FIH_FAILURE);
        goto out;
    }

    pubkey = (uint8_t *)bootutil_keys[key_id].key;
    end = pubkey + *bootutil_keys[key_id].len;

#if !defined(MCUBOOT_KEY_IMPORT_BYPASS_ASN)
    rc = bootutil_import_key_mldsa(&pubkey, end);
    if (rc) {
        BOOT_LOG_DBG("bootutil_verify_sig_mldsa: import key failed %d", rc);
        FIH_SET(fih_rc, FIH_FAILURE);
        goto out;
    }
#else
    /* Directly use the key contents from the ASN stream,
     * these are the last MLDSA_PUBKEY_LEN bytes.
     * There is no check whether this is the correct key,
     * here, by the algorithm selected.
     */
    BOOT_LOG_DBG("bootutil_verify_sig_mldsa: bypass ASN1");
    if (*bootutil_keys[key_id].len < MLDSA_PUBKEY_LEN) {
        FIH_SET(fih_rc, FIH_FAILURE);
        goto out;
    }

    pubkey = end - MLDSA_PUBKEY_LEN;
#endif

    rc = crypto_sign_verify(sig, slen, msg, mlen, NULL, 0, pubkey);

    if (rc != 0) {
        /* mldsa-native returns 0 on success, non-zero on failure. */
        FIH_SET(fih_rc, FIH_FAILURE);
        goto out;
    }

    FIH_SET(fih_rc, FIH_SUCCESS);
out:

    FIH_RET(fih_rc);
}

#endif /* MCUBOOT_SIGN_MLDSA */
