/*
 * netformtest.c -- netform.c (u-defeatist-0048) on a host, under ASan,
 * with gfx.c, feckless_keyboard.c and portalweb.c themselves: the pages
 * fit portrait and landscape, the list dedupes and drops hidden networks,
 * typing goes where the focus is, Join refuses what portalweb_check()
 * refuses in the same words remote.c uses, a join in progress takes no
 * keys, and the password is cleared when it should be.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_wifi.h"
#include "gfx.h"
#include "netform.h"

static int fails, checks;
#define CHECK(c)                                                              \
    do {                                                                      \
        checks++;                                                             \
        if (!(c)) { fails++; fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); } \
    } while (0)

/* feckless_network_handler's, not linked here */
const char *wifi_auth_name(uint8_t auth)
{
    static const char *const n[] = { "open", "WEP", "WPA", "WPA2", "WPA/WPA2", "WPA2-ENT", "WPA3", "WPA2/WPA3" };
    return auth < sizeof n / sizeof n[0] ? n[auth] : "?";
}

static void colours(netform_t *f)
{
    f->bg = 0x0000; f->row = 0x1111; f->field = 0x2222; f->key = 0x3333; f->text = 0xffff;
    f->dim = 0x7777; f->edge = 0x4444; f->go = 0x07e0; f->bad = 0xf800; f->on = 0x0410;
}

static void fit(netform_t *f)
{
    memset(f, 0, sizeof *f);
    netform_fit(f, 0, 0, gfx_w(), gfx_h());
    colours(f);
    netform_begin(f);
}

/* A point on the keyboard that types ch with the state as it is. */
static bool key_at(const netform_t *f, int ch, int *x, int *y)
{
    for (int ty = f->kb.y + 2; ty < f->kb.y + KBD_ROWS * f->kb.key_h; ty += f->kb.key_h / 2) {
        for (int tx = f->kb.x + 2; tx < f->kb.x + f->kb.w; tx += 4) {
            kbd_t k = f->kb;
            if (kbd_hit(&k, tx, ty) == ch) { *x = tx; *y = ty; return true; }
        }
    }
    return false;
}

/* Types s, using shift and #+ as a person would. */
static bool type_str(netform_t *f, const char *s)
{
    for (; *s; s++) {
        int x, y, tries = 0;
        while (!key_at(f, (unsigned char)*s, &x, &y)) {
            /* try shift, then #+, then both off again */
            int sx, sy;
            if (tries == 0 || tries == 2) { if (!key_at(f, KBD_STATE, &sx, &sy)) return false; }
            netform_tap(f, f->kb.x + 2, f->kb.y + 3 * f->kb.key_h + 2);    /* shift: the bottom row's start */
            if (tries == 1) {
                /* #+ : the third row's start */
                if (f->kb.shift) netform_tap(f, f->kb.x + 2, f->kb.y + 3 * f->kb.key_h + 2);
                netform_tap(f, f->kb.x + 2, f->kb.y + 2 * f->kb.key_h + 2);
            }
            if (++tries > 4) return false;
        }
        if (netform_tap(f, x, y) == NETFORM_NONE) return false;
    }
    return true;
}

static wifi_seen_t net(const char *ssid, int8_t rssi, uint8_t auth, bool hidden)
{
    wifi_seen_t n = { .rssi = rssi, .auth = auth, .hidden = hidden };
    snprintf(n.ssid, sizeof n.ssid, "%s", ssid);
    return n;
}

/* Centre of the list's row i. */
static void row_xy(const netform_t *f, int i, int *x, int *y)
{
    *x = f->x + f->w / 2;
    *y = f->y + (f->row_h + f->row_h / 8) * (1 + i) + f->row_h / 2;
}

static void test_fit(void)
{
    netform_t f;
    gfx_set_rotation(0);
    fit(&f);
    CHECK(f.row_h == 72 && f.scale == 3 && f.w == 720);
    CHECK(netform_enter_h(&f) <= f.h);
    CHECK(f.kb.y + KBD_ROWS * f.kb.key_h <= f.h);
    gfx_set_rotation(1);
    fit(&f);
    CHECK(gfx_w() == 1280 && f.w == 900 && f.x == 190 && f.row_h == 60 && f.scale == 2);
    CHECK(netform_enter_h(&f) <= f.h);
    gfx_set_rotation(0);
}

static void test_list(void)
{
    netform_t f;
    fit(&f);
    CHECK(f.page == NETFORM_LIST && f.status == NETFORM_SCANNING);
    netform_draw(&f);
    const wifi_seen_t raw[] = {
        net("fivescore", -41, 7, false), net("", -58, 6, true), net("cafe", -60, 0, false),
        net("fivescore", -70, 7, false), net("ARRIS", -50, 3, false),
    };
    netform_set_scan(&f, raw, 5);
    CHECK(f.status == NETFORM_IDLE && f.scanned && !f.scan_failed);
    CHECK(f.seen_n == 3 && !strcmp(f.seen[0].ssid, "fivescore") && !strcmp(f.seen[1].ssid, "cafe") &&
          !strcmp(f.seen[2].ssid, "ARRIS"));
    CHECK(netform_list_rows(&f) == 3);
    netform_draw(&f);

    /* an open network: refused, and said */
    int x, y;
    row_xy(&f, 1, &x, &y);
    CHECK(netform_tap(&f, x, y) == NETFORM_REDRAW && f.open_tapped && f.page == NETFORM_LIST);
    netform_draw(&f);

    /* Scan again; again while scanning does nothing */
    const int by = f.y + f.h - f.row_h / 2, bw = (f.w - 2 * (f.row_h / 8)) / 3;
    CHECK(netform_tap(&f, f.x + bw + bw / 2, by) == NETFORM_SCAN && f.status == NETFORM_SCANNING);
    CHECK(netform_tap(&f, f.x + bw + bw / 2, by) == NETFORM_NONE);
    netform_set_scan(&f, NULL, -1);
    CHECK(f.scan_failed && f.seen_n == 0 && f.status == NETFORM_IDLE);
    netform_draw(&f);

    /* many: as many rows as fit */
    wifi_seen_t many[40];
    for (int i = 0; i < 40; i++) {
        char s[16];
        snprintf(s, sizeof s, "net%02d", i);
        many[i] = net(s, (int8_t)(-40 - i), 3, false);
    }
    netform_set_scan(&f, many, 40);
    CHECK(f.seen_n == NETFORM_SEEN_MAX);
    CHECK(netform_list_rows(&f) > 8 && netform_list_rows(&f) <= NETFORM_SEEN_MAX);
    netform_draw(&f);

    /* Cancel */
    CHECK(netform_tap(&f, f.x + bw / 2, by) == NETFORM_CLOSE);
}

static void test_enter(void)
{
    netform_t f;
    fit(&f);
    const wifi_seen_t raw[] = { net("fivescore", -41, 7, false) };
    netform_set_scan(&f, raw, 1);
    int x, y;
    row_xy(&f, 0, &x, &y);
    CHECK(netform_tap(&f, x, y) == NETFORM_REDRAW);
    CHECK(f.page == NETFORM_ENTER && !strcmp(f.ssid, "fivescore") && f.from_scan && f.auth == 7 && f.focus == 1);
    netform_draw(&f);

    /* a short password: refused before trying */
    CHECK(type_str(&f, "abc"));
    CHECK(!strcmp(f.pass, "abc"));
    const int gap = f.row_h / 8, bw = (f.w - gap) / 2;
    const int note_h = 2 * (GFX_GLYPH_H(f.scale - 1) + 4) + gap;
    const int buttons_y = f.y + 3 * (f.row_h + gap) + note_h;
    const int join_x = f.x + f.w - bw / 2, back_x = f.x + bw / 2, by = buttons_y + f.row_h / 2;
    CHECK(netform_tap(&f, join_x, by) == NETFORM_REDRAW && f.note == NETFORM_NOTE_BAD_SECRET);
    netform_draw(&f);

    /* del three times, then a real one with a capital, a digit and punctuation */
    int dx, dy;
    CHECK(key_at(&f, KBD_DEL, &dx, &dy));
    for (int i = 0; i < 3; i++) netform_tap(&f, dx, dy);
    CHECK(f.pass[0] == '\0' && f.note == NETFORM_NOTE_NONE);
    CHECK(type_str(&f, "Hunter2!"));
    CHECK(!strcmp(f.pass, "Hunter2!"));
    /* the space bar */
    netform_tap(&f, f.x + f.w / 2, f.kb.y + KBD_ROWS * f.row_h + gap + f.row_h / 2);
    CHECK(!strcmp(f.pass, "Hunter2! "));
    CHECK(key_at(&f, KBD_DEL, &dx, &dy));
    netform_tap(&f, dx, dy);
    CHECK(netform_tap(&f, join_x, by) == NETFORM_JOIN);

    /* joining: no keys, no Back; the note says so */
    netform_set_status(&f, NETFORM_JOINING, ESP_OK);
    netform_draw(&f);
    CHECK(netform_tap(&f, back_x, by) == NETFORM_NONE && f.page == NETFORM_ENTER);
    int kx, ky;
    CHECK(key_at(&f, 'q', &kx, &ky));
    CHECK(netform_tap(&f, kx, ky) == NETFORM_NONE && !strcmp(f.pass, "Hunter2!"));

    /* refused: keys again, and a key clears the failure */
    netform_set_status(&f, NETFORM_FAILED, ESP_ERR_WIFI_PASSWORD);
    netform_draw(&f);
    CHECK(netform_tap(&f, kx, ky) == NETFORM_REDRAW && f.status == NETFORM_IDLE);

    /* joined: Done closes */
    netform_set_status(&f, NETFORM_JOINED, ESP_OK);
    netform_draw(&f);
    CHECK(netform_tap(&f, join_x, by) == NETFORM_CLOSE);
    netform_done(&f);
    CHECK(f.pass[0] == '\0' && f.page == NETFORM_LIST);
}

static void test_other(void)
{
    netform_t f;
    fit(&f);
    netform_set_scan(&f, NULL, 0);
    netform_draw(&f);
    const int by = f.y + f.h - f.row_h / 2, bw = (f.w - 2 * (f.row_h / 8)) / 3;
    CHECK(netform_tap(&f, f.x + f.w - bw / 2, by) == NETFORM_REDRAW);
    CHECK(f.page == NETFORM_ENTER && f.other && !f.from_scan && f.focus == 0 && !f.ssid[0]);
    CHECK(type_str(&f, "hidden-net"));
    CHECK(!strcmp(f.ssid, "hidden-net") && !f.pass[0]);
    /* to the password field */
    const int gap = f.row_h / 8;
    const int pass_y = f.y + 2 * (f.row_h + gap) + f.row_h / 2;
    CHECK(netform_tap(&f, f.x + f.w * 30 / 100 + 20, pass_y) == NETFORM_REDRAW && f.focus == 1);
    CHECK(type_str(&f, "correcthorse"));
    CHECK(!strcmp(f.ssid, "hidden-net") && !strcmp(f.pass, "correcthorse"));
    /* Show */
    const int fw = f.w - f.w * 30 / 100;
    CHECK(netform_tap(&f, f.x + f.w - fw / 8, pass_y) == NETFORM_REDRAW && f.reveal);
    netform_draw(&f);
    const int bw2 = (f.w - gap) / 2;
    const int note_h = 2 * (GFX_GLYPH_H(f.scale - 1) + 4) + gap;
    const int jy = f.y + 3 * (f.row_h + gap) + note_h + f.row_h / 2;
    CHECK(netform_tap(&f, f.x + f.w - bw2 / 2, jy) == NETFORM_JOIN);

    /* Back: the list, the password gone */
    CHECK(netform_tap(&f, f.x + bw2 / 2, jy) == NETFORM_REDRAW && f.page == NETFORM_LIST && !f.pass[0]);

    /* an empty name: refused */
    CHECK(netform_tap(&f, f.x + f.w - bw / 2, by) == NETFORM_REDRAW);
    CHECK(netform_tap(&f, f.x + f.w * 30 / 100 + 20, pass_y) == NETFORM_REDRAW);
    CHECK(type_str(&f, "longenough"));
    CHECK(netform_tap(&f, f.x + f.w - bw2 / 2, jy) == NETFORM_REDRAW && f.note == NETFORM_NOTE_BAD_SSID &&
          f.focus == 0);
    netform_draw(&f);
    netform_done(&f);
}

int main(void)
{
    if (gfx_init(NULL, 720, 1280) != ESP_OK) {
        fprintf(stderr, "gfx_init failed\n");
        return 1;
    }
    test_fit();
    test_list();
    test_enter();
    test_other();
    gfx_set_rotation(1);
    test_enter();           /* landscape: the same taps, found by the same layout */
    gfx_set_rotation(0);
    printf("netform: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
