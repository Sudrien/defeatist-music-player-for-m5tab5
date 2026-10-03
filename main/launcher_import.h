/*
 * launcher_import.h -- one-tap import of saved Wi-Fi networks from an
 * M5Launcher install sharing this board.
 *
 * WHY THIS EXISTS
 *
 * Under a launcher, Defeatist and M5Launcher each keep their own network
 * list (wifistore.h for us, an AES-encrypted config.conf on the card for
 * Launcher), so a saved password must be entered twice -- once through
 * each app's setup. This reads Launcher's list and saves it here, so the
 * phone-keyboard portal is needed only for networks Launcher never had.
 *
 * WHY IT IS NOT A STORED KEY
 *
 * Launcher encrypts config.conf's passwords with a build-time key
 * (AES-128-CBC, a fixed IV "LauncherWifiKey!"), injected from a CI
 * secret and therefore not in Launcher's source. Hard-coding a captured
 * value would silently stop working the day that secret is rotated. So
 * the key is RECOVERED at run time instead: every printable run in
 * Launcher's own firmware image is a candidate, and the right one is the
 * one whose decryption of a config.conf entry yields valid PKCS#7 over
 * printable text. That oracle needs no known plaintext and survives
 * rotation. See launcher_import.c.
 *
 * WHAT IT WRITES
 *
 * Recovered passphrases go through wifistore_save(ssid, secret, is_psk)
 * exactly as the portal's verified ones do -- is_psk=false for an 8..63
 * passphrase, is_psk=true for a 64-hex value, matching wifistore's
 * contract. Open networks (no secret) are counted and skipped: a
 * credential store has nothing to hold for them. NOTHING here joins a
 * network or logs a passphrase.
 *
 * THREADING, as portal.h: the work runs on its own task and never
 * blocks ui_task. ui_task reads a COPIED snapshot (launcher_import_status)
 * at draw time; it is never handed a pointer the worker owns.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LI_IDLE = 0,   /* never run this boot, or ready to run again */
    LI_RUNNING,    /* worker task scanning / decrypting          */
    LI_DONE,       /* finished; imported/skipped are final       */
    LI_FAILED      /* no config.conf, or key not recoverable     */
} li_phase_t;

typedef struct {
    li_phase_t phase;
    int imported;       /* networks handed to wifistore_save */
    int skipped;        /* open or out-of-range entries passed over */
    char msg[48];       /* short, already-translated-or-literal status line */
} li_status_t;

/*
 * Kick off an import on a worker task and return at once.
 *
 * ESP_ERR_INVALID_STATE if one is already running. Does NOT require the
 * radio: it reads flash, the card and NVS, none of which need Wi-Fi up.
 */
esp_err_t launcher_import_request(void);

/* True between request() and the worker finishing. */
bool launcher_import_running(void);

/* Copy the current snapshot out. Safe from ui_task at any time. */
void launcher_import_status(li_status_t *out);

#ifdef __cplusplus
}
#endif
