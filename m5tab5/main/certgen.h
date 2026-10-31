/*
 * certgen.h -- a self-signed TLS certificate for this one player, made
 * on the player.
 *
 * Pure mbedTLS: no NVS, no ESP-IDF, so it builds and runs on a host
 * against the same mbedTLS the firmware links (see devcert.c for where
 * the result is kept, and ARCHITECTURE.md 5121 for how it was checked).
 *
 * WHY ON THE DEVICE AND NOT AT BUILD TIME. A key compiled into the
 * firmware is in the firmware image, and the firmware is built from a
 * public repository: every player running a build would share a private
 * key anyone can read. A key generated here never leaves the device.
 *
 * WHAT IT IS AND IS NOT. No browser trusts it -- nothing a browser
 * already trusts signed it -- so the first visit warns once. What it
 * buys is that the connection is encrypted from then on (a Wi-Fi
 * password typed into the page cannot be read by someone listening),
 * and that the certificate can be recognised: the NET tab shows its
 * SHA-256 fingerprint, and a browser shows the same one.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The caller's random source: fill `buf` with `len` random bytes, 0 on
 * success. The firmware passes the hardware RNG. */
typedef int (*certgen_rng_t)(void *ctx, unsigned char *buf, size_t len);

/*
 * An ECDSA P-256 key and a certificate for it, both PEM, NUL-terminated.
 *
 *   subject = issuer = CN=<name>, O=Defeatist Music Player
 *   subjectAltName   = DNS:<lower-case name>.local
 *   valid            2026-01-01 .. 2049-12-31, so a clock that has not
 *                    been set yet, or one set wrong, never makes it
 *                    "not yet valid" or "expired"
 *   serial           16 random bytes, top bit clear
 *   basicConstraints CA:FALSE, keyUsage digitalSignature
 *
 * No IP address in it, deliberately: DHCP can move the address, and a
 * certificate that followed it would change its fingerprint -- the one
 * thing a person is asked to recognise. A browser's one-time exception
 * covers the name mismatch along with the self-signature.
 *
 * Returns 0, or a negative mbedTLS error.
 */
int certgen_make(const char *name, certgen_rng_t rng, void *rng_ctx,
                 char *key_pem, size_t key_cap,
                 char *crt_pem, size_t crt_cap);

/*
 * SHA-256 of the certificate's DER, which is what browsers call its
 * fingerprint. 0, or a negative mbedTLS error.
 */
int certgen_fingerprint(const char *crt_pem, unsigned char out[32]);

#ifdef __cplusplus
}
#endif
