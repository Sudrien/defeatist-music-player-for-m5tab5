/*
 * shim_import.h -- import of saved Wi-Fi networks from an M5Launcher
 * install sharing this board. Runs on the shim's side, not in the app.
 *
 * WHY IT IS HERE AND NOT IN THE APP (8068)
 *
 * It was a row in the network panel, started by a tap, on its own task.
 * The app is becoming an ELF on the feckless ABI, and this is the one
 * thing in it that has no business there: it reads another firmware's
 * flash partition, decrypts with a vendored AES, and parses Launcher's
 * config.conf -- none of it names, or could name, an ABI function. The
 * firmware that Launcher actually boots is the shim (the standalone .bin
 * that embeds the ELF), so the shim does it, once, before the app runs;
 * the ELF finds the networks already in NVS and never knows. Under
 * Avoidant's ELF mode no shim runs, and Avoidant has its own import
 * (its 0045).
 *
 * WHEN IT RUNS
 *
 * shim_import_launcher_wifi() is called before the radio is started, and
 * does nothing -- reads nothing from flash -- unless ALL of these hold:
 *   - launcher_kind_now() is LAUNCHER_SLOT (booted from a launcher's
 *     table slot, as M5Launcher boots a firmware);
 *   - no network is saved yet (wifistore_count() == 0), so it never
 *     overwrites a list the user built;
 *   - it has not run to a conclusion before (a u8 "limport" in the
 *     app's NVS namespace).
 * No card, or no config.conf on it, is not a conclusion: it retries the
 * next boot. Anything past that point is, whether it imported or could
 * not recover the key, because the scan is seconds of flash reads that
 * the same card would repeat for the same answer.
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
 * rotation. See shim_import.c.
 *
 * WHAT IT WRITES
 *
 * Recovered passphrases go through wifistore_save(ssid, secret, is_psk)
 * exactly as the portal's verified ones do -- is_psk=false for an 8..63
 * passphrase, is_psk=true for a 64-hex value. Open networks are skipped:
 * a credential store has nothing to hold for them. NOTHING here joins a
 * network or logs a passphrase.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Needs: NVS up (nvsh_init), wifistore_init(nvs_ns) done, the card
 * mounted. Does not need the radio. Blocks for as long as the scan takes
 * (seconds, once); call it from the boot task, not a UI task.
 *
 * Returns the number of networks saved (0 when it did nothing or found
 * none). nvs_ns is the namespace wifistore was given.
 */
int shim_import_launcher_wifi(const char *nvs_ns);

#ifdef __cplusplus
}
#endif
