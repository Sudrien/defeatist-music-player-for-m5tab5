/* SPDX-License-Identifier: MIT
 *
 * boxwin_main.c -- the boxed app (u-defeatist-0002; boxwin.h says what it
 * measures and why).
 *
 * An ELF for a feckless box (feckless_abi_box_elf(), feckless-libs' umode
 * branch), run by a launcher that boxes apps: Avoidant's umode branch. It
 * decodes every .mp3 .mp2 .ape .dsf .dff in a folder called boxwin at the
 * top of the card or the drive and in the folders inside it, one level
 * down, with the player's own decoders, and logs
 * for each how long the box computed between host calls. test_audio_files/
 * copied there is the set the numbers are meant to be compared on.
 *
 * Two passes. The first uses the decoders' C kernels. The second turns on
 * the P4's PIE kernels, which is what the player ships with -- and whether a
 * box can run a PIE instruction at all is not known: ESP-IDF turns the FPU
 * on per task at its first use, feckless_umode does that for a box
 * (u-feckless-0028), and nothing there names the PIE unit. So the C pass
 * and its table are finished and logged first, and the line before the PIE
 * self-test says what it means if the app ends there.
 */
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "feckless_abi.h"
#include "feckless_abi_storage.h"
#include "feckless_box.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "gfx.h"
#include "lcd.h"
#include "storage.h"
#include "storage_io.h"
#include "tab5io.h"
#include "touch.h"

#include "boxwin.h"
#include "boxwin_arith.h"

static const char *TAG = "boxwin";

#define BRIGHTNESS  60
#define SCALE       2
#define PAD         16
#define ROW_H       (GFX_GLYPH_H(SCALE) + 6)
#define C_BG        0x0000
#define C_TEXT      0xFFFF
#define C_DIM       0x8410
#define C_WARN      0xFD20
#define C_BAD       0xF800

#define FOLDER      "boxwin"
#define MAX_FILES   64
#define MAX_SUBS    8               /* folders inside FOLDER that are looked in */
#define NAME_LEN    160             /* "folder/file", from FOLDER down */
#define PATH_LEN    (16 + sizeof(FOLDER) + NAME_LEN)

/* PSRAM in a box, like everything in it; not on the 64 KB stack. */
static char        s_name[MAX_FILES][NAME_LEN];
static bw_result_t s_res[MAX_FILES];
static char        s_line[BW_LINE_MAX];
static int         s_row;

int64_t bw_now(void)   { return esp_timer_get_time(); }
void    bw_lease(void)   { storage_io_acquire(STORAGE_IO_PLAYBACK); }
void    bw_unlease(void) { storage_io_release(); }

/* A line on the panel, under the last. The log is the record; this is for
 * a board with no cable on it. */
static void say(const char *s, uint16_t c)
{
    const int y = PAD + s_row * ROW_H;
    if (y + ROW_H > LCD_V_RES) return;
    gfx_fill_rect(0, y, LCD_H_RES, ROW_H, C_BG);
    gfx_draw_text(PAD, y, s, SCALE, LCD_H_RES - 2 * PAD, c);
    gfx_blit(y, y + ROW_H);
    s_row++;
}

/* "name: 12.3 ms" for the panel, which has no room for bw_format()'s line. */
static void say_result(const char *name, const bw_result_t *r)
{
    char t[120];
    const char *base = strrchr(name, '/');
    base = base ? base + 1 : name;
    if (!r->ok) {
        snprintf(t, sizeof t, "%.56s: %.60s", base, r->why ? r->why : "failed");
        say(t, C_BAD);
        return;
    }
    const bool wrong = r->pcm_want && r->pcm_exact && r->pcm_hash != r->pcm_want;
    snprintf(t, sizeof t, "%.56s%s: %u.%u ms%s", base, r->pie ? " PIE" : "",
             (unsigned)(r->win.max_us / 1000u), (unsigned)(r->win.max_us % 1000u / 100u),
             wrong ? "  WRONG PCM" : "");
    say(t, wrong || r->win.max_us >= BW_AUDIO_US ? C_BAD : r->win.max_us >= BW_PANEL_US ? C_WARN : C_TEXT);
}

static bool wanted(const char *name)
{
    static const char *const ext[] = { "mp3", "mp2", "ape", "dsf", "dff" };
    const char *dot = strrchr(name, '.');
    if (!dot || name[0] == '.') return false;
    for (size_t i = 0; i < sizeof ext / sizeof ext[0]; i++) {
        if (!strcasecmp(dot + 1, ext[i])) return true;
    }
    return false;
}

/* One folder's files into s_name[n..], kept in order by name so two runs'
 * logs line up; `sub` is its name under `dir`, or "" for `dir` itself. The
 * folders in it go into subs[] when that is given. Logs what it found: the
 * first run on a board said only "no files" (u-defeatist-0004), which could
 * have been a folder that would not open as well as one with nothing in it.
 *
 * The folder is closed before another is opened: a box has four DIRs. */
static int list_one(const char *dir, const char *sub, int n,
                    char subs[][NAME_LEN], int *nsubs)
{
    /* storage_join_path() and not snprintf("%s/%s"): it refuses a path
     * that does not fit where snprintf would open a shorter one. */
    char path[PATH_LEN];
    if (!sub[0]) {
        snprintf(path, sizeof path, "%s", dir);
    } else if (!storage_join_path(path, sizeof path, dir, sub)) {
        return n;
    }
    bw_lease();
    DIR *d = opendir(path);
    int seen = 0, took = 0, folders = 0;
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (e->d_name[0] == '.') continue;
            seen++;
            if (e->d_type == DT_DIR) {
                folders++;
                const size_t len = strlen(e->d_name);
                if (subs && *nsubs < MAX_SUBS && len < NAME_LEN) {
                    memcpy(subs[(*nsubs)++], e->d_name, len + 1);
                }
                continue;
            }
            char rel[NAME_LEN];
            if (!wanted(e->d_name) || n >= MAX_FILES) continue;
            if (sub[0]) {
                if (!storage_join_path(rel, sizeof rel, sub, e->d_name)) continue;
            } else {
                const size_t len = strlen(e->d_name);
                if (len >= sizeof rel) continue;
                memcpy(rel, e->d_name, len + 1);
            }
            int at = n;
            while (at > 0 && strcmp(s_name[at - 1], rel) > 0) {
                memcpy(s_name[at], s_name[at - 1], NAME_LEN);
                at--;
            }
            memcpy(s_name[at], rel, NAME_LEN);
            n++;
            took++;
        }
        closedir(d);
    }
    bw_unlease();
    if (!d) {
        ESP_LOGW(TAG, "%s: would not open", path);
    } else {
        ESP_LOGW(TAG, "%s: %d entries, %d folders, %d files to decode", path, seen, folders, took);
    }
    return n;
}

/* FOLDER's files and those of the folders in it, one level down: a copy of
 * test_audio_files/ dropped into it whole is found. */
static int list(const char *dir)
{
    static char subs[MAX_SUBS][NAME_LEN];
    int nsubs = 0;
    int n = list_one(dir, "", 0, subs, &nsubs);
    for (int i = 0; i < nsubs; i++) {
        n = list_one(dir, subs[i], n, NULL, NULL);
    }
    return n;
}

static void pass(const char *dir, int n, bool pie)
{
    for (int i = 0; i < n; i++) {
        char path[PATH_LEN];
        if (!storage_join_path(path, sizeof path, dir, s_name[i])) continue;
        if (pie && !s_res[i].ok) continue;              /* refused in the C pass */
        if (pie && !strncmp(s_res[i].kind, "mp", 2)) continue;  /* minimp3 has no PIE kernel */
        /* Before, not only after: a fault ends the app with no line of its
         * own, and the first one was laid at the wrong file's door for it
         * (u-defeatist-0005). */
        ESP_LOGW(TAG, "next: %s%s", s_name[i], pie ? " (PIE)" : "");
        bw_result_t r;
        bw_run_file(path, pie, &r);
        if (!pie) s_res[i] = r;
        ESP_LOGW(TAG, "%s", bw_format(s_name[i], &r, s_line, sizeof s_line));
        say_result(s_name[i], &r);
    }
}

void app_main(void)
{
    feckless_abi_require();
    feckless_abi_require_storage();
    feckless_abi_require_box();

    ESP_LOGW(TAG, "=== boxwin: the player's decoders in a box, for their windows ===");

    ESP_ERROR_CHECK(tab5io_init());
    esp_lcd_panel_handle_t panel;
    ESP_ERROR_CHECK(lcd_init(&panel));
    ESP_ERROR_CHECK(gfx_init(panel, LCD_H_RES, LCD_V_RES));
    const bool touch = touch_init(tab5io_bus(), LCD_H_RES, LCD_V_RES) == ESP_OK;
    gfx_fill_rect(0, 0, LCD_H_RES, LCD_V_RES, C_BG);
    gfx_blit(0, LCD_V_RES);
    lcd_backlight_set(BRIGHTNESS);
    say("boxwin: longest window a file. The log has the rest.", C_DIM);

    /* u-defeatist-0010: the arithmetic every decoder's set-up stands on,
     * before any of them. It needs no card. */
    bw_arith_t ar;
    bw_arith(&ar);
    const bool dbl_ok = ar.dbl_hash == BW_ARITH_DBL_EXPECT, div_ok = ar.div_hash == BW_ARITH_DIV_EXPECT;
    ESP_LOGW(TAG, "double arithmetic: hash %08x, %s; an add %u ns, a multiply %u ns, "
                  "a divide %u ns, from a 64-bit integer %u ns",
             (unsigned)ar.dbl_hash, dbl_ok ? "the host's" : "NOT THE HOST'S",
             (unsigned)ar.add_ns, (unsigned)ar.mul_ns, (unsigned)ar.div_ns, (unsigned)ar.conv_ns);
    ESP_LOGW(TAG, "64-bit division: hash %08x, %s; a divide %u ns, by 1000 %u ns",
             (unsigned)ar.div_hash, div_ok ? "the host's" : "NOT THE HOST'S",
             (unsigned)ar.udiv_ns, (unsigned)ar.udiv1000_ns);
    if (!dbl_ok || !div_ok) {
        ESP_LOGE(TAG, "the box's arithmetic gave a result a host does not (wanted %08x and %08x): "
                      "what follows was computed on it",
                 (unsigned)BW_ARITH_DBL_EXPECT, (unsigned)BW_ARITH_DIV_EXPECT);
    }
    /* u-defeatist-0013: the double libm, against newlib's own. */
    bw_maths_t mt;
    bw_maths(&mt);
    const bool maths_ok = mt.hash == BW_MATHS_EXPECT;
    ESP_LOGW(TAG, "double maths: hash %08x, %s; sqrt %u ns, sin %u ns, exp %u ns, log %u ns, pow %u ns",
             (unsigned)mt.hash, maths_ok ? "newlib's" : "NOT NEWLIB'S",
             (unsigned)mt.sqrt_ns, (unsigned)mt.sin_ns, (unsigned)mt.exp_ns,
             (unsigned)mt.log_ns, (unsigned)mt.pow_ns);
    if (!maths_ok) {
        ESP_LOGE(TAG, "the box's double maths gave a result newlib's does not (wanted %08x)",
                 (unsigned)BW_MATHS_EXPECT);
    }
    const bool all_ok = dbl_ok && div_ok && maths_ok;
    say(all_ok ? "Arithmetic and maths: as they should be." : "ARITHMETIC OR MATHS: NOT AS THEY SHOULD BE",
        all_ok ? C_DIM : C_BAD);

    vTaskDelay(pdMS_TO_TICKS(1500));    /* the card mounts on storage's poll task */

    char dir[PATH_LEN];
    int n = 0;
    for (int v = 0; v < STORAGE_COUNT && !n; v++) {
        const char *mount = storage_mount_path((storage_id_t)v);
        if (!storage_present((storage_id_t)v)) {
            ESP_LOGW(TAG, "%s: not mounted", mount ? mount : "?");
            continue;
        }
        if (!storage_join_path(dir, sizeof dir, mount, FOLDER)) continue;
        n = list(dir);
    }
    if (!n) {
        ESP_LOGE(TAG, "no .mp3 .mp2 .ape .dsf .dff in a folder called " FOLDER
                      " on the card or the drive, or in a folder inside it");
        say("Nothing to decode: put files in /" FOLDER, C_BAD);
    } else {
        ESP_LOGW(TAG, "%d files in %s; the C kernels", n, dir);
        pass(dir, n, false);

        if (bw_pie_built()) {
            ESP_LOGW(TAG, "the PIE self-tests next. If the app ends here on an illegal "
                          "instruction, a box does not turn the PIE unit on, and the "
                          "table above is all a boxed player would have");
            say("PIE self-test...", C_DIM);
            vTaskDelay(pdMS_TO_TICKS(200));     /* the line out before it */
            if (bw_pie_selftest()) {
                ESP_LOGW(TAG, "PIE works in a box; the PIE kernels");
                pass(dir, n, true);
            } else {
                ESP_LOGE(TAG, "PIE ran in a box and FAILED its self-test against the C kernels");
                say("PIE failed its self-test", C_BAD);
            }
        }
    }
    ESP_LOGW(TAG, "=== boxwin: done ===");
    say(touch ? "Done. Touch to leave." : "Done.", C_DIM);
    for (int i = 0; touch && i < 20 * 600; i++) {       /* ten minutes at most */
        int x, y;
        if (touch_get(&x, &y)) break;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
