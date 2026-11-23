/*
 * netform.h -- adding a Wi-Fi network on the player's own screen.
 *
 * u-defeatist-0048. portal.h's first line was "There is no keyboard",
 * and the setup portal -- an open access point, captive DNS and a form
 * served to a phone -- existed to borrow one. feckless_keyboard is one,
 * so the network gets typed here, and the NET tab's "Add a network"
 * opens this instead of the portal.
 *
 * Two pages:
 *
 *   LIST    the scan: one row a network, strongest first, a name once
 *           (portal.c's and remote.c's dedupe), hidden ones left out;
 *           open networks dimmed, because portalweb_check() refuses them
 *           ("Open networks are not supported yet.") as both web forms
 *           do. Under it Cancel, Scan again, and Other network -- for a
 *           hidden one, typed by name.
 *   ENTER   the network's name (typed only for Other), the password with
 *           Show, a note, Back and Join, and the keyboard with a space
 *           bar. The note is the join's state and its words are remote.c's,
 *           so the browser's Wi-Fi section and this one say the same
 *           things, translated once.
 *
 * Shaped like feckless_mail's mailform: it owns no task, no radio and no
 * hardware. panel.c gives it a rectangle, draws it on ui_task, hands it
 * taps, and does what netform_tap() returns -- NETFORM_SCAN and
 * NETFORM_JOIN through wifiadd.c, whose results come back through
 * netform_set_scan() and netform_set_status(). So it runs on a host under
 * ASan (texttest/netformtest.c), with gfx.c, feckless_keyboard.c and
 * portalweb.c themselves.
 *
 * State is held as values and enums and put into words at draw time
 * (portal.h, "STATE IS COPIED, NEVER BORROWED"). The password lives in
 * f->pass from the first key to netform_done() or a join handed over,
 * and nowhere else; netform_done() clears it.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "feckless_keyboard.h"
#include "portalweb.h"
#include "wifi.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Networks listed: as many as portal.c kept. */
#define NETFORM_SEEN_MAX    (24)

typedef enum { NETFORM_LIST = 0, NETFORM_ENTER } netform_page_t;

/* What the app last told it, from wifiadd.c. */
typedef enum {
    NETFORM_IDLE = 0,
    NETFORM_SCANNING,
    NETFORM_JOINING,
    NETFORM_JOINED,         /* joined and saved */
    NETFORM_FAILED,         /* err says why */
} netform_status_t;

/* What a tap did. */
typedef enum {
    NETFORM_NONE = 0,       /* nothing: no need to redraw */
    NETFORM_REDRAW,
    NETFORM_CLOSE,          /* Cancel, or Done after a join: leave */
    NETFORM_SCAN,           /* Scan again: start a scan, then netform_set_scan() */
    NETFORM_JOIN,           /* Join, and the pair passed portalweb_check():
                             * hand f->ssid, f->pass and wifijoin_net(f->auth)
                             * (UNKNOWN if !from_scan) to the join,
                             * then netform_set_status(JOINING) */
} netform_result_t;

/* Why Join was refused before trying, or none. */
typedef enum {
    NETFORM_NOTE_NONE = 0,
    NETFORM_NOTE_BAD_SSID,
    NETFORM_NOTE_OPEN,
    NETFORM_NOTE_BAD_SECRET,
    NETFORM_NOTE_NON_ASCII,
} netform_note_t;

typedef struct {
    /* where: the whole rectangle, which it fills */
    int x, y, w, h;
    int row_h;              /* a row, a button, a key */
    int scale;              /* gfx text scale */
    uint16_t bg, row, field, key, text, dim, edge, go, bad, on;

    /* set by netform_begin() and the calls below */
    netform_page_t page;
    netform_status_t status;
    esp_err_t err;          /* with NETFORM_FAILED */
    bool scan_failed;       /* the last scan came back -1 */
    bool scanned;           /* a scan has come back since netform_begin() */
    wifi_seen_t seen[NETFORM_SEEN_MAX];
    int seen_n;
    char ssid[33];
    char pass[65];          /* a passphrase or 64 hex digits, and its terminator */
    bool other;             /* the name is typed (Other network) */
    int focus;              /* 0 the name, 1 the password */
    bool reveal;
    uint8_t auth;           /* the scanned network's wifi_auth_mode_t ... */
    bool from_scan;         /* ... when it came from the list: wifijoin_net() of it */
    netform_note_t note;
    bool open_tapped;       /* LIST: an open network was tapped; say why not */
    kbd_t kb;
} netform_t;

/* The geometry for a w x h screen at (x, y): a row a twelfth of the
 * height at most 72 px, the width at most 900 px and centred, so the
 * ENTER page -- the tallest -- fits portrait and landscape alike. The
 * colours are the caller's. */
void netform_fit(netform_t *f, int x, int y, int w, int h);

/* The height the ENTER page reaches from f->y, for the test. */
int netform_enter_h(const netform_t *f);

/* Start on LIST, scanning; set the geometry and colours first. The caller
 * starts the scan. */
void netform_begin(netform_t *f);

/* A scan's result: n networks in raw (wifi_scan_list()'s), or n < 0 for
 * a failed scan. Status back to IDLE. */
void netform_set_scan(netform_t *f, const wifi_seen_t *raw, int n);

/* The join's progress, or a scan's start. */
void netform_set_status(netform_t *f, netform_status_t status, esp_err_t err);

void netform_draw(const netform_t *f);
netform_result_t netform_tap(netform_t *f, int tx, int ty);

/* The password cleared: after a join has its copy, and on leaving. */
void netform_forget_pass(netform_t *f);
void netform_done(netform_t *f);

/* Rows the list page shows, for the test. */
int netform_list_rows(const netform_t *f);

#ifdef __cplusplus
}
#endif
