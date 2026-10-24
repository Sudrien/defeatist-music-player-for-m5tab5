/*
 * browser.c -- the file chooser.
 *
 * SPDX-License-Identifier: MIT
 */

#include "esp_attr.h"
#include <dirent.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "browser.h"
#include "i18n.h"         /* 6017 */
#include "cuedir.h"
#include "m3uline.h"
#include "favorites.h"
#include "starred.h"
#include "radiobrowser.h"
#include "stations.h"
#include "decoder.h"
#include "gfx.h"
#include "storage.h"
#include "recorder.h"         /* 5215 */
#include "mpd.h"              /* 5260 */
#include "podindex.h"         /* 8008 */
#include "remote.h"           /* 8008: remote_url() */
#include "ethernet.h"         /* 8016: net_online() */
#include "poddl.h"            /* 8020 */
#include "podpath.h"          /* 8020 */

static const char *TAG = "tab5_browser";

#define C_BG        RGB(0x0C, 0x0C, 0x0C)
#define C_ROW       RGB(0x1A, 0x1A, 0x1A)
#define C_ROW_ALT   RGB(0x14, 0x14, 0x14)
#define C_TEXT      RGB(0xEE, 0xEE, 0xEE)
#define C_DIM       RGB(0x77, 0x77, 0x77)
#define C_DISABLED  RGB(0x44, 0x44, 0x44)
#define C_ACCENT    RGB(0xD1, 0x3B, 0x2C)
/*
 * Starred. Gold rather than a second use of C_ACCENT, which already
 * means "this is the one playing" -- two states on the same row need
 * two colours or the row says one thing and means either.
 */
#define C_STAR      RGB(0xE8, 0xB3, 0x2C)
#define C_TAB_ON    RGB(0x22, 0x22, 0x22)
#define C_TAB_OFF   RGB(0x10, 0x10, 0x10)
#define C_BTN       RGB(0x26, 0x26, 0x26)
#define C_RULE      RGB(0x33, 0x33, 0x33)
/* Between C_RULE and C_DIM. The prefix note has to be readable when
 * looked for and easy to look past when not, which neither of the
 * existing greys manages -- one disappears, the other competes with the
 * path above it. */
#define C_FAINT     RGB(0x55, 0x55, 0x55)

/* Layout. Sized for the same 294 PPI the transport bar is: a 88 px row is
 * about 7.5 mm, which is a comfortable thumb target, and the 27 px text in
 * it is the same height as the artist line on the player. */
#define TAB_H       (96)
#define PATH_H      (60)
#define LIST_TOP    (TAB_H + PATH_H)
#define FOOT_H      (120)
#define ROW_H       (88)

/*
 * The scrollbar: how wide it is drawn, and how much wider it is to hit.
 *
 * The two differ on purpose. A 16 px bar is the right weight next to
 * 64 px rows; a 16 px target is not, at 294 PPI with a fingertip. The
 * grab zone is the outer 72 px of the list, which is dead space in every
 * row anyway -- filenames are drawn from the left and the ones long
 * enough to reach the right edge are already being clipped there.
 */
#define SCROLL_W        (16)

/*
 * The star's column, at the right of a station row and clear of the
 * scrollbar.
 *
 * STAR_W is the whole tappable strip, not the glyph: the glyph is
 * STAR_R across and a 72 px strip is 6 mm at 294 PPI, which is what a
 * thumb needs for a target that sits beside a much larger one. The row
 * tap still owns everything to the left of it, so the common action --
 * play this station -- keeps the whole width it had minus this strip,
 * and the name's draw width shrinks by the same amount so a long name
 * cannot run under the star.
 */
#define STAR_W          (72)
#define STAR_R          (18)
#define SCROLL_HIT_W    (72)
#define NAME_SCALE  (3)
#define LABEL_SCALE (2)

/* Entries held for the open directory. Beyond this the listing is
 * truncated, for the same reason PLAYLIST_MAX exists: this is a strdup
 * per name on a touch event. */
#define MAX_ENTRIES (512)

typedef struct {
    char *name;
    /*
     * What to draw instead of the name, or NULL. Set for cue tracks
     * only, whose name is "Album.cue#03" -- the thing that is opened and
     * compared, and nothing anyone wants to read. The prefix elision
     * does not apply to a label; it is already the part that differs.
     */
    char *label;
    bool is_dir;
    /*
     * Starred, resolved when the row was built and not while drawing.
     *
     * favorites_contains() is a scan of up to STATIONLIST_MAX entries.
     * Once per row per list load is 64x64 comparisons; once per row per
     * FRAME is that at the panel's refresh rate, for an answer that
     * cannot change without something calling browser_stations_reloaded()
     * anyway. The URL is not kept -- only the answer -- so this stays
     * true to BROWSER_PLAY_STREAM's rule that stations.c is the only
     * thing that knows what station i is.
     */
    bool fav;
} entry_t;

static bool s_open;
static bool s_dirty;
static bool s_was_down;

/*
 * Which tab is up.
 *
 * A third tab rather than an entry in the file list, and the reason is
 * that a station is not on a volume. An entry would have to live in
 * some directory, which would make it appear and disappear with the
 * card and put it in a sort order next to filenames. The tabs already
 * mean "which source of things to play", which is exactly the
 * distinction.
 *
 * BROWSER_TAB_SD and _USB match storage_id_t numerically so the two
 * that are volumes can still be passed to storage_present() and
 * storage_mount_path() without a mapping table.
 */
typedef enum {
    BROWSER_TAB_SD = STORAGE_SD,
    BROWSER_TAB_USB = STORAGE_USB,
    BROWSER_TAB_RADIO,
    BROWSER_TAB_PODCAST,    /* 8008: no rows yet; says what it needs */
    BROWSER_TAB_COUNT
} browser_tab_t;

static browser_tab_t s_tab = BROWSER_TAB_SD;

/*
 * 8008: the PODCAST tab is showing. Like s_radio it means "not a
 * volume", and every test below that took !s_radio to mean a volume
 * tests this too -- storage_present() and storage_mount_path() on
 * BROWSER_TAB_PODCAST read past the volume array. It has no rows yet:
 * it says whether there is a Podcast Index key and where to enter one.
 * s_pod_seen is the key state last drawn, so a key that starts working
 * while the tab is up redraws it.
 */
static bool s_pod;
static podindex_state_t s_pod_seen;
/* 8014: the categories the rows were built from, in the same order, so
 * row i IS category i; and the generation they came from. */
static podcat_t s_pod_cats[PODCATS_MAX];
static int      s_pod_n;
static uint32_t s_pod_gen;
/* 8016: the tab wants the categories and there is no network yet. The
 * fetch starts when there is one, and the spinner turns until then. */
static bool     s_pod_wait;
/*
 * 8018: the tab's two levels, as RADIO has two: the categories, and one
 * category's podcasts. s_pod_cat is the category's row in s_pod_cats;
 * the podcasts are copied in as the categories are, row i IS podcast i.
 */
static int       s_pod_level;        /* 0 categories, 1 podcasts, 2 episodes (8019), 3 which drive (8020) */
static int       s_pod_cat;
static podfeed_t s_pod_feeds[PODFEEDS_MAX];
static int       s_pod_fn;
static uint32_t  s_pod_fgen;
/* 8019: the third level, one podcast's episodes; s_pod_feed is the
 * podcast's row in s_pod_feeds. */
static int       s_pod_feed;
static podep_t   s_pod_eps[PODEPS_MAX];
static int       s_pod_en;
static uint32_t  s_pod_egen;
/*
 * 8020: downloads. s_pod_show is the podcast's title, kept when its row
 * is tapped, because the folder is named for it and the podcast list may
 * be another category's by the time an episode is. Level 3 is the drive
 * picker, shown only when both are in: s_pod_pick is the episode's row,
 * s_pod_vols the volumes its two rows stand for.
 */
static char         s_pod_show[PODFEEDS_TITLE_MAX];
static int          s_pod_pick;
static storage_id_t s_pod_vols[STORAGE_COUNT];
static int          s_pod_nvols;
/* 8024: level 3 is the drive picker, or -- for the episode that is
 * downloading, paused or stopped -- its two actions. */
static bool         s_pod_act;
static storage_id_t s_pod_act_vol;      /* where that episode's part file is */
static uint32_t     s_pod_dlgen;
static const char  *s_pod_note;         /* a one-off answer to the last tap, or NULL */
/*
 * 8021: the podcast whose episodes level 2 lists, by id rather than by
 * its row in s_pod_feeds -- it may have come from a folder's link, with
 * no podcast list behind it, in which case UP goes to the categories.
 */
static int64_t      s_pod_cur;
static bool         s_pod_linked;
/* 8021: a volume tab's folder that holds PODPATH_LINK: row 0 is the way
 * back to that podcast, and these say which. */
static bool         s_dir_link;
static int64_t      s_dir_link_feed;
static char         s_dir_link_show[PODFEEDS_TITLE_MAX];

/*
 * The station list, loaded into the same rows the files use.
 *
 * Copied in rather than drawn from stations.c directly, so that the
 * scrollbar, the row striping, the paging and the clipping all work
 * unchanged -- this tab adds a source of names, not a second list
 * widget. is_dir is false for every one of them.
 *
 * NOTE: the chooser does NOT call stations_load(). That opens a file and
 * ui_task is the one task that must not block on the card; the player
 * loads the list from its idle branch. So this tab shows the list as
 * last read, and a stations.m3u edited with the card still in will not
 * appear until something reloads it.
 */
static bool s_radio;
/*
 * THE RADIO TAB HAS TWO LEVELS AND THIS SAYS WHICH.
 *
 * Menu: the card's list, the two charts, the pinned tags. Stations:
 * whatever list is loaded. It is the folders-then-files gesture the
 * volume tabs already use, against the same rows, which is the whole
 * argument for browsing a directory on a device with no keyboard --
 * the interaction already exists and needed no typing.
 *
 * The menu is the level the tab opens on, always. Opening on the last
 * list would mean the tab shows a tag somebody browsed twenty minutes
 * ago as though it were theirs.
 */
static bool s_radio_menu;
/* Set by the player task while a fetch is in flight, or after one
 * failed. Cleared when a list arrives. */
static char s_radio_status[96];
/* 5067: see browser_set_radio_busy(). */
static volatile bool s_radio_busy;
static uint32_t      s_spin_frame;      /* last spinner step drawn */
static char s_dir[512];
static char s_result[512];

static entry_t *s_entries;
static int s_count;
static int s_top;               /* first visible row */

/*
 * A drag on the scrollbar, in progress.
 *
 * The bar was drawn from the first version and has never been touchable:
 * eight pixels of decoration reporting a position on a list that could
 * only be moved a page at a time from the footer. On a 9-track folder
 * that is a curiosity. On a folder with two hundred files in it, it is
 * twenty-two presses to reach the end of something the bar has been
 * showing you the shape of the whole time.
 *
 * Held across polls because a drag is not a press: browser_touch() is
 * otherwise built entirely on the down edge, which is right for buttons
 * and useless for anything that has to follow a finger.
 */
static bool s_scroll_drag;

/* The shared leading text of this folder's filenames, and its length.
 * Declared up here rather than beside find_common_prefix() because
 * entries_free() has to be able to drop it. */
static char s_prefix[128];
static int  s_prefix_len;

static play_order_t s_order = PLAY_ORDER_ALL;
static uint32_t s_seen_generation = UINT32_MAX;
static uint32_t s_seen_rec_files;           /* 5215 */
static bool     s_seen_oneshot;             /* 5260 */

play_order_t browser_order(void) { return s_order; }

static const char *order_label(void);

void browser_set_order(play_order_t o)
{
    if ((int)o < 0 || (int)o > (int)PLAY_ORDER_REPEAT_ALL || o == s_order) return;
    s_order = o;
    ESP_LOGI(TAG, "play order now %s (from mpd)", order_label());
    s_dirty = true;
}
bool browser_is_open(void) { return s_open; }

static int rows_visible(void)
{
    return (gfx_h() - FOOT_H - LIST_TOP) / ROW_H;
}

/*
 * The scrollbar's geometry, in one place.
 *
 * Drawing and hit-testing computed this independently in the first
 * version of this patch and it was wrong within a day -- the thumb was
 * drawn from LIST_TOP and grabbed from the top of the screen, so the
 * finger led the bar by the height of the tab strip. One function, two
 * callers, no chance to drift.
 *
 * Returns false when the list fits, which is also when there is nothing
 * to drag.
 */
static bool scroll_geom(int *track_y, int *track_h, int *bar_h, int *max_top)
{
    const int rows = rows_visible();
    if (s_count <= rows) return false;

    *track_y = LIST_TOP;
    *track_h = rows * ROW_H;
    *bar_h = (*track_h * rows) / s_count;
    if (*bar_h < 24) *bar_h = 24;
    *max_top = s_count - rows;
    return true;
}

/* ------------------------------------------------------------------ */
/* Directory listing                                                   */
/* ------------------------------------------------------------------ */

static void entries_free(void)
{
    for (int i = 0; i < s_count; i++) {
        free(s_entries[i].name);
        free(s_entries[i].label);
        s_entries[i].label = NULL;
    }
    s_count = 0;
    /* The prefix belonged to those names. A volume going away without a
     * reload -- which is what calls this -- would otherwise leave the
     * header claiming an elision over an empty list. */
    s_prefix[0] = '\0';
    s_prefix_len = 0;
}

/* Folders first, then files, each run sorted case-insensitively. Mixing
 * them alphabetically buries a disc subfolder in the middle of the track
 * list, and the two are different kinds of thing to tap. */
static int cmp_entry(const void *a, const void *b)
{
    const entry_t *x = a, *y = b;
    if (x->is_dir != y->is_dir) return x->is_dir ? -1 : 1;
    return strcasecmp(x->name, y->name);
}

/* Defined below, next to the rules it enforces; called from the bottom
 * of load_dir() because it can only run once the listing is complete
 * and sorted. */
static void find_common_prefix(void);

/*
 * Local stars, resolved per row when the list is built -- the reason
 * entry_t.fav exists, and the same once-per-load rule the radio rows
 * follow. The first call for a volume reads its starred.m3u, which is
 * I/O on the task that is already reading the directory.
 */
static void stars_resolve(void)
{
    static char full[512];                  /* not the stack */
    for (int i = 0; i < s_count; i++) {
        s_entries[i].fav =
            storage_join_path(full, sizeof(full), s_dir, s_entries[i].name) &&
            starred_contains(full, s_entries[i].is_dir);
    }
}

/*
 * 5041: one listing at a time.
 *
 * load_dir() is called from two tasks. At boot the decode task's resume
 * reopens the chooser on the track's folder (browser_open(), from
 * restore_last_track()) while ui_task's draw sees the card's mount and
 * loads the root of the tab. Each empties the list, sets s_dir and
 * appends -- so run together they left the Selections folder's path over
 * the root's folders and the Selections files, one list, and a tap on
 * test_audio_files opened /sd/Selections_.../test_audio_files:
 *
 *   button: row 1 (dir) "test_audio_files"
 *   cannot open /sd/Selections_from_the_2005-2006_Season-12519/test_audio_files
 *
 * The lock makes each listing whole; the draw-side loads also re-check
 * under it, so a draw that queued behind browser_open() does not then
 * replace the folder that was just opened with the root.
 */
static SemaphoreHandle_t load_lock(void)
{
    static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
    static DRAM_ATTR StaticSemaphore_t buf;   /* internal: see psram_bss.lf */
    static SemaphoreHandle_t h;
    taskENTER_CRITICAL(&mux);
    if (!h) h = xSemaphoreCreateMutexStatic(&buf);
    taskEXIT_CRITICAL(&mux);
    return h;
}

static void load_dir_locked(const char *dir);

static void load_dir(const char *dir)
{
    xSemaphoreTake(load_lock(), portMAX_DELAY);
    load_dir_locked(dir);
    xSemaphoreGive(load_lock());
}

/*
 * 5186: a stored playlist's folder -- "<mount>/Playlists", where MPD's
 * `save` writes (mpd.c, 5184). Its .m3u files are listed there, and only
 * there: stations.m3u and starred.m3u at a card's root are the player's
 * own, and a tap on one would load stations or stars as a queue.
 */
static bool is_playlists_dir(const char *dir)
{
    return strcmp(dir, STORAGE_SD_MOUNT "/Playlists") == 0 ||
           strcmp(dir, STORAGE_USB_MOUNT "/Playlists") == 0;
}

bool browser_is_m3u(const char *name)
{
    return m3u_is_name(name);           /* 5198: and .m3u8 */
}

/*
 * 8021: a folder of downloaded episodes leads back to their podcast.
 * When `dir` holds PODPATH_LINK, a row goes in first -- a folder, its
 * label "<podcast> on Podcast Index" -- and s_dir_link says row 0 is it.
 * Its name is the link file's, which is never opened as a track: the tap
 * is caught before anything joins it to s_dir.
 */
static void pod_link_row(const char *dir)
{
    s_dir_link = false;
    char p[400], text[200];
    if (snprintf(p, sizeof(p), "%s/" PODPATH_LINK, dir) >= (int)sizeof(p)) return;
    FILE *f = fopen(p, "r");
    if (!f) return;
    const size_t n = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[n] = '\0';
    if (!podpath_link_parse(text, n, &s_dir_link_feed, s_dir_link_show, sizeof(s_dir_link_show))) return;
    if (s_count >= MAX_ENTRIES) return;
    memmove(&s_entries[1], &s_entries[0], (size_t)s_count * sizeof(entry_t));
    char label[PODFEEDS_TITLE_MAX + 48];
    snprintf(label, sizeof(label), _("%s on Podcast Index"),
             s_dir_link_show[0] ? s_dir_link_show : same("Podcast"));
    s_entries[0] = (entry_t){ 0 };
    s_entries[0].name = strdup(PODPATH_LINK);
    s_entries[0].label = strdup(label);
    s_entries[0].is_dir = true;
    if (!s_entries[0].name || !s_entries[0].label) {
        free(s_entries[0].name);
        free(s_entries[0].label);
        memmove(&s_entries[0], &s_entries[1], (size_t)s_count * sizeof(entry_t));
        return;
    }
    s_count++;
    s_dir_link = true;
}

static void load_dir_locked(const char *dir)
{
    if (!s_entries) {
        s_entries = heap_caps_calloc(MAX_ENTRIES, sizeof(entry_t),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_entries) {
            ESP_LOGE(TAG, "out of memory for the listing");
            return;
        }
    }

    entries_free();
    s_top = 0;
    snprintf(s_dir, sizeof(s_dir), "%s", dir);
    s_dirty = true;

    DIR *d = opendir(dir);
    if (!d) {
        ESP_LOGW(TAG, "cannot open %s", dir);
        return;
    }

    /* The same cue view the playlist builds, so a row here is a track
     * there. See cuedir.h. */
    cuedir_t *cues = cuedir_load(dir, STORAGE_IO_BACKGROUND);

    const bool lists = is_playlists_dir(dir);   /* 5186 */
    struct dirent *e;
    while ((e = readdir(d)) != NULL && s_count < MAX_ENTRIES) {
        const bool is_dir = (e->d_type == DT_DIR);
        /* Everything the decoder cannot open is hidden rather than
         * greyed. A card root is mostly System Volume Information and
         * stray .txt files, and a list where two thirds of the rows are
         * untappable is a worse list. */
        if (storage_is_hidden(e->d_name)) continue;   /* . .. and dotfiles */
        if (!is_dir && !decoder_supports(e->d_name) && !(lists && browser_is_m3u(e->d_name))) continue;
        if (!is_dir && cuedir_hides(cues, e->d_name)) continue;

        s_entries[s_count].name = strdup(e->d_name);
        if (!s_entries[s_count].name) break;
        s_entries[s_count].is_dir = is_dir;
        s_count++;
    }
    closedir(d);

    for (int i = 0; i < cuedir_count(cues) && s_count < MAX_ENTRIES; i++) {
        s_entries[s_count].name = strdup(cuedir_name(cues, i));
        s_entries[s_count].label = strdup(cuedir_label(cues, i));
        if (!s_entries[s_count].name) break;
        s_entries[s_count].is_dir = false;
        s_count++;
    }
    cuedir_free(cues);

    qsort(s_entries, (size_t)s_count, sizeof(entry_t), cmp_entry);
    pod_link_row(dir);                  /* 8021: before the prefix, which skips labels */
    find_common_prefix();
    stars_resolve();

    /*
     * Dirty again now the rows exist. The flag set at the top is for
     * the path and the tabs; a draw that runs while this is still
     * reading -- browser_open() from the decode task at boot, beside
     * ui_task's draw -- takes it with s_count at 0, paints an empty
     * list, and clears it. That was cheap to lose when a listing was a
     * readdir. With cue sheets it is every sheet in the folder parsed,
     * 100 ms and more, and the list stayed black until a tab change
     * loaded it again.
     */
    s_dirty = true;
}

/*
 * The bit of every filename in this folder that is not telling you
 * anything.
 *
 * Ripped albums come off a CD as
 *
 *   Advent_Chamber_Orchestra_-_04_-_Mozart_-_Eine_Kleine_Nachtmusik.mp3
 *
 * and a folder of them is nine rows that agree for the first
 * twenty-seven characters. At NAME_SCALE the row holds about forty, so
 * the artist and the album spend two thirds of every line repeating
 * what the folder name above them already says, and the part that
 * differs -- the track number and the title, the only reason to look at
 * the list -- is pushed off the right edge and clipped.
 *
 * So it is found once per folder and dropped from the drawing. THE
 * ENTRIES ARE NOT MODIFIED: s_entries[i].name stays the real filename,
 * because it is what gets opened, what is compared against the playing
 * track, and what the playlist is built from. Only the row's starting
 * offset moves.
 *
 * WHERE IT STOPS
 *
 * At a separator, never mid-word. The raw common prefix of 04_ and 05_
 * includes the "0", which would leave rows reading "4_-_Mozart" and
 * "5_-_Handel" -- technically shorter and actively worse, because a
 * truncation that lands inside a number reads as data loss rather than
 * as tidying. Backing up to the last _ - . or space costs a couple of
 * characters and keeps every row starting on something whole.
 *
 * WHEN IT DECLINES
 *
 * Fewer than two files, a prefix shorter than MIN_PREFIX, or any file
 * left with almost nothing after it. That last one is the case that
 * matters: a folder holding 01.mp3 through 09.mp3 has a common prefix
 * of "0", and hiding it turns a legible list into the digits 1 to 9.
 * The point is to remove what is redundant, not to remove as much as
 * possible.
 *
 * Directories are neither counted nor shortened. A subfolder is not part
 * of the album's naming scheme and the prefix rarely applies to it.
 */
#define MIN_PREFIX      (8)     /* below this it is not worth the elision */
#define MIN_REMAINDER   (4)     /* what every row must still have left */

static bool is_sep(char c)
{
    return c == '_' || c == '-' || c == '.' || c == ' ';
}

static void find_common_prefix(void)
{
    s_prefix[0] = '\0';
    s_prefix_len = 0;

    const char *first = NULL;
    int files = 0, lcp = 0;

    for (int i = 0; i < s_count; i++) {
        if (s_entries[i].is_dir || s_entries[i].label) continue;
        const char *n = s_entries[i].name;
        if (!first) { first = n; lcp = (int)strlen(n); files = 1; continue; }
        files++;

        int k = 0;
        while (k < lcp && first[k] && n[k] == first[k]) k++;
        lcp = k;
        if (lcp < MIN_PREFIX) return;   /* cannot recover; nothing to do */
    }

    if (files < 2 || lcp < MIN_PREFIX) return;

    /* Back up to a separator, and include it -- the row should start on
     * the next real character, not on the underscore before it. */
    while (lcp > 0 && !is_sep(first[lcp - 1])) lcp--;
    if (lcp < MIN_PREFIX) return;

    /* Every row has to be left with something worth reading. */
    for (int i = 0; i < s_count; i++) {
        if (s_entries[i].is_dir || s_entries[i].label) continue;
        if ((int)strlen(s_entries[i].name) - lcp < MIN_REMAINDER) return;
    }

    if (lcp >= (int)sizeof(s_prefix)) lcp = (int)sizeof(s_prefix) - 1;
    memcpy(s_prefix, first, (size_t)lcp);
    s_prefix[lcp] = '\0';
    s_prefix_len = lcp;
    ESP_LOGI(TAG, "hiding a shared prefix on %d files: \"%s\"", files, s_prefix);
}

/* Path of the volume root for the active tab. */
static const char *tab_root(void)
{
    /* RADIO has no root. Callers guard on s_radio before using this;
     * returning "" rather than NULL keeps a stray use from crashing. */
    if (s_tab == BROWSER_TAB_RADIO || s_tab == BROWSER_TAB_PODCAST) return "";   /* 8008 */
    return storage_mount_path((storage_id_t)s_tab);
}

static bool at_root(void)
{
    return strcmp(s_dir, tab_root()) == 0;
}

static void go_up(void)
{
    if (at_root()) return;
    char up[512];
    snprintf(up, sizeof(up), "%s", s_dir);
    char *slash = strrchr(up, '/');
    if (!slash) return;
    if (slash == up) return;
    *slash = '\0';
    load_dir(up);
}

/*
 * An absent volume's tab is still selectable, which is the one place
 * this differs from a normal tab strip. The grey means "nothing here
 * yet", not "not a button".
 *
 * It used to mean more than that: selecting the USB tab was what powered
 * the port, so the tab HAD to be selectable or there was no way to ask.
 * The port comes up at boot now -- a USB audio device cannot announce
 * itself through a dark port, and waiting for someone to open the
 * chooser was never a sensible gate on that -- so this is back to being
 * an ordinary reason rather than a load-bearing one. The tab stays
 * selectable anyway: a tab that ignores taps while the drive spins up
 * reads as a lost tap.
 */
/* A tab has something in it. For the volumes that is a mount; for RADIO
 * it is a station list that has been read. */
static bool tab_present(browser_tab_t t)
{
    if (t == BROWSER_TAB_RADIO) return stations_count() > 0;
    if (t == BROWSER_TAB_PODCAST) return podindex_state(NULL, 0, NULL) == PODINDEX_WORKING;  /* 8008 */
    return storage_present((storage_id_t)t);
}

/*
 * Fill the rows from the menu table.
 *
 * is_dir is true for every row, and that is not a lie told to get a
 * folder icon: each one opens into a list of things to play, which is
 * what is_dir means everywhere else in this file. The card row included
 * -- it opens into the card's stations.
 */
static void load_radio_menu(void)
{
    entries_free();
    if (!s_entries) {
        s_entries = heap_caps_calloc(MAX_ENTRIES, sizeof(entry_t),
                                     MALLOC_CAP_SPIRAM);
        if (!s_entries) {
            ESP_LOGE(TAG, "no PSRAM for the radio menu");
            return;
        }
    }
    for (int i = 0; i < RADIOBROWSER_MENU_ROWS && s_count < MAX_ENTRIES; i++) {
        s_entries[s_count].name = strdup(_(radiobrowser_menu_label(i)));
        if (!s_entries[s_count].name) break;
        s_entries[s_count].is_dir = true;
        s_count++;
    }
    s_top = 0;
    s_dirty = true;
}

void browser_set_radio_busy(bool busy)
{
    if (busy == s_radio_busy) return;
    s_radio_busy = busy;
    s_dirty = true;
}

void browser_set_radio_status(const char *line)
{
    snprintf(s_radio_status, sizeof(s_radio_status), "%s", line ? line : "");
    s_dirty = true;
}

/* 5063: defined with browser_set_station() below; declared here because
 * load_stations() re-finds the playing row by URL. */
static int  s_playing_station;
static char s_playing_url[STATION_URL_MAX];

/* Fill the rows from the station list. Same entry_t the files use, so
 * everything downstream of here is unchanged. */
static void load_stations(void)
{
    entries_free();
    if (!s_entries) {
        s_entries = heap_caps_calloc(MAX_ENTRIES, sizeof(entry_t),
                                     MALLOC_CAP_SPIRAM);
        if (!s_entries) {
            ESP_LOGE(TAG, "no PSRAM for the station rows");
            return;
        }
    }
    const int n = stations_count();
    int found = -1;     /* 5063: the playing station's row in THIS list */
    for (int i = 0; i < n && s_count < MAX_ENTRIES; i++) {
        station_t st;
        if (!stations_get(i, &st)) continue;
        s_entries[s_count].name = strdup(st.name);
        if (!s_entries[s_count].name) break;
        s_entries[s_count].is_dir = false;
        s_entries[s_count].fav = favorites_contains(st.url);
        if (found < 0 && s_playing_url[0] && strcmp(st.url, s_playing_url) == 0) {
            found = s_count;
        }
        s_count++;
    }
    s_playing_station = found;
    /* NOT sorted. The file's order is the listener's order -- it is what
     * next and previous move through, and what stations_index() counts
     * in. Sorting the display would make the first row and the first
     * station different things. */
    s_top = 0;
    s_dirty = true;
    ESP_LOGI(TAG, "radio: %d stations", s_count);
}

/*
 * The list has been re-read. Same rows, rebuilt.
 *
 * load_stations() already does everything needed -- it frees the rows,
 * refills them from stations.c in the file's order, resets the scroll and
 * marks the screen dirty -- so this is a guard and a call. The guard is
 * the whole of what is new: this arrives asynchronously, from a load
 * another task performed, and by then the tab showing may not be RADIO
 * any more.
 */
void browser_stations_reloaded(void)
{
    if (!s_open || !s_radio) return;
    /*
     * A list arriving is what opens the second level. Both ways in lead
     * here -- the card row and a directory row -- so there is one place
     * that decides the tab is showing stations, rather than one per
     * request kind.
     */
    s_radio_menu = false;
    s_radio_status[0] = '\0';
    load_stations();
}

void browser_stars_changed(void)
{
    if (!s_open || s_radio) return;
    stars_resolve();
    s_dirty = true;
}

/* 8014: the PODCAST tab's rows, from podindex.c's list. Folders: each
 * will open into the podcasts in that category. */
static void load_pod_rows(void)
{
    entries_free();
    if (!s_entries) {
        s_entries = heap_caps_calloc(MAX_ENTRIES, sizeof(entry_t), MALLOC_CAP_SPIRAM);
        if (!s_entries) {
            ESP_LOGE(TAG, "no PSRAM for the category rows");
            return;
        }
    }
    (void)podindex_cats(s_pod_cats, PODCATS_MAX, &s_pod_n, &s_pod_gen);
    for (int i = 0; i < s_pod_n && s_count < MAX_ENTRIES; i++) {
        s_entries[s_count].name = strdup(s_pod_cats[i].name);
        if (!s_entries[s_count].name) break;
        s_entries[s_count].is_dir = true;
        s_count++;
    }
    s_top = 0;
    s_dirty = true;
}

/* 8018: the podcast rows, from podindex.c's list for s_pod_cat. */
static void load_pod_feed_rows(void)
{
    entries_free();
    if (!s_entries) {
        s_entries = heap_caps_calloc(MAX_ENTRIES, sizeof(entry_t), MALLOC_CAP_SPIRAM);
        if (!s_entries) {
            ESP_LOGE(TAG, "no PSRAM for the podcast rows");
            return;
        }
    }
    (void)podindex_feeds(s_pod_feeds, PODFEEDS_MAX, &s_pod_fn, NULL, &s_pod_fgen);
    for (int i = 0; i < s_pod_fn && s_count < MAX_ENTRIES; i++) {
        s_entries[s_count].name = strdup(s_pod_feeds[i].title);
        if (!s_entries[s_count].name) break;
        s_entries[s_count].is_dir = true;       /* each opens into its episodes */
        s_count++;
    }
    s_top = 0;
    s_dirty = true;
}

/* 8019: the episode rows, from podindex.c's list for s_pod_feed. */
static void load_pod_ep_rows(void)
{
    entries_free();
    if (!s_entries) {
        s_entries = heap_caps_calloc(MAX_ENTRIES, sizeof(entry_t), MALLOC_CAP_SPIRAM);
        if (!s_entries) {
            ESP_LOGE(TAG, "no PSRAM for the episode rows");
            return;
        }
    }
    (void)podindex_eps(s_pod_eps, PODEPS_MAX, &s_pod_en, NULL, &s_pod_egen);
    for (int i = 0; i < s_pod_en && s_count < MAX_ENTRIES; i++) {
        s_entries[s_count].name = strdup(s_pod_eps[i].title);
        if (!s_entries[s_count].name) break;
        s_entries[s_count].is_dir = false;      /* each is a thing to play */
        s_count++;
    }
    /* 8023: "Music Makers San Diego 203 - Dave Preston", forty times over:
     * the same elision a folder of files gets, and the same note under
     * the status line saying what came off the front. Episode rows are
     * files to it -- not folders, no label -- so nothing else changes. */
    find_common_prefix();
    s_top = 0;
    s_dirty = true;
}

/* 8020: the episode's folder and file name on `vol`. */
static bool pod_ep_where(storage_id_t vol, int ep, char *dir, size_t dsz, char *base, size_t bsz)
{
    if (!podpath_dir(storage_mount_path(vol), s_pod_show, dir, dsz)) return false;
    podpath_base(s_pod_eps[ep].published, s_pod_eps[ep].title, base, bsz);
    return true;
}

/* 8020: download episode `ep` to `vol`; back to the episode rows. */
static void pod_download(int ep, storage_id_t vol)
{
    static char dir[400], base[PODPATH_NAME_MAX + 16];
    if (!pod_ep_where(vol, ep, dir, sizeof(dir), base, sizeof(base))) {
        ESP_LOGW(TAG, "episode %lld: the path is too long", (long long)s_pod_eps[ep].id);
        return;
    }
    if (!poddl_start(s_pod_eps[ep].id, s_pod_eps[ep].url, dir, base, s_pod_cur, s_pod_show,
                     s_pod_eps[ep].feed_image, s_pod_eps[ep].image))                    /* 8027 */
        ESP_LOGI(TAG, "episode %lld: another download is running", (long long)s_pod_eps[ep].id);
}

/* 8024: bytes as "12.3 MB", tenths of a MiB, which is what a card's
 * free space and a file manager say. */
static void fmt_mb(int64_t b, char *out, size_t cap)
{
    const int64_t t = b * 10 >> 20;
    snprintf(out, cap, "%lld.%lld MB", (long long)(t / 10), (long long)(t % 10));
}

/* 8024: seconds as "1:02:03" or "2:03". */
static void fmt_len(int32_t s, char *out, size_t cap)
{
    if (s >= 3600) snprintf(out, cap, "%d:%02d:%02d", (int)(s / 3600), (int)(s / 60 % 60), (int)(s % 60));
    else           snprintf(out, cap, "%d:%02d", (int)(s / 60), (int)(s % 60));
}

/* 8024: the actions for the episode being downloaded. */
static void load_pod_act_rows(void)
{
    entries_free();
    const poddl_state_t ds = poddl_state(NULL, NULL, NULL, NULL, 0, NULL);
    const char *rows[2] = { ds == PODDL_RUNNING ? _("Pause the download") : _("Go on with the download"),
                            _("Cancel the download and delete it") };
    for (int i = 0; i < 2 && s_count < MAX_ENTRIES; i++) {
        s_entries[s_count].name = strdup(rows[i]);
        if (!s_entries[s_count].name) break;
        s_entries[s_count].is_dir = true;
        s_count++;
    }
    s_top = 0;
    s_dirty = true;
}

/* 8020: the drive picker's rows, one per volume that is in. 8026: the
 * episode's page, these rows over its description, for one drive too. */
static void load_pod_pick_rows(void)
{
    entries_free();
    for (int i = 0; i < s_pod_nvols && s_count < MAX_ENTRIES; i++) {
        char row[64];
        snprintf(row, sizeof(row), _("Download to %s"), storage_label(s_pod_vols[i]));
        s_entries[s_count].name = strdup(row);
        if (!s_entries[s_count].name) break;
        s_entries[s_count].is_dir = true;
        s_count++;
    }
    s_top = 0;
    s_dirty = true;
}

/* 8019: ask for a podcast's episodes, waiting as pod_fetch_feeds() does. */
static void pod_fetch_eps(void)
{
    s_pod_wait = !net_online() || !podindex_eps_fetch(s_pod_cur);          /* 8021: by id */
}

/* 8018: ask for a category's podcasts now, or when there is a network. */
static void pod_fetch_feeds(void)
{
    /* Refused while another fetch runs: wait, and browser_poll() asks
     * again, rather than show "nothing trending" for a list never asked. */
    s_pod_wait = !net_online() || !podindex_feeds_fetch(s_pod_cats[s_pod_cat].id);
}

static void select_tab(browser_tab_t id)
{
    const bool same = (id == s_tab);
    s_tab = id;
    s_pod = false;                      /* 8008 */

    if (id == BROWSER_TAB_PODCAST) {
        s_radio = false;
        s_pod = true;
        s_pod_seen = podindex_state(NULL, 0, NULL);
        s_pod_level = 0;                /* 8018: the tab opens on the categories */
        entries_free();
        s_prefix[0] = '\0';
        s_prefix_len = 0;
        s_dir[0] = '\0';
        s_dirty = true;
        /* 8014: the categories, fetched the first time the tab is opened
         * with a working key and again on a tap after a failure -- the
         * status line says to tap. */
        if (s_pod_seen == PODINDEX_WORKING) {
            load_pod_rows();
            const podindex_cats_t cs = podindex_cats(NULL, 0, NULL, NULL);
            s_pod_wait = false;
            if (cs == PODINDEX_CATS_NONE || cs == PODINDEX_CATS_FAILED) {
                /* 8016: no address yet is waiting, not failing. */
                if (net_online()) (void)podindex_cats_fetch();
                else              s_pod_wait = true;
            }
        }
        return;
    }

    if (id == BROWSER_TAB_RADIO) {
        s_radio = true;
        /* The prefix elision is a filename thing and station names are
         * not filenames. Left set, "all start with" would be computed
         * from the previous folder and applied to station rows. */
        s_prefix[0] = '\0';
        s_prefix_len = 0;
        s_dir[0] = '\0';
        s_radio_status[0] = '\0';
        s_radio_menu = true;
        load_radio_menu();
        return;
    }

    s_radio = false;

    if (!storage_present((storage_id_t)id)) {
        entries_free();
        s_dir[0] = '\0';
        s_dirty = true;
        return;
    }

    if (same && s_count > 0) return;
    load_dir(storage_mount_path((storage_id_t)id));
}

void browser_open(const char *start)
{
    s_open = true;
    s_was_down = false;
    /* A drag cannot survive the screen it was on. Left set, the first
     * press after reopening would be treated as the continuation of a
     * gesture that ended somewhere else. */
    s_scroll_drag = false;
    s_dirty = true;
    s_seen_generation = storage_generation();
    s_seen_rec_files = recorder_files_changed();        /* 5215 */

    /*
     * A VOLUME TAB, WHICH MEANS NOT THE RADIO TAB.
     *
     * Every path below sets s_tab to a storage id or leaves both tabs
     * grey with no rows; none of them can open on RADIO. But none of
     * them went through select_tab() either, and select_tab() is the
     * only thing that had ever cleared s_radio -- so once the radio tab
     * had been visited, every REOPEN of the chooser showed a volume's
     * files with s_radio still true.
     *
     * The board found it and the log is unambiguous:
     *
     *   button: row 3 (station) "Advent_Chamber_Orchestra_-_04_-_Mozart..."
     *   station 4 of 4: ice1.somafm.com
     *
     * A file row, named as a station, playing whatever station happened
     * to sit at that index -- and for rows past the end of a four-entry
     * station list, playing nothing at all while the press logged as
     * received. The footer's slot 0 was RLOD over a directory too, and
     * `radio: 4 stations` was drawn over a nine-file listing.
     *
     * Reset here rather than by routing browser_open() through
     * select_tab(): that function loads, and this one has three
     * different ideas about what to load and which volume to load it
     * from. What was missing was one line of state, and this is that
     * line.
     */
    s_radio = false;
    s_pod = false;                      /* 8008: likewise */

    /* Reopen where the current track lives, when that volume is still
     * there. Coming back to the root of the card every time is the thing
     * that makes a chooser tedious on an album you are picking through. */
    storage_id_t want = STORAGE_COUNT;
    if (start && *start) want = storage_of_path(start);

    if (want < STORAGE_COUNT && storage_present(want)) {
        s_tab = want;
        char dir[512];
        snprintf(dir, sizeof(dir), "%s", start);
        char *slash = strrchr(dir, '/');
        if (slash && slash != dir) *slash = '\0';
        load_dir(dir);
        return;
    }

    for (int i = 0; i < STORAGE_COUNT; i++) {
        if (storage_present((storage_id_t)i)) {
            s_tab = (storage_id_t)i;
            load_dir(storage_mount_path(s_tab));
            return;
        }
    }

    /*
     * 5065: nothing mounted, so the radio. It used to be both volume tabs
     * grey with no rows, and on a Tab5 with no card or drive that was the
     * whole boot screen: two taps (RADIO, then the kept list) before
     * anything could be chosen, every time. The radio tab is the one
     * thing that works without storage, and since 5048 there is a kept
     * list for it to show. A drive that mounts afterwards does not move
     * the view -- mount changes leave the radio tab alone -- and its tab
     * is one tap away.
     */
    select_tab(BROWSER_TAB_RADIO);
}

void browser_close(void)
{
    s_open = false;
    entries_free();
}

/* ------------------------------------------------------------------ */
/* Drawing                                                             */
/* ------------------------------------------------------------------ */

static const char *status_line(void)
{
    if (s_pod) {                        /* 8008 */
        const podindex_state_t st = podindex_state(NULL, 0, NULL);
        if (st == PODINDEX_WORKING && s_pod_level == 3)        /* 8020 */
            return s_pod_act ? _("this episode's download")
                 : s_pod_nvols == 0 ? _("no card or USB drive to download to")     /* 8026 */
                 : s_pod_pick < s_pod_en ? s_pod_eps[s_pod_pick].title : "";
        if (st == PODINDEX_WORKING && s_pod_level == 2) {      /* 8019 */
            static char eline[96];
            const podindex_cats_t es = podindex_eps(NULL, 0, NULL, NULL, NULL);
            if (s_pod_wait) return _("podcasts - waiting for a network");
            if (s_pod_note) return s_pod_note;          /* 8020 */
            /* 8020: a download of one of these episodes, ahead of the count. */
            {
                int64_t id = 0, got = 0, total = 0;
                const poddl_state_t ds = poddl_state(&id, &got, &total, NULL, 0, NULL);
                bool mine = false;
                for (int k = 0; k < s_pod_en && !mine; k++) mine = s_pod_eps[k].id == id;
                /* 8024: sizes, not a percentage; and a tap on the episode
                 * gives its pause and cancel. */
                char a[24], b[24];
                fmt_mb(got, a, sizeof(a));
                fmt_mb(total, b, sizeof(b));
                if (mine && (ds == PODDL_RUNNING || ds == PODDL_PAUSED)) {
                    const char *fmt = ds == PODDL_PAUSED
                        ? (total > 0 ? _("paused - %s of %s") : _("paused - %s"))
                        : (total > 0 ? _("downloading - %s of %s") : _("downloading - %s"));
                    snprintf(eline, sizeof(eline), fmt, a, b);
                    return eline;
                }
                if (mine && ds == PODDL_DONE) return _("downloaded - tap it again to play");
                if (mine && ds == PODDL_FAILED) {
                    snprintf(eline, sizeof(eline), _("stopped at %s - tap it to go on"), a);
                    return eline;
                }
            }
            if (es == PODINDEX_CATS_LOADING) return _("podcasts - asking Podcast Index for the episodes");
            if (es == PODINDEX_CATS_FAILED && s_count == 0)
                return _("episodes - Podcast Index did not answer; UP, then try again");
            if (s_count == 0) return _("no episodes listed");
            snprintf(eline, sizeof(eline), _p("%d episodes, newest first", s_count), s_count);
            return eline;
        }
        if (st == PODINDEX_WORKING && s_pod_level == 1) {      /* 8018 */
            static char fline[160];
            const char *cat = s_pod_cats[s_pod_cat].name;
            const podindex_cats_t fs = podindex_feeds(NULL, 0, NULL, NULL, NULL);
            if (s_pod_wait) return _("podcasts - waiting for a network");
            if (fs == PODINDEX_CATS_LOADING)
                snprintf(fline, sizeof(fline), _("podcasts - asking Podcast Index for %s"), cat);
            else if (fs == PODINDEX_CATS_FAILED && s_count == 0)
                snprintf(fline, sizeof(fline), _("%s - Podcast Index did not answer; UP, then try again"), cat);
            else if (s_count > 0)
                snprintf(fline, sizeof(fline), _p("%d podcasts in %s", s_count), s_count, cat);
            else
                snprintf(fline, sizeof(fline), _("nothing trending in %s"), cat);
            return fline;
        }
        if (st == PODINDEX_WORKING) {               /* 8014 */
            static char line[96];
            const podindex_cats_t cs = podindex_cats(NULL, 0, NULL, NULL);
            if (s_pod_wait) return _("podcasts - waiting for a network");          /* 8016 */
            if (cs == PODINDEX_CATS_LOADING) return _("podcasts - asking Podcast Index for its categories");
            if (cs == PODINDEX_CATS_FAILED && s_count == 0)
                return _("podcasts - Podcast Index did not answer; tap PODCAST to try again");
            if (s_count > 0) {
                snprintf(line, sizeof(line), _p("%d podcast categories", s_count), s_count);
                return line;
            }
        }
        return st == PODINDEX_WORKING  ? _("podcasts - Podcast Index key saved")
             : st == PODINDEX_CHECKING ? _("podcasts - checking the Podcast Index key")
                                       : _("podcasts need a Podcast Index key");
    }
    if (s_radio) {
        static char line[96];
        /* Whatever the player task last said, ahead of everything: a
         * fetch in flight or a fetch that failed is the only thing on
         * this tab worth the row while it is true. */
        if (s_radio_status[0]) return s_radio_status;
        if (s_radio_menu) return _("radio - the card, the charts, or a tag");
        const int n = stations_count();
        if (n <= 0) {
            /* Names the file, because the fix is to make one. */
            snprintf(line, sizeof(line), _("no %s on the card"), STATIONS_FILENAME);
            return line;
        }
        snprintf(line, sizeof(line), _p("%d stations from %s", n), n, stations_source());
        return line;
    }
    if (s_dir[0]) return s_dir;
    if (s_tab == BROWSER_TAB_USB) {
        /* The false branch is a few milliseconds of bring-up at boot,
         * not a state anyone can tap their way into any more. */
        return storage_usb_powered() ? _("USB port on - waiting for a drive")
                                     : _("USB port coming up");
    }
    return _("no card in the slot");
}

static void draw_tab(browser_tab_t id, int x, int w)
{
    const bool present = tab_present(id);
    /* Selected is selected even with nothing mounted: the USB tab can be
     * the active one while the port is still coming up, and a strip with
     * no underline at all would read as a lost tap. */
    const bool active = (id == s_tab);

    gfx_fill_rect(x, 0, w, TAB_H, active ? C_TAB_ON : C_TAB_OFF);
    if (active) gfx_fill_rect(x, TAB_H - 5, w, 5, present ? C_ACCENT : C_DISABLED);

    /* storage_label() reports the volume's own label -- "SD8G" -- which
     * RADIO has no equivalent of and should not borrow. */
    const char *label = (id == BROWSER_TAB_RADIO)   ? same("RADIO")
                      : (id == BROWSER_TAB_PODCAST) ? same("PODCAST")        /* 8008 */
                                                    : storage_label((storage_id_t)id);
    const int tw = gfx_text_w(label, NAME_SCALE);
    gfx_draw_text(x + (w - tw) / 2, (TAB_H - GFX_GLYPH_H(NAME_SCALE)) / 2, label,
                  NAME_SCALE, w - 16,
                  present ? (active ? C_TEXT : C_DIM) : C_DISABLED);
}

/* A folder, drawn the same way ui.c draws the one on the transport bar so
 * the button and the rows it produces are visibly the same idea. */
static void draw_folder_icon(int cx, int cy, uint16_t c)
{
    gfx_fill_rect(cx - 20, cy - 14, 16, 5, c);
    gfx_fill_rect(cx - 20, cy - 9, 40, 25, c);
    gfx_fill_rect(cx - 16, cy - 5, 32, 17, C_ROW);
}

/*
 * The star: filled when starred, a ring when not.
 *
 * IT USED TO BE TWO OVERLAPPING TRIANGLES, which is a hexagram -- six
 * points -- while the comment above it said "five-pointed". No pair of
 * triangles makes a five-pointed star; it is a ten-vertex polygon, and
 * gfx_fill_star() draws one.
 *
 * The unfilled case is the same star again at 58% in the row's own
 * colour, which leaves a rim. `bg` and not C_ROW: the rows alternate
 * C_ROW and C_ROW_ALT, and a hole punched in the wrong one is a star
 * with a shadow on every second line.
 *
 * 58% rather than the 60% the triangles used because a star's arms are
 * thinner than a triangle's edge at the same fraction, and at r=18 the
 * rim wants the two or three pixels this leaves it.
 */
static void draw_star(int cx, int cy, int r, bool filled, uint16_t c,
                      uint16_t bg)
{
    gfx_fill_star(cx, cy, r, c);
    if (!filled) gfx_fill_star(cx, cy, (r * 58) / 100, bg);
}

/* A note: stem and head. Two rectangles and a circle is enough to read as
 * "this one is a track" at a glance, which is all the icon is for. */
static void draw_note_icon(int cx, int cy, uint16_t c)
{
    gfx_fill_rect(cx + 6, cy - 16, 4, 24, c);
    gfx_fill_rect(cx + 6, cy - 16, 14, 5, c);
    gfx_fill_circle(cx + 1, cy + 9, 7, c);
}

/*
 * The track being heard, as told by the player. See browser_set_playing().
 *
 * Held here rather than asked for, because the answer is not derivable
 * on this side: playlist_current() is where the DECODER is, and at a
 * boundary that is twenty seconds ahead of the speaker. 0513 dirtied
 * the list when that changed, which worked and was too early by exactly
 * one ring.
 */
static char s_playing[512];

/*
 * The station being heard, as an index, or -1 for none.
 *
 * An index and not a name, because a station list is a position: two
 * entries with the same name are two stations and stations_index() is
 * what next and previous move through. Same reason BROWSER_PLAY_STREAM
 * hands back an index rather than a URL.
 *
 * Told rather than read from stations_index(), which is always set to
 * something: it keeps the last choice after a stream ends, so reading it
 * directly would leave a station marked as playing when nothing is.
 * That distinction is the player's to make and this is how it says it --
 * exactly as s_playing works for files.
 */
static int s_playing_station = -1;

/*
 * 5063: AND ITS URL, because the index belongs to the list it was chosen
 * from. BBC World Service played as station 2 of 2 from the kept list;
 * opening "news", where it is row 0, left the marker on row 1 -- the
 * index carried into a list it was never an index of. load_stations()
 * finds the row again by this. The scratch is module scope, not a
 * local: a station_t is over 600 bytes (CLAUDE.md), and this runs on the
 * player task.
 */
static char      s_playing_url[STATION_URL_MAX];
static station_t s_set_scratch;

void browser_set_station(int index)
{
    if (index >= 0 && stations_get(index, &s_set_scratch)) {
        snprintf(s_playing_url, sizeof(s_playing_url), "%s", s_set_scratch.url);
    } else {
        s_playing_url[0] = '\0';
    }
    if (index == s_playing_station) return;
    s_playing_station = index;
    s_dirty = true;     /* same reason as browser_set_playing()'s */
}

void browser_set_playing(const char *path)
{
    const char *p = (path && *path) ? path : "";
    if (strcmp(s_playing, p) == 0) return;

    snprintf(s_playing, sizeof(s_playing), "%s", p);

    /* The redraw goes with it. Every other thing that moves the marker
     * is a press, which dirties the list on its own; this one arrives
     * from another task and has nothing else to ride on. */
    s_dirty = true;
}

/* Is this row the track that is playing? Compared as a whole path: a
 * same-named track in a different folder is a different track, and on a
 * card full of "01 Intro.mp3" that is not a hypothetical. */
static bool is_current(const char *name)
{
    const char *path = s_playing[0] ? s_playing : NULL;
    if (!path || !s_dir[0]) return false;

    char full[512];
    /* A name too long to join is a name that cannot be the current track
     * either, since the current track was opened through the same join. */
    if (!storage_join_path(full, sizeof(full), s_dir, name)) return false;
    return strcmp(full, path) == 0;
}

static void draw_button(int x, int y, int w, int h, const char *label, bool on)
{
    gfx_fill_rect(x, y, w, h, on ? C_BTN : C_TAB_OFF);
    const int tw = gfx_text_w(label, LABEL_SCALE);
    gfx_draw_text(x + (w - tw) / 2, y + (h - GFX_GLYPH_H(LABEL_SCALE)) / 2, label,
                  LABEL_SCALE, w - 8, on ? C_TEXT : C_DISABLED);
}

static const char *order_label(void)
{
    switch (s_order) {
    /* 6019: N_() -- the log prints these; the button passes them
     * through _(). */
    case PLAY_ORDER_ONE:     return N_("ONE");
    case PLAY_ORDER_SHUFFLE: return N_("RND");
    case PLAY_ORDER_REPEAT_ONE: return N_("RPT1");     /* 5261: was RPT */
    case PLAY_ORDER_REPEAT_ALL: return N_("RPT");      /* 5261 */
    case PLAY_ORDER_EAT:     return N_("EAT");          /* 5252 */
    default:                 return N_("ALL");
    }
}

/* 5260: the button's label, marked for one song while a client's
 * `consume oneshot` or `single oneshot` stands. order_label() stays the
 * bare name, for the log lines. */
static const char *order_button_label(void)
{
    if (!s_seen_oneshot) return order_label();
    switch (s_order) {
    case PLAY_ORDER_ONE:     return N_("ONE1");     /* 5261: was ONE·1 */
    case PLAY_ORDER_EAT:     return N_("EAT1");     /* 5261: was EAT·1 */
    default:                 return order_label();
    }
}

/* Six buttons across the bottom, equal width. The arithmetic is done from
 * the panel width rather than written out, because a 720 px panel divides
 * evenly and nothing here should assume that twice. */
#define FOOT_BUTTONS (6)

static void foot_box(int i, int *x, int *w)
{
    const int bw = gfx_w() / FOOT_BUTTONS;
    *x = i * bw;
    *w = (i == FOOT_BUTTONS - 1) ? (gfx_w() - *x) : bw;
}

/*
 * 5215: a Recordings folder re-read in place when the recorder has
 * finished or renamed a file in it. The tapped row is where the finger
 * left it: s_top survives the reload (load_dir() resets it), clamped if
 * the list got shorter. Only a folder called Recordings, which is the
 * only place the recorder writes -- any other folder would be re-read
 * for nothing.
 */
static bool is_recordings_dir(const char *dir)
{
    const char *slash = strrchr(dir, '/');
    return slash && strcmp(slash + 1, "Recordings") == 0;
}

static void recordings_refresh(void)
{
    const uint32_t g = recorder_files_changed();
    if (g == s_seen_rec_files) return;
    s_seen_rec_files = g;
    if (s_radio || !s_dir[0] || !is_recordings_dir(s_dir)) return;

    static char dir[sizeof(s_dir)];     /* load_dir() writes s_dir */
    snprintf(dir, sizeof(dir), "%s", s_dir);
    const int top = s_top;
    load_dir(dir);
    int max_top = s_count - rows_visible();
    if (max_top < 0) max_top = 0;
    s_top = top < max_top ? top : max_top;
    s_dirty = true;
    ESP_LOGI(TAG, "%s re-read: a recording was saved or renamed (%d rows, row %d)",
             dir, s_count, s_top);
}

void browser_draw(void)
{
    if (!s_open) return;
    recordings_refresh();                               /* 5215 */

    /* 5260: a oneshot set or ended changes the order button's label and
     * need not change the order -- consume oneshot on top of consume. */
    if (mpd_oneshot() != s_seen_oneshot) {
        s_seen_oneshot = !s_seen_oneshot;
        s_dirty = true;
    }

    /* A card going in or a drive coming out while the chooser is up has
     * to be visible without a touch, so the generation counter is the
     * other thing that dirties the screen. */
    const uint32_t gen = storage_generation();
    if (gen != s_seen_generation) {
        s_seen_generation = gen;
        s_dirty = true;
        /*
         * The radio tab has no volume behind it, so none of the
         * mount-chasing below applies to it -- and all of it would be
         * actively wrong: `!storage_present(s_tab)` on
         * BROWSER_TAB_RADIO reads past the end of the volume array, and
         * the adopt-what-appeared branch would silently switch the tab
         * out from under a station list and replace the rows with a
         * directory. The station rows are only rebuilt by select_tab().
         *
         * Scoped rather than an early return: s_dirty has just been set
         * and returning here would leave the repaint a frame late, so
         * the tab strip would not redraw until the next poll.
         */
        if (!s_radio && !s_pod) {           /* 8008: nor the podcast tab */
        /* The tab stays where the user put it. Jumping to whatever else
         * is mounted would undo the tap that powered this port about a
         * second before the drive it was waiting for turned up. */
        if (!storage_present((storage_id_t)s_tab)) {
            if (s_dir[0]) {                 /* it was there and went away */
                entries_free();
                s_dir[0] = '\0';
            } else {
                /*
                 * Never landed anywhere, so there is no tap to respect.
                 *
                 * This is the boot case: the chooser comes up before
                 * anything is mounted, defaults to the SD tab, and with
                 * no card there is nothing behind it. A drive turning
                 * up a second later left the user looking at an empty
                 * SD listing with their music one untapped tab away.
                 *
                 * Adopting the volume that just appeared is not
                 * overriding a choice, because none has been made.
                 */
                xSemaphoreTake(load_lock(), portMAX_DELAY);
                if (!s_dir[0] && !storage_present((storage_id_t)s_tab)) {
                    for (int i = 0; i < STORAGE_COUNT; i++) {
                        if (!storage_present((storage_id_t)i)) continue;
                        s_tab = (browser_tab_t)i;
                        load_dir_locked(tab_root());
                        break;
                    }
                }
                xSemaphoreGive(load_lock());
            }
        } else if (s_count == 0 || !s_dir[0]) {
            xSemaphoreTake(load_lock(), portMAX_DELAY);
            if (s_count == 0 || !s_dir[0]) load_dir_locked(s_dir[0] ? s_dir : tab_root());
            xSemaphoreGive(load_lock());
        }
        }
    }

    /* 5067: the spinner moves on the clock, so a new step is a repaint. */
    const bool spin = (s_radio && s_radio_busy) ||
                      (s_pod && (s_pod_wait ||                                     /* 8016 */
                                 (s_pod_level == 0                                 /* 8018 */
                                  ? podindex_cats(NULL, 0, NULL, NULL)
                                  : s_pod_level == 1                               /* 8019 */
                                  ? podindex_feeds(NULL, 0, NULL, NULL, NULL)
                                  : podindex_eps(NULL, 0, NULL, NULL, NULL)) == PODINDEX_CATS_LOADING ||  /* 8014 */
                                 (s_pod_level == 2 &&                              /* 8020 */
                                  poddl_state(NULL, NULL, NULL, NULL, 0, NULL) == PODDL_RUNNING)));
    if (spin) {
        const uint32_t step = (uint32_t)(esp_timer_get_time() / 100000);
        if (step != s_spin_frame) {
            s_spin_frame = step;
            s_dirty = true;
        }
    }
    /* 8008: a key that starts (or stops) working while the tab is up. */
    if (s_pod) {
        const podindex_state_t st = podindex_state(NULL, 0, NULL);
        if (st != s_pod_seen) { s_pod_seen = st; s_dirty = true; }
        /* 8016: the network the tab was waiting for. */
        if (s_pod_wait && net_online()) {
            s_pod_wait = false;
            if (s_pod_level == 2)      s_pod_wait = !podindex_eps_fetch(s_pod_cur);  /* 8019, 8021 */
            else if (s_pod_level == 1) s_pod_wait = !podindex_feeds_fetch(s_pod_cats[s_pod_cat].id);  /* 8018 */
            else                       (void)podindex_cats_fetch();
            s_dirty = true;
        }
        /* 8018: a category's podcasts arrived, or their state moved. */
        /* 8020: a download moved on. */
        {
            uint32_t g = 0;
            (void)poddl_state(NULL, NULL, NULL, NULL, 0, &g);
            if (g != s_pod_dlgen) { s_pod_dlgen = g; if (s_pod_level == 2) s_dirty = true; }
        }
        /* 8019: a podcast's episodes arrived, or their state moved. */
        if (s_pod_level == 2 && podindex_eps_gen() != s_pod_egen) {
            load_pod_ep_rows();
            s_dirty = true;
        }
        if (s_pod_level == 1 && podindex_feeds_gen() != s_pod_fgen) {
            load_pod_feed_rows();
            s_dirty = true;
        }
        /* 8014: a list arrived, or its state moved. */
        if (s_pod_level == 0 && podindex_cats_gen() != s_pod_gen) {            /* 8018: level 0 */
            if (st == PODINDEX_WORKING) load_pod_rows();
            else s_pod_gen = podindex_cats_gen();
            s_dirty = true;
        }
    }
    if (!s_dirty) return;
    s_dirty = false;

    const int w = gfx_w(), h = gfx_h();
    gfx_fill_rect(0, 0, w, h, C_BG);

    /*
     * Four tabs (8008: PODCAST, beside RADIO), and the arithmetic is
     * written so they tile exactly: the last one takes whatever the
     * divisions left over rather than each being w/4 and leaving a seam
     * of background at the right edge on a width that is not a multiple
     * of four. "PODCAST" at NAME_SCALE is 7 x 21 = 147 px, inside the
     * 180 - 16 a quarter of 720 leaves it.
     */
    {
        const int t1 = w / 4, t2 = (2 * w) / 4, t3 = (3 * w) / 4;
        draw_tab(BROWSER_TAB_SD, 0, t1);
        draw_tab(BROWSER_TAB_USB, t1, t2 - t1);
        draw_tab(BROWSER_TAB_RADIO, t2, t3 - t2);
        draw_tab(BROWSER_TAB_PODCAST, t3, w - t3);
    }

    /* Current directory, tail kept -- the end of the path is the part
     * that says where you are. With nothing mounted it says why, because
     * "no drive" and "no power" are the difference between waiting and
     * tapping. */
    /*
     * One line normally, two when something is being hidden -- and the
     * second line is not optional politeness. A list quietly showing
     * names that are not the names on the card is a list you cannot
     * trust; saying what came off the front makes it an abbreviation
     * rather than a discrepancy.
     *
     * Both fit in PATH_H at LABEL_SCALE without moving LIST_TOP, so the
     * list does not lose a row on folders that happen to be tidy.
     */
    if (s_prefix_len) {
        const int gh = GFX_GLYPH_H(LABEL_SCALE);
        const int pad = (PATH_H - 2 * gh) / 3;
        gfx_draw_text_tail(16, TAB_H + pad, status_line(), LABEL_SCALE,
                           w - 32, C_DIM);

        char note[160];
        snprintf(note, sizeof(note), _("all start with  %s"), s_prefix);
        /* Head kept, not tail: the front of the prefix is what identifies
         * it, and it is the front that the rows are missing. */
        gfx_draw_text(16, TAB_H + 2 * pad + gh, note, LABEL_SCALE,
                      w - 32, C_FAINT);
    } else if (spin) {
        /* 5067: the spinner at the right of the row, the text short of it. */
        const int r = GFX_GLYPH_H(LABEL_SCALE) / 2 + 4;
        gfx_draw_text_tail(16, TAB_H + (PATH_H - GFX_GLYPH_H(LABEL_SCALE)) / 2,
                           status_line(), LABEL_SCALE, w - 48 - 2 * r, C_DIM);
        gfx_draw_spinner(w - 16 - r, TAB_H + PATH_H / 2, r,
                         (uint32_t)(esp_timer_get_time() / 1000), C_TEXT, C_FAINT);
    } else {
        gfx_draw_text_tail(16, TAB_H + (PATH_H - GFX_GLYPH_H(LABEL_SCALE)) / 2,
                           status_line(), LABEL_SCALE, w - 32, C_DIM);
    }
    gfx_fill_rect(0, LIST_TOP - 2, w, 2, C_RULE);

    const int rows = rows_visible();
    /* 8028: the episode being downloaded, or paused, asked once a draw. */
    int64_t dl_id = 0, dl_got = 0, dl_total = 0;
    const poddl_state_t dl_st = (s_pod && s_pod_level == 2)
        ? poddl_state(&dl_id, &dl_got, &dl_total, NULL, 0, NULL) : PODDL_IDLE;
    const bool dl_live = dl_st == PODDL_RUNNING || dl_st == PODDL_PAUSED;
    for (int r = 0; r < rows; r++) {
        const int i = s_top + r;
        const int y = LIST_TOP + r * ROW_H;
        if (i >= s_count) {
            gfx_fill_rect(0, y, w, ROW_H, C_BG);
            continue;
        }
        const uint16_t row_bg = (r & 1) ? C_ROW_ALT : C_ROW;
        gfx_fill_rect(0, y, w, ROW_H, row_bg);

        /*
         * On the radio tab the marker is the station index, because
         * is_current() cannot answer there: it joins s_dir with the row
         * name and compares against a file path, and s_dir is empty on
         * RADIO -- so it returned false for every station and NOTHING
         * was ever marked. A file was correctly highlighted on the SD
         * tab in the same session, which is what made it look like the
         * marker working rather than the marker being absent.
         *
         * Row i IS station i: load_stations() builds the rows in the
         * list's order and does not sort, for exactly this kind of
         * reason.
         */
        /* And not on the menu level, where row i is a tag rather than
         * station i -- the marker would land on whichever genre happens
         * to sit at the playing station's index. */
        const bool playing = s_radio
            ? (!s_radio_menu && i == s_playing_station)
            : s_pod                                                           /* 8028 */
            ? (dl_live && i < s_pod_en && s_pod_eps[i].id == dl_id)
            : (!s_entries[i].is_dir && is_current(s_entries[i].name));
        /*
         * 8028: the downloading episode is marked as the playing track
         * is -- the accent on its note and title -- and a bar along the
         * row's foot says how far it has got: the accent while it runs,
         * dim while paused. No bar when the server gave no length.
         */
        if (playing && s_pod && dl_total > 0) {
            int64_t fill = (int64_t)w * dl_got / dl_total;
            if (fill > w) fill = w;
            gfx_fill_rect(0, y + ROW_H - 5, w, 5, C_FAINT);
            gfx_fill_rect(0, y + ROW_H - 5, (int)fill, 5, dl_st == PODDL_RUNNING ? C_ACCENT : C_DIM);
        }

        if (s_entries[i].is_dir) {
            draw_folder_icon(48, y + ROW_H / 2, C_DIM);
        } else {
            draw_note_icon(48, y + ROW_H / 2, playing ? C_ACCENT : C_DIM);
        }
        /* Files only. See find_common_prefix() -- the name itself is
         * untouched; this is where the elision happens and the only
         * place it happens. */
        const char *label = s_entries[i].label ? s_entries[i].label
                          : s_entries[i].name
                          + (s_entries[i].is_dir ? 0 : s_prefix_len);

        /*
         * The star, on station rows only -- not on the menu level,
         * where a row is a chart or a tag and there is nothing to star,
         * and not on the volume tabs, where a row is a file.
         *
         * An empty outline on every unstarred row rather than nothing
         * at all: a control that appears only once it has been used is
         * one nobody finds. It is drawn dim so that a list with none
         * starred does not read as a column of decorations.
         */
        const bool starrable = s_radio ? !s_radio_menu : !s_pod;   /* 8014: nothing to star yet */
        /* 8021: nor the way back to a podcast. */
        const bool is_link = !s_radio && !s_pod && s_dir_link && i == 0;
        /* 8024: an episode's length and size at the row's right end. */
        char meta[40] = "";
        if (s_pod && s_pod_level == 2 && i < s_pod_en) {
            char l[16] = "", z[24] = "";
            if (s_pod_eps[i].duration > 0) fmt_len(s_pod_eps[i].duration, l, sizeof(l));
            if (s_pod_eps[i].size > 0)     fmt_mb(s_pod_eps[i].size, z, sizeof(z));
            snprintf(meta, sizeof(meta), "%s%s%s", l, l[0] && z[0] ? "  " : "", z);
        }
        const int  meta_w = meta[0] ? gfx_text_w(meta, LABEL_SCALE) + 24 : 0;
        if (meta[0])
            gfx_draw_text(w - SCROLL_W - 16 - (meta_w - 24), y + (ROW_H - GFX_GLYPH_H(LABEL_SCALE)) / 2,
                          meta, LABEL_SCALE, meta_w, C_DIM);
        const int  name_w = w - 112 - SCROLL_W - (starrable && !is_link ? STAR_W : 0) - meta_w;
        gfx_draw_text(96, y + (ROW_H - GFX_GLYPH_H(NAME_SCALE)) / 2, label,
                      NAME_SCALE, name_w, playing ? C_ACCENT : C_TEXT);

        if (starrable && !is_link) {
            draw_star(w - SCROLL_W - STAR_W / 2, y + ROW_H / 2, STAR_R,
                      s_entries[i].fav,
                      s_entries[i].fav ? C_STAR : C_DIM, row_bg);
        }
    }

    /*
     * Scroll position, as a bar down the right edge -- and, since 0606,
     * the way to change it. A number of pages would need a font; a bar
     * says the same thing in sixteen pixels and can be dragged.
     *
     * The call IS the test. s_count > rows is the same question
     * scroll_geom() already answers, and asking it separately while
     * discarding the answer is what let the outputs be read on a path
     * the compiler could not prove they had been written on. One
     * condition, and it is the one that fills the variables.
     */
    int track_y, track_h, bar_h, max_top;
    if (scroll_geom(&track_y, &track_h, &bar_h, &max_top)) {
        const int bar_y = track_y + (max_top > 0 ? ((track_h - bar_h) * s_top) / max_top : 0);

        /*
         * Wider than it was, because it is a control now rather than a
         * readout. Eight pixels is legible; it is not something a finger
         * aims at. SCROLL_W is still narrow enough to leave the row taps
         * the whole of the rest of the width.
         */
        gfx_fill_rect(w - SCROLL_W, track_y, SCROLL_W, track_h, C_ROW);
        gfx_fill_rect(w - SCROLL_W, bar_y, SCROLL_W, bar_h,
                      s_scroll_drag ? C_TEXT : C_DIM);
    }

    /*
     * 8008: the podcast tab's whole content, where its rows will go.
     * Without a key: what is needed and where to put it -- the remote's
     * own address when it is up, else how to turn it on. Paragraphs, so
     * gfx wraps them and a translator gets each whole.
     */
    /*
     * 8026: an episode's page -- under its download rows, its length,
     * size and date, then its description, as much as fits above the
     * footer. A paragraph, so gfx wraps it and right-to-left text is laid
     * out as gfx lays it out.
     */
    if (s_pod && s_pod_level == 3 && !s_pod_act && s_pod_pick < s_pod_en) {
        const podep_t *e = &s_pod_eps[s_pod_pick];
        const int px = 32, pw = w - 64 - SCROLL_W, step = GFX_GLYPH_H(LABEL_SCALE) + 10;
        int py = LIST_TOP + s_count * ROW_H + 24;
        const int bottom = h - FOOT_H - 8;
        char l[16] = "", z[24] = "", d[16] = "", meta[64];
        if (e->duration > 0) fmt_len(e->duration, l, sizeof(l));
        if (e->size > 0)     fmt_mb(e->size, z, sizeof(z));
        if (e->published > 0) {
            char b[PODPATH_NAME_MAX + 16];
            podpath_base(e->published, "", b, sizeof(b));      /* "YYYY-MM-DD " */
            snprintf(d, sizeof(d), "%.10s", b);
        }
        snprintf(meta, sizeof(meta), "%s%s%s%s%s", d, d[0] && (l[0] || z[0]) ? "  " : "",
                 l, l[0] && z[0] ? "  " : "", z);
        if (meta[0] && py + step <= bottom) {
            gfx_draw_text(px, py, meta, LABEL_SCALE, pw, C_DIM);
            py += step + 8;
        }
        const int rows_left = (bottom - py) / step;
        if (e->desc[0] && rows_left > 0)
            gfx_draw_para(px, py, e->desc, LABEL_SCALE, pw, step, rows_left, C_TEXT);
    }

    /* 8014: with a key the rows are the content, and the status line says
     * what is happening when there are none yet. */
    if (s_pod && podindex_state(NULL, 0, NULL) != PODINDEX_WORKING) {
        const int px = 32, pw = w - 64, step = GFX_GLYPH_H(LABEL_SCALE) + 10;
        int py = LIST_TOP + 32;
        const char *p1 = _("Podcasts are found through Podcast Index, which needs your own free API key "
                           "and secret from api.podcastindex.org. Enter them on the remote's page, "
                           "under Podcast Index:");
        py += step * gfx_draw_para(px, py, p1, LABEL_SCALE, pw, step, 12, C_TEXT);
        {
            char url[48];
            py += step;
            if (remote_running() && remote_url(url, sizeof(url))) {
                gfx_draw_text(px, py, url, NAME_SCALE, pw, C_ACCENT);
            } else {
                gfx_draw_para(px, py, _("The remote is off. Turn on Remote control on the "
                                        "NET tab, and its address shows here."),
                              LABEL_SCALE, pw, step, 6, C_DIM);
            }
        }
    }

    const int fy = h - FOOT_H;
    gfx_fill_rect(0, fy, w, FOOT_H, C_BG);
    gfx_fill_rect(0, fy, w, 2, C_RULE);

    int bx, bw;
        /*
     * Slot 0 is UP on a volume and RLOD on the radio tab.
     *
     * Reused rather than given a seventh button. UP is a volume
     * operation and has nothing to do on RADIO, so the slot was drawn
     * disabled and refused -- a dead button in the corner the eye goes
     * to first. Reload is the operation that tab actually wants and had
     * nowhere to be.
     *
     * Always enabled on RADIO, including with no stations at all. That
     * is the case it is most for: the status line says
     * "no stations.m3u on the card", and the point of reading it is to
     * go and make one.
     */
    foot_box(0, &bx, &bw);
    if (s_radio) {
        /*
         * UP on the second level, RLOD on the first, which is the same
         * reuse of this slot the tab already makes and for the same
         * reason. Going back up from a list of stations is exactly what
         * UP means on a volume; reloading is what the menu level has to
         * offer, and the two never want the slot at once.
         */
        draw_button(bx + 4, fy + 8, bw - 8, FOOT_H - 16,
                    s_radio_menu ? _("RLOD") : _("UP"), true);
    } else {
        /* 8018: on PODCAST, UP is back to the categories. */
        draw_button(bx + 4, fy + 8, bw - 8, FOOT_H - 16, _("UP"),
                    s_pod ? s_pod_level >= 1 : (!at_root() && s_dir[0]));     /* 8019: >= */
    }
    foot_box(1, &bx, &bw); draw_button(bx + 4, fy + 8, bw - 8, FOOT_H - 16, _("FLDR"), !s_radio && s_dir[0] != '\0');
    foot_box(2, &bx, &bw); draw_button(bx + 4, fy + 8, bw - 8, FOOT_H - 16, _("UP^"), s_top > 0);
    foot_box(3, &bx, &bw); draw_button(bx + 4, fy + 8, bw - 8, FOOT_H - 16, _("DN"), s_top + rows < s_count);
    foot_box(4, &bx, &bw); draw_button(bx + 4, fy + 8, bw - 8, FOOT_H - 16, _(order_button_label()), true);
    foot_box(5, &bx, &bw); draw_button(bx + 4, fy + 8, bw - 8, FOOT_H - 16, same("X"), true);  /* 6106 */

    gfx_blit(0, h);
}

/* ------------------------------------------------------------------ */
/* Touch                                                               */
/* ------------------------------------------------------------------ */

browser_result_t browser_touch(bool down, int x, int y)
{
    browser_result_t res = { BROWSER_NONE, NULL, -1 };
    const bool tapped = down && !s_was_down;
    s_was_down = down;

    if (!s_open) return res;

    const int w = gfx_w(), h = gfx_h();
    const int rows = rows_visible();

    /*
     * The scrollbar, before anything else and outside the tapped test.
     *
     * Outside it because a drag is a sequence of downs with one edge at
     * the front, and everything else in this function wants only that
     * edge. Before the rows because the grab zone overlaps them: a press
     * inside it is a scroll, not a file, and a press that starts a drag
     * must not also open whatever it happened to land on.
     */
    if (s_scroll_drag || (tapped && x >= w - SCROLL_HIT_W)) {
        int track_y, track_h, bar_h, max_top;
        if (down && scroll_geom(&track_y, &track_h, &bar_h, &max_top) &&
            y >= track_y && y < track_y + track_h) {

            /*
             * The thumb centres on the finger rather than keeping the
             * offset it was grabbed at.
             *
             * Grab-offset is the desktop behaviour and it is the right
             * one for a mouse, where the pointer is a pixel and the
             * thumb is visible under it. Here the thumb is under a
             * fingertip that covers it entirely, so preserving an offset
             * preserves something nobody can see, and a press on the
             * track above the thumb would do nothing at all instead of
             * going there. Centring makes press-anywhere and drag the
             * same gesture.
             */
            const int span = track_h - bar_h;
            int pos = y - track_y - bar_h / 2;
            if (pos < 0) pos = 0;
            if (pos > span) pos = span;

            const int want = span > 0 ? (pos * max_top + span / 2) / span : 0;
            if (want != s_top) {
                s_top = want;
                s_dirty = true;
            }
            /* Set after the move, so the first frame of a drag already
             * draws the thumb in its held colour. */
            s_scroll_drag = true;
            return res;
        }

        if (!down && s_scroll_drag) {
            s_scroll_drag = false;
            s_dirty = true;          /* back to the resting colour */
            ESP_LOGI(TAG, "scrolled to row %d of %d", s_top, s_count);
            return res;
        }
    }

    if (!tapped) return res;

    if (y < TAB_H) {
        /* Same quarters as the drawing (8008), and derived the same way
         * so a tap lands on the strip it looks like it landed on. */
        const int t1 = w / 4, t2 = (2 * w) / 4, t3 = (3 * w) / 4;
        const browser_tab_t want = (x < t1) ? BROWSER_TAB_SD
                                 : (x < t2) ? BROWSER_TAB_USB
                                 : (x < t3) ? BROWSER_TAB_RADIO
                                            : BROWSER_TAB_PODCAST;
        static const char *const tab_name[BROWSER_TAB_COUNT] = {   /* 6021: log */
            same("SD"), same("USB"), same("RADIO"), same("PODCAST")
        };
        ESP_LOGI(TAG, "button: tab %s", tab_name[want]);
        select_tab(want);
        return res;
    }

    if (y >= h - FOOT_H) {
        const int bw = w / FOOT_BUTTONS;
        int which = x / bw;
        if (which >= FOOT_BUTTONS) which = FOOT_BUTTONS - 1;

        /* One line per footer press, named, before the switch acts on
         * it -- so a press that turns out to do nothing (page up at the
         * top of the list, play-folder on an empty folder) still shows
         * up as having been received. A button that is working and a
         * button that is not both look like silence otherwise. */
        /* 6021: same() -- console only, English. */
        static const char *const foot_name[FOOT_BUTTONS] = {
            same("up"), same("play folder"), same("page up"), same("page down"),
            same("order"), same("cancel")
        };
        static const char *const foot_name_radio[FOOT_BUTTONS] = {
            same("reload stations"), same("play folder"), same("page up"),
            same("page down"), same("order"), same("cancel")
        };
        static const char *const foot_name_menu[FOOT_BUTTONS] = {
            same("back to the menu"), same("play folder"), same("page up"),
            same("page down"), same("order"), same("cancel")
        };
        ESP_LOGI(TAG, "button: %s",
                 !s_radio ? foot_name[which]
                          : s_radio_menu ? foot_name_radio[which]
                                         : foot_name_menu[which]);

        /*
         * UP and FLDR are volume operations and the radio tab has
         * neither a parent nor a folder to play. Refused here as well as
         * drawn disabled, because a disabled button that still acts is
         * worse than one that is not drawn at all.
         */
        /* 8018: slot 0 on PODCAST is back to the categories, and does
         * nothing on them; go_up() has no directory to go up from. */
        if (s_pod && which == 0) {
            /* 8019: from the episodes back to the podcasts, which are
             * still held, so no request. */
            if (s_pod_level == 3) {             /* 8020: no drive chosen; 8024: no action */
                s_pod_act = false;
                s_pod_level = 2;
                load_pod_ep_rows();
            } else if (s_pod_level == 2 && s_pod_linked) {
                /* 8021: from a folder's link there is no podcast list
                 * behind this one; the categories are. */
                s_pod_level = 0;
                s_pod_wait = false;
                s_pod_linked = false;
                load_pod_rows();
                const podindex_cats_t cs = podindex_cats(NULL, 0, NULL, NULL);
                if (cs == PODINDEX_CATS_NONE || cs == PODINDEX_CATS_FAILED) {
                    if (net_online()) (void)podindex_cats_fetch();
                    else              s_pod_wait = true;
                }
            } else if (s_pod_level == 2) {
                s_pod_level = 1;
                s_pod_wait = false;
                load_pod_feed_rows();
            } else if (s_pod_level == 1) {
                s_pod_level = 0;
                s_pod_wait = false;
                load_pod_rows();
            }
            return res;
        }
        /*
         * Slot 0 on RADIO is the reload, not UP. Before the refusal
         * below, which still owns slot 1.
         */
        if (s_radio && which == 0) {
            if (!s_radio_menu) {
                /* Back to the menu, and done here: the rows are this
                 * task's to rebuild and there is nothing to ask the
                 * player for. The list stays loaded, so a station
                 * playing out of it keeps playing and next still moves
                 * through it -- going up a level is a change of view
                 * and not a change of what is on. */
                s_radio_menu = true;
                s_radio_status[0] = '\0';
                load_radio_menu();
                return res;
            }
            res.kind = BROWSER_RELOAD_STATIONS;
            return res;
        }
        if (s_radio && which == 1) {
            ESP_LOGI(TAG, "not on the radio tab");
            return res;
        }

        switch (which) {
        case 0:
            go_up();
            break;
        case 1:
            if (s_dir[0]) {
                snprintf(s_result, sizeof(s_result), "%s", s_dir);
                res.kind = BROWSER_PLAY_FOLDER;
                res.path = s_result;
            }
            break;
        case 2:
            if (s_top > 0) {
                s_top -= rows;
                if (s_top < 0) s_top = 0;
                s_dirty = true;
            }
            break;
        case 3:
            if (s_top + rows < s_count) {
                s_top += rows;
                s_dirty = true;
            }
            break;
        case 4:
            /* ONE -> ALL -> RPT -> EAT -> RND -> RPT1 -> ONE. RPT1 sits
             * after RND so the whole-list modes stay adjacent and the two
             * single-track modes bookend the cycle; 5252's EAT is ALL
             * that eats, and 5261's RPT is ALL that wraps, so both sit
             * beside ALL. */
            s_order = (s_order == PLAY_ORDER_ONE)     ? PLAY_ORDER_ALL
                    : (s_order == PLAY_ORDER_ALL)     ? PLAY_ORDER_REPEAT_ALL
                    : (s_order == PLAY_ORDER_REPEAT_ALL) ? PLAY_ORDER_EAT
                    : (s_order == PLAY_ORDER_EAT)     ? PLAY_ORDER_SHUFFLE
                    : (s_order == PLAY_ORDER_SHUFFLE) ? PLAY_ORDER_REPEAT_ONE
                                                      : PLAY_ORDER_ONE;
            ESP_LOGI(TAG, "play order now %s", order_label());
            s_dirty = true;
            break;
        default:
            res.kind = BROWSER_CANCELLED;
            break;
        }
        return res;
    }

    const int r = (y - LIST_TOP) / ROW_H;
    if (r < 0 || r >= rows) return res;
    const int i = s_top + r;
    if (i >= s_count) return res;

    ESP_LOGI(TAG, "button: row %d (%s) \"%s\"", i,
             s_pod ? (s_pod_level == 3 ? (s_pod_act ? "action" : "drive")             /* 8024 */
                      : s_pod_level == 2 ? "episode"           /* 8018-8020 */
                      : s_pod_level ? "podcast" : "category")
                   : s_radio ? "station" : s_entries[i].is_dir ? "dir" : "file",
             s_entries[i].name);

    if (s_pod) {
        s_pod_note = NULL;                      /* 8020: answered by this tap, if at all */
        if (s_pod_level == 0) {
            /* 8018: a category row opens into its podcasts. The rows go
             * now; the list replaces them when it arrives. */
            if (i >= s_pod_n) return res;
            s_pod_cat = i;
            s_pod_level = 1;
            entries_free();
            s_top = 0;
            s_dirty = true;
            ESP_LOGI(TAG, "category %d (\"%s\"): asking for its podcasts",
                     s_pod_cats[i].id, s_pod_cats[i].name);
            pod_fetch_feeds();
            return res;
        }
        if (s_pod_level == 1) {
            /* 8019: a podcast row opens into its episodes, as a category
             * row opens into its podcasts. */
            if (i >= s_pod_fn) return res;
            s_pod_feed = i;
            snprintf(s_pod_show, sizeof(s_pod_show), "%s", s_pod_feeds[i].title);   /* 8020 */
            s_pod_cur = s_pod_feeds[i].id;                                          /* 8021 */
            s_pod_linked = false;
            s_pod_level = 2;
            entries_free();
            s_top = 0;
            s_dirty = true;
            ESP_LOGI(TAG, "podcast %lld: asking for its episodes", (long long)s_pod_feeds[i].id);
            pod_fetch_eps();
            return res;
        }
        if (s_pod_level == 3 && s_pod_act) {
            /* 8024: pause or go on; cancel. */
            const poddl_state_t ds = poddl_state(NULL, NULL, NULL, NULL, 0, NULL);
            if (i == 0 && ds == PODDL_RUNNING) poddl_pause();
            else if (i == 0 && s_pod_pick < s_pod_en) pod_download(s_pod_pick, s_pod_act_vol);
            else if (i == 1) poddl_cancel();
            s_pod_act = false;
            s_pod_level = 2;
            load_pod_ep_rows();
            return res;
        }
        if (s_pod_level == 3) {
            /* 8020: a drive row. */
            if (i < s_pod_nvols && s_pod_pick < s_pod_en) pod_download(s_pod_pick, s_pod_vols[i]);
            s_pod_level = 2;
            load_pod_ep_rows();
            return res;
        }
        /*
         * 8020: an episode row. On a drive already: play it, and its
         * folder becomes the list, as a file tapped on a volume tab.
         * Downloading now: nothing. Else download it -- to the one drive
         * that is in, or ask which when both are.
         */
        if (i >= s_pod_en) return res;
        {
            static char dir[400], base[PODPATH_NAME_MAX + 16];
            s_pod_nvols = 0;
            for (int v = 0; v < STORAGE_COUNT; v++) {
                if (!storage_present((storage_id_t)v)) continue;
                s_pod_vols[s_pod_nvols++] = (storage_id_t)v;
                if (pod_ep_where((storage_id_t)v, i, dir, sizeof(dir), base, sizeof(base)) &&
                    poddl_find(dir, base, s_result, sizeof(s_result))) {
                    ESP_LOGI(TAG, "episode %lld: playing %s", (long long)s_pod_eps[i].id, s_result);
                    res.kind = BROWSER_PLAY_FILE;
                    res.path = s_result;
                    return res;
                }
            }
            /* 8024: a part file of this episode on a drive -- paused, cut
             * off, or from before a reboot. */
            storage_id_t part_vol = STORAGE_COUNT;
            for (int k = 0; k < s_pod_nvols && part_vol == STORAGE_COUNT; k++)
                if (pod_ep_where(s_pod_vols[k], i, dir, sizeof(dir), base, sizeof(base)) &&
                    poddl_part(dir, base, NULL))
                    part_vol = s_pod_vols[k];
            int64_t id = 0;
            const poddl_state_t ds = poddl_state(&id, NULL, NULL, NULL, 0, NULL);
            if (ds == PODDL_RUNNING && id != s_pod_eps[i].id) {
                ESP_LOGI(TAG, "episode %lld: another download is running", (long long)s_pod_eps[i].id);
                s_pod_note = _("another episode is downloading");
                s_dirty = true;
                return res;
            }
            /* 8024: this episode, running or with a part file: its actions. */
            if ((ds == PODDL_RUNNING && id == s_pod_eps[i].id) || part_vol != STORAGE_COUNT) {
                s_pod_pick = i;
                s_pod_act = true;
                s_pod_act_vol = part_vol != STORAGE_COUNT ? part_vol : s_pod_vols[0];
                s_pod_level = 3;
                load_pod_act_rows();
                return res;
            }
            /* 8026: its page -- what it is, then where to put it -- for
             * one drive, two, or none (the status line says so). */
            s_pod_act = false;
            s_pod_pick = i;
            s_pod_level = 3;
            load_pod_pick_rows();
        }
        return res;
    }

    if (s_radio && s_radio_menu) {
        /*
         * A menu row. Row 0 is the card and is a reload; everything
         * else is a request to the directory. What the row MEANS is
         * radiobrowser.h's business -- this asks whether it is a fetch
         * and passes the number on.
         */
        if (!radiobrowser_menu_kind(i, NULL, NULL)) {
            /* Three rows are not fetches, and they are three different
             * actions. radiobrowser.h names them so this does not
             * hard-code which number is which. */
            res.kind = (i == RADIOBROWSER_MENU_ADD) ? BROWSER_ADD_STATION
                     : (i == RADIOBROWSER_MENU_FAV) ? BROWSER_LOAD_FAVORITES
                                                    : BROWSER_RELOAD_STATIONS;
            return res;
        }
        res.kind = BROWSER_FETCH_STATIONS;
        res.index = i;
        return res;
    }

    if (s_radio) {
        /*
         * The star's strip, tested before the row: it is inside the
         * row's box and has to win, which is the rule row 7 of the
         * panel already follows. The scrollbar has had its own test
         * long before this point, so this strip never reaches it.
         */
        if (x >= gfx_w() - SCROLL_W - STAR_W && x < gfx_w() - SCROLL_W) {
            res.kind = BROWSER_TOGGLE_FAVORITE;
            res.index = i;
            return res;
        }
        /* The index, not the URL. See BROWSER_PLAY_STREAM in browser.h:
         * the rows were built from the station list in its own order, so
         * row i IS station i, and stations.c stays the only thing that
         * knows what station i is. */
        res.kind = BROWSER_PLAY_STREAM;
        res.index = i;
        return res;
    }

    /* 8021: the way back to a podcast's episodes, ahead of the star's
     * strip and of anything that would join ".podcast" to s_dir. */
    if (s_dir_link && i == 0) {
        ESP_LOGI(TAG, "podcast %lld: back to its episodes", (long long)s_dir_link_feed);
        select_tab(BROWSER_TAB_PODCAST);
        if (podindex_state(NULL, 0, NULL) != PODINDEX_WORKING) return res;  /* the tab says why */
        snprintf(s_pod_show, sizeof(s_pod_show), "%s", s_dir_link_show);
        s_pod_cur = s_dir_link_feed;
        s_pod_linked = true;
        s_pod_level = 2;
        entries_free();
        s_top = 0;
        s_dirty = true;
        pod_fetch_eps();
        return res;
    }

    /* The star's strip on a file or folder row, same place and same
     * precedence as the station rows'. */
    if (x >= gfx_w() - SCROLL_W - STAR_W && x < gfx_w() - SCROLL_W) {
        if (!storage_join_path(s_result, sizeof(s_result), s_dir, s_entries[i].name)) {
            return res;
        }
        res.kind = BROWSER_TOGGLE_STAR;
        res.path = s_result;
        res.index = s_entries[i].is_dir ? 1 : 0;
        return res;
    }

    if (s_entries[i].is_dir) {
        char sub[512];
        if (!storage_join_path(sub, sizeof(sub), s_dir, s_entries[i].name)) {
            ESP_LOGW(TAG, "path too long: %s/%s", s_dir, s_entries[i].name);
            return res;
        }
        load_dir(sub);
        return res;
    }

    if (!storage_join_path(s_result, sizeof(s_result), s_dir, s_entries[i].name)) {
        ESP_LOGW(TAG, "path too long: %s/%s", s_dir, s_entries[i].name);
        return res;
    }
    res.kind = BROWSER_PLAY_FILE;
    res.path = s_result;
    return res;
}
