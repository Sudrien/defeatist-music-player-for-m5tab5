/*
 * devcert.h -- this player's own TLS key and certificate, kept in NVS.
 *
 * Made once, the first time the remote starts, by certgen.c, and kept
 * across reboots and firmware updates in the "devcert" NVS namespace --
 * so the fingerprint a browser was told to accept stays the same. Erasing
 * NVS makes a new one, and every browser warns again.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The PEMs, NUL-terminated, lengths INCLUDING the NUL (which is what
 * esp_https_server's parser wants). Generates and stores them the first
 * time; false if that failed. The buffers live for the program's life.
 */
bool devcert_get(const char **crt, size_t *crt_len,
                 const char **key, size_t *key_len);

/* The SHA-256 fingerprint as "AB:CD:...", 95 characters and a NUL, or
 * false before devcert_get() has succeeded. */
bool devcert_fingerprint(char *out, size_t out_size);

#ifdef __cplusplus
}
#endif
