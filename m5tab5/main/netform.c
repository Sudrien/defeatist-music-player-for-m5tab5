/*
 * netform.c -- see netform.h.
 *
 * The rectangle, top to bottom; a row is row_h with a gap of row_h / 8.
 *
 *   LIST                                   ENTER
 *   Add a network          Scanning…       Add a network
 *   fivescore          WPA2/WPA3  -41      Network   [fivescore          ]
 *   ARRIS-B02D             WPA2  -50       Password  [********    ][Show ]
 *   ...as many as fit...                   the note, two lines
 *   the note, two lines                    [  Back  ]        [  Join  ]
 *   [Cancel] [Scan again] [Other network]  the keyboard, four rows
 *                                                 [   space   ]
 *
 * SPDX-License-Identifier: MIT
 */
#include "netform.h"

#include <stdio.h>
#include <string.h>

#include "esp_wifi.h"     /* ESP_ERR_WIFI_PASSWORD */
#include "gfx.h"
#include "i18n.h"

#define AUTH_OPEN 0         /* WIFI_AUTH_OPEN */

static int gap(const netform_t *f) { return f->row_h / 8; }
static int pitch(const netform_t *f) { return f->row_h + gap(f); }
static int small(const netform_t *f) { return f->scale > 1 ? f->scale - 1 : 1; }
static int note_step(const netform_t *f) { return GFX_GLYPH_H(small(f)) + 4; }
static int note_h(const netform_t *f) { return 2 * note_step(f) + gap(f); }
static int body_y(const netform_t *f) { return f->y + pitch(f); }

/* LIST */
static int lbuttons_y(const netform_t *f) { return f->y + f->h - f->row_h; }
static int lnote_y(const netform_t *f) { return lbuttons_y(f) - note_h(f); }
static int lbutton_w(const netform_t *f) { return (f->w - 2 * gap(f)) / 3; }

/* ENTER */
static int label_w(const netform_t *f) { return f->w * 30 / 100; }
static int field_x(const netform_t *f) { return f->x + label_w(f); }
static int field_w(const netform_t *f) { return f->w - label_w(f); }
static int show_w(const netform_t *f) { return field_w(f) / 4; }
static int name_y(const netform_t *f) { return body_y(f); }
static int pass_y(const netform_t *f) { return name_y(f) + pitch(f); }
static int enote_y(const netform_t *f) { return pass_y(f) + pitch(f); }
static int ebuttons_y(const netform_t *f) { return enote_y(f) + note_h(f); }
static int ebutton_w(const netform_t *f) { return (f->w - gap(f)) / 2; }
static int kbd_y(const netform_t *f) { return ebuttons_y(f) + pitch(f); }
static int space_y(const netform_t *f) { return kbd_y(f) + KBD_ROWS * f->row_h + gap(f); }

static bool in(int x, int y, int bx, int by, int bw, int bh)
{
    return x >= bx && x < bx + bw && y >= by && y < by + bh;
}

void netform_fit(netform_t *f, int x, int y, int w, int h)
{
    int rh = h / 12;
    if (rh > 72) rh = 72;
    const int fw = w > 900 ? 900 : w;
    f->x = x + (w - fw) / 2;
    f->y = y;
    f->w = fw;
    f->h = h;
    f->row_h = rh;
    f->scale = rh >= 64 ? 3 : 2;
}

int netform_enter_h(const netform_t *f) { return space_y(f) + f->row_h - f->y; }

static bool busy(const netform_t *f) { return f->status == NETFORM_SCANNING || f->status == NETFORM_JOINING; }

int netform_list_rows(const netform_t *f)
{
    int n = (lnote_y(f) - body_y(f)) / pitch(f);
    if (n < 0) n = 0;
    return n < f->seen_n ? n : f->seen_n;
}

void netform_begin(netform_t *f)
{
    f->page = NETFORM_LIST;
    f->status = NETFORM_SCANNING;
    f->err = ESP_OK;
    f->scan_failed = false;
    f->scanned = false;
    f->seen_n = 0;
    f->ssid[0] = '\0';
    netform_forget_pass(f);
    f->other = false;
    f->focus = 1;
    f->reveal = false;
    f->auth = 0;
    f->from_scan = false;
    f->note = NETFORM_NOTE_NONE;
    f->open_tapped = false;
    f->kb = (kbd_t){ .x = f->x, .y = kbd_y(f), .w = f->w, .key_h = f->row_h, .scale = f->scale,
                     .key = f->key, .text = f->text, .on = f->on };
}

void netform_set_scan(netform_t *f, const wifi_seen_t *raw, int n)
{
    f->scanned = true;
    f->scan_failed = n < 0;
    f->seen_n = 0;
    f->open_tapped = false;
    for (int i = 0; raw && i < n && f->seen_n < NETFORM_SEEN_MAX; i++) {
        if (raw[i].hidden || !raw[i].ssid[0]) continue;
        bool dup = false;
        for (int k = 0; k < f->seen_n && !dup; k++) dup = !strcmp(f->seen[k].ssid, raw[i].ssid);
        if (!dup) f->seen[f->seen_n++] = raw[i];
    }
    if (f->status == NETFORM_SCANNING) f->status = NETFORM_IDLE;
}

void netform_set_status(netform_t *f, netform_status_t status, esp_err_t err)
{
    f->status = status;
    f->err = err;
}

void netform_forget_pass(netform_t *f)
{
    volatile char *p = f->pass;
    for (size_t i = 0; i < sizeof f->pass; i++) p[i] = 0;
}

void netform_done(netform_t *f)
{
    netform_forget_pass(f);
    f->page = NETFORM_LIST;
    f->status = NETFORM_IDLE;
}

/* ---- drawing ----------------------------------------------------------- */

static void button(const netform_t *f, int x, int y, int w, const char *label, uint16_t bg, bool live)
{
    gfx_fill_rect(x, y, w, f->row_h, live ? bg : f->row);
    const int tw = gfx_text_w(label, f->scale);
    gfx_draw_text(x + (tw < w - 8 ? (w - tw) / 2 : 4), y + (f->row_h - GFX_GLYPH_H(f->scale)) / 2, label,
                  f->scale, w - 8, live ? f->text : f->dim);
}

/* A field's box, its edge lit when focused, its text from the end. */
static void box(const netform_t *f, int y, int w, const char *s, bool masked, bool focus, bool live)
{
    const int x = field_x(f), b = f->row_h >= 48 ? 3 : 2;
    gfx_fill_rect(x, y, w, f->row_h, focus && live ? f->go : f->edge);
    gfx_fill_rect(x + b, y + b, w - 2 * b, f->row_h - 2 * b, f->field);
    char shown[sizeof f->pass + 2];
    size_t n = strlen(s);
    if (n > sizeof shown - 2) n = sizeof shown - 2;
    if (masked) memset(shown, '*', n);
    else memcpy(shown, s, n);
    shown[n] = '\0';
    if (focus && live) { shown[n] = '_'; shown[n + 1] = '\0'; }
    gfx_draw_text_tail(x + b + 10, y + (f->row_h - GFX_GLYPH_H(f->scale)) / 2, shown, f->scale,
                       w - 2 * b - 20, live ? f->text : f->dim);
}

static void note(const netform_t *f, int y, const char *s, uint16_t c)
{
    if (s && s[0]) gfx_draw_para(f->x, y, s, small(f), f->w, note_step(f), 2, c);
}

static void title(const netform_t *f, const char *right)
{
    const int th = GFX_GLYPH_H(f->scale);
    gfx_draw_text(f->x, f->y + (f->row_h - th) / 2, _("Add a network"), f->scale, f->w * 2 / 3, f->text);
    if (right) {
        const int s = small(f), rw = gfx_text_w(right, s);
        gfx_draw_text(f->x + f->w - rw, f->y + (f->row_h - GFX_GLYPH_H(s)) / 2, right, s, f->w / 3, f->dim);
    }
}

static void draw_list(const netform_t *f)
{
    title(f, f->status == NETFORM_SCANNING ? _("Scanning…") : NULL);
    const int rows = netform_list_rows(f), s = small(f), th = GFX_GLYPH_H(f->scale);
    for (int i = 0; i < rows; i++) {
        const wifi_seen_t *n = &f->seen[i];
        const int y = body_y(f) + i * pitch(f);
        const bool open = n->auth == AUTH_OPEN;
        gfx_fill_rect(f->x, y, f->w, f->row_h, f->row);
        char right[40];
        snprintf(right, sizeof right, "%s  %d dBm", open ? _("open") : wifi_auth_name(n->auth), (int)n->rssi);
        const int rw = gfx_text_w(right, s);
        gfx_draw_text(f->x + f->w - rw - 16, y + (f->row_h - GFX_GLYPH_H(s)) / 2, right, s, rw, f->dim);
        gfx_draw_text(f->x + 16, y + (f->row_h - th) / 2, n->ssid, f->scale, f->w - rw - 48,
                      open ? f->dim : f->text);
    }
    const char *msg = NULL;
    uint16_t c = f->dim;
    if (f->open_tapped) { msg = _("Open networks are not supported yet."); c = f->bad; }
    else if (f->scan_failed) { msg = _("Could not scan. Is Wi-Fi on?"); c = f->bad; }
    else if (f->scanned && !f->seen_n) msg = _("No networks heard. Scan again, or type a hidden one's name.");
    else if (f->scanned) msg = _("Tap a network, then type its password.");
    note(f, lnote_y(f) + gap(f) / 2, msg, c);
    const int bw = lbutton_w(f), by = lbuttons_y(f);
    button(f, f->x, by, bw, _("Cancel"), f->key, true);
    button(f, f->x + bw + gap(f), by, bw, _("Scan again"), f->key, f->status != NETFORM_SCANNING);
    button(f, f->x + f->w - bw, by, bw, _("Other network"), f->key, true);
}

static void draw_enter(const netform_t *f)
{
    title(f, NULL);
    const bool live = !busy(f) && f->status != NETFORM_JOINED;
    const int th = GFX_GLYPH_H(f->scale);
    gfx_draw_text(f->x, name_y(f) + (f->row_h - th) / 2, _("Network"), f->scale, label_w(f) - 8, f->text);
    box(f, name_y(f), field_w(f), f->ssid, false, f->other && f->focus == 0, live);
    gfx_draw_text(f->x, pass_y(f) + (f->row_h - th) / 2, _("Password"), f->scale, label_w(f) - 8, f->text);
    box(f, pass_y(f), field_w(f) - show_w(f) - gap(f), f->pass, !f->reveal, f->focus == 1, live);
    button(f, field_x(f) + field_w(f) - show_w(f), pass_y(f), show_w(f), f->reveal ? _("Hide") : _("Show"),
           f->reveal ? f->on : f->key, true);

    /* the note: a refusal, else the join's state, else the hint. The
     * join's words are remote.c's (6025), the SSID put in after. */
    char buf[160];
    const char *msg = NULL;
    uint16_t c = f->dim;
    switch (f->note) {
    case NETFORM_NOTE_BAD_SSID:   msg = _("A network name is 1 to 32 characters."); break;
    case NETFORM_NOTE_OPEN:       msg = _("Open networks are not supported yet."); break;
    case NETFORM_NOTE_BAD_SECRET: msg = _("A Wi-Fi password is 8 to 63 characters, or 64 hex digits."); break;
    case NETFORM_NOTE_NON_ASCII:
        msg = _("The password has a character a Wi-Fi password cannot have -- often a phone's smart "
                "punctuation. Retype it with that turned off.");
        break;
    default: break;
    }
    if (msg) c = f->bad;
    else {
        const char *fmt = NULL;
        switch (f->status) {
        case NETFORM_JOINING: fmt = _("Trying %.32s…"); c = f->text; break;
        case NETFORM_JOINED:  fmt = _("Joined and saved %s."); c = f->go; break;
        case NETFORM_FAILED:
            c = f->bad;
            switch (f->err) {
            case ESP_ERR_NOT_FOUND:     fmt = _("%s was not found. Is it in range?"); break;
            case ESP_ERR_WIFI_PASSWORD: fmt = _("%s refused the password."); break;
            case ESP_ERR_TIMEOUT:       fmt = _("%s did not answer."); break;
            case ESP_ERR_INVALID_STATE: msg = _("Wi-Fi is off on the player."); break;
            case ESP_ERR_NO_MEM:        msg = _("The player could not start that. Try again."); break;
            default:                    fmt = _("Could not join %s."); break;
            }
            break;
        default:
            msg = f->other && f->focus == 0 ? _("Type the network's name, then its password.")
                                            : _("A Wi-Fi password is 8 to 63 characters, or 64 hex digits.");
            break;
        }
        if (fmt) {
            snprintf(buf, sizeof buf, fmt, f->ssid);
            msg = buf;
        }
    }
    note(f, enote_y(f) + gap(f) / 2, msg, c);

    const int bw = ebutton_w(f), by = ebuttons_y(f);
    button(f, f->x, by, bw, _("Back"), f->key, !busy(f));
    if (f->status == NETFORM_JOINED) button(f, f->x + f->w - bw, by, bw, _("Done"), f->go, true);
    else button(f, f->x + f->w - bw, by, bw, _("Join"), f->go, live);
    if (live) {
        kbd_draw(&f->kb);
        button(f, f->x + f->w / 4, space_y(f), f->w / 2, _("space"), f->key, true);
    }
}

void netform_draw(const netform_t *f)
{
    gfx_fill_rect(f->x, f->y, f->w, f->h, f->bg);
    if (f->page == NETFORM_LIST) draw_list(f);
    else draw_enter(f);
}

/* ---- taps --------------------------------------------------------------- */

static void enter(netform_t *f, const char *ssid, uint8_t auth, bool from_scan)
{
    f->page = NETFORM_ENTER;
    snprintf(f->ssid, sizeof f->ssid, "%s", ssid);
    f->auth = auth;
    f->from_scan = from_scan;
    f->other = !from_scan;
    f->focus = from_scan ? 1 : 0;
    f->reveal = false;
    f->note = NETFORM_NOTE_NONE;
    f->status = NETFORM_IDLE;
    netform_forget_pass(f);
    f->kb.shift = KBD_SHIFT_OFF;
    f->kb.symbols = false;
}

static netform_result_t type(netform_t *f, int ch)
{
    char *s = f->focus == 0 ? f->ssid : f->pass;
    const size_t cap = f->focus == 0 ? sizeof f->ssid : sizeof f->pass;
    if (f->focus == 0 && !f->other) return NETFORM_NONE;
    f->note = NETFORM_NOTE_NONE;
    if (f->status == NETFORM_FAILED) f->status = NETFORM_IDLE;
    if (ch == KBD_DEL) {
        if (s[0]) kbd_edit(s, cap, KBD_DEL);
        return NETFORM_REDRAW;
    }
    if (ch < 0x20 || ch > 0x7e) return NETFORM_NONE;
    kbd_edit(s, cap, ch);       /* drops it when full */
    return NETFORM_REDRAW;
}

static netform_result_t join(netform_t *f)
{
    portalweb_check_t c = f->ssid[0] ? portalweb_check(f->ssid, f->pass) : PORTALWEB_BAD_SSID;
    switch (c) {
    case PORTALWEB_OK_PASSPHRASE:
    case PORTALWEB_OK_PSK:
        f->note = NETFORM_NOTE_NONE;
        return NETFORM_JOIN;
    case PORTALWEB_BAD_SSID:   f->note = NETFORM_NOTE_BAD_SSID; f->focus = f->other ? 0 : 1; break;
    case PORTALWEB_NO_SECRET:  f->note = NETFORM_NOTE_OPEN; f->focus = 1; break;
    case PORTALWEB_NON_ASCII:  f->note = NETFORM_NOTE_NON_ASCII; f->focus = 1; break;
    default:                   f->note = NETFORM_NOTE_BAD_SECRET; f->focus = 1; break;
    }
    return NETFORM_REDRAW;
}

static netform_result_t tap_list(netform_t *f, int tx, int ty)
{
    const int bw = lbutton_w(f), by = lbuttons_y(f);
    if (in(tx, ty, f->x, by, bw, f->row_h)) return NETFORM_CLOSE;
    if (in(tx, ty, f->x + bw + gap(f), by, bw, f->row_h)) {
        if (f->status == NETFORM_SCANNING) return NETFORM_NONE;
        f->status = NETFORM_SCANNING;
        f->open_tapped = false;
        return NETFORM_SCAN;
    }
    if (in(tx, ty, f->x + f->w - bw, by, bw, f->row_h)) {
        enter(f, "", 0, false);
        return NETFORM_REDRAW;
    }
    const int rows = netform_list_rows(f);
    for (int i = 0; i < rows; i++) {
        if (!in(tx, ty, f->x, body_y(f) + i * pitch(f), f->w, f->row_h)) continue;
        if (f->seen[i].auth == AUTH_OPEN) {
            f->open_tapped = true;
            return NETFORM_REDRAW;
        }
        enter(f, f->seen[i].ssid, f->seen[i].auth, true);
        return NETFORM_REDRAW;
    }
    return NETFORM_NONE;
}

static netform_result_t tap_enter(netform_t *f, int tx, int ty)
{
    const int bw = ebutton_w(f), by = ebuttons_y(f);
    if (in(tx, ty, f->x, by, bw, f->row_h)) {
        if (busy(f)) return NETFORM_NONE;
        netform_forget_pass(f);
        f->page = NETFORM_LIST;
        f->note = NETFORM_NOTE_NONE;
        f->status = NETFORM_IDLE;
        return NETFORM_REDRAW;
    }
    if (f->status == NETFORM_JOINED) {
        return in(tx, ty, f->x + f->w - bw, by, bw, f->row_h) ? NETFORM_CLOSE : NETFORM_NONE;
    }
    if (in(tx, ty, field_x(f) + field_w(f) - show_w(f), pass_y(f), show_w(f), f->row_h)) {
        f->reveal = !f->reveal;
        return NETFORM_REDRAW;
    }
    if (busy(f)) return NETFORM_NONE;
    if (in(tx, ty, f->x + f->w - bw, by, bw, f->row_h)) return join(f);
    const int k = kbd_hit(&f->kb, tx, ty);
    if (k == KBD_STATE) return NETFORM_REDRAW;
    if (k == KBD_DEL || k > 0) {
        const netform_result_t r = type(f, k);
        return r == NETFORM_NONE ? NETFORM_REDRAW : r;      /* a shift used up */
    }
    if (in(tx, ty, f->x + f->w / 4, space_y(f), f->w / 2, f->row_h)) return type(f, ' ');
    if (f->other && in(tx, ty, field_x(f), name_y(f), field_w(f), f->row_h) && f->focus != 0) {
        f->focus = 0;
        return NETFORM_REDRAW;
    }
    if (in(tx, ty, field_x(f), pass_y(f), field_w(f) - show_w(f), f->row_h) && f->focus != 1) {
        f->focus = 1;
        return NETFORM_REDRAW;
    }
    return NETFORM_NONE;
}

netform_result_t netform_tap(netform_t *f, int tx, int ty)
{
    return f->page == NETFORM_LIST ? tap_list(f, tx, ty) : tap_enter(f, tx, ty);
}
