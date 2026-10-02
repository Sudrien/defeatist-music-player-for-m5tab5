/*
 * recorder -- see recorder.h, and 5106 in ARCHITECTURE.md.
 *
 * SPDX-License-Identifier: MIT
 */
#include "recorder.h"
#include "i18n.h"         /* 6017 */

#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

#include "audio_out.h"
#include "beam.h"
#include "flacenc.h"
#include "heapmap.h"
#include "levelhist.h"        /* 5214 */
#include "micpcm.h"           /* 5208 */
#include "settings.h"
#include "storage.h"
#include "storage_io.h"
#include "uac.h"              /* 5208 */

static const char *TAG = "tab5_rec";

#define REC_DIR             "Recordings"
#define REC_FRAME_BYTES     (AUDIO_CAPTURE_CHANNELS * sizeof(int32_t))

/* Two seconds of capture between the microphones and the card. A card's
 * worst ordinary stall (a FAT allocation, a wear-levelling pause) is a
 * few hundred ms; two seconds is the margin, at 768 KB of PSRAM.
 * 5208: sized for the largest frame, the built-in pair's. The headset's
 * mono frames get four seconds of it; a USB microphone's at 48 kHz get
 * two or four, and at 96 kHz stereo one. */
#define REC_RING_BYTES      (2u * AUDIO_CAPTURE_RATE * REC_FRAME_BYTES)

/*
 * 5113: the start of every capture is not audio. On the board, every
 * take -- beam and stereo -- begins with ~30 ms of exact digital silence,
 * a thump at 30-50 ms some 15 dB over the room, and a second at ~130-160
 * ms in which L-R is as loud as L+R: uncorrelated between the capsules,
 * so electrical rather than acoustic -- the ES7210 and its bias settling.
 * It put two clicks at the head of every file, and the second is exactly
 * what the beam's canceller adapts on (a +2.4 dB peak on one take). The
 * steady scene is there from ~200 ms. This much is read and dropped
 * before anything reaches the ring.
 */
#define REC_SETTLE_MS       (250)

/* What rec_in moves per read: 5 ms, a quarter of the DMA's 20 ms. */
#define REC_IN_FRAMES       (240)

/* What rec_enc takes per pass: one FLAC block. */
#define REC_BLOCK           (4096)

/* Under FAT32's 4 GiB file limit with room for the last block and the
 * header rewrite. About 5 1/2 hours at the rate these files come out. */
#define REC_MAX_BYTES       (4000000000ull)

/* Stacks. rec_in reads into a module buffer and calls nothing deep.
 * rec_enc goes through flacenc (about 300 bytes, 5104) and fwrite into
 * FatFs, which is the deep part. Internal RAM, and only for the length
 * of a recording: created at start, deleted at the end. */
#define REC_IN_STACK        (3072)
#define REC_ENC_STACK       (6144)
#define REC_IN_PRIO         (6)     /* i2s_wr's: the DMA has 20 ms */
#define REC_ENC_PRIO        (3)

/* ---- state ---- */

/*
 * The ring and the two work buffers, allocated at the first recording
 * and never freed -- netstream.c's rule for a static stream buffer over a
 * permanent allocation, for its reason (CLAUDE.md). PSRAM: 768 KB + 32 KB
 * + 2 KB, none of it touched when nothing is recording.
 */
static StreamBufferHandle_t s_ring;
static StaticStreamBuffer_t s_ring_struct;
static uint8_t             *s_ring_storage;
static int32_t             *s_in_buf;       /* REC_IN_FRAMES frames */
static int32_t             *s_enc_buf;      /* REC_BLOCK frames */
static beam_t              *s_beam;         /* 5109: ~1.2 KB, PSRAM with the rest */
static bool                 s_use_beam;     /* this recording's choice */

/*
 * 5208: this recording's input and its format. s_in_ch is the frame in
 * the ring, as the source delivers it; s_fold folds a USB microphone's
 * stereo to mono on rec_enc. s_rate is what every seconds and ms figure
 * divides by: a USB microphone's own rate, not AUDIO_CAPTURE_RATE.
 */
static settings_rec_from_t  s_src;         /* 5216: resolved, never AUTO */
static bool                 s_auto;         /* 5216: chosen by AUTO */
static uint32_t             s_rate = AUDIO_CAPTURE_RATE;
static unsigned             s_in_ch = AUDIO_CAPTURE_CHANNELS;
static unsigned             s_bits = AUDIO_CAPTURE_BITS;
static bool                 s_fold;
static volatile bool        s_src_gone;     /* the USB microphone was unplugged */

/*
 * 5214: what the screen shows while recording. A minute of input peak,
 * in the streaming strip's own units (levelhist.h), fed by rec_in from
 * what it reads -- before the beam, so it is the microphones and not
 * the processing. And how long the input has been exact digital zero:
 * a live ADC never is, even in a silent room, so 300 ms of it means the
 * source is muted (a USB headset's mute switch sends zeros) or sending
 * nothing. Both under s_mux; levelhist_push() is a few stores.
 */
#define REC_SILENT_MS       (300)
static levelhist_t          s_hist;
static uint32_t             s_hist_frames;  /* frames not yet whole ms */
static uint32_t             s_zero_frames;

/* 5215: bumped when a recording is finished or renamed, so a folder
 * list showing Recordings/ knows it is stale. See recorder.h. */
static volatile uint32_t    s_files_gen;

static void files_changed(void)
{
    /* rec_enc and ui_task both bump it: one atomic add, no lock. */
    __atomic_add_fetch(&s_files_gen, 1, __ATOMIC_RELAXED);
}

uint32_t recorder_files_changed(void) { return s_files_gen; }

static FILE      *s_file;
static flacenc_t *s_enc;

static volatile bool s_active;
static volatile bool s_stop;
static volatile bool s_in_done;
static volatile bool s_write_failed;

/* Counters: plain adds, under a spinlock so the ui_task's copy is never
 * torn. The notice strings are under a mutex -- snprintf does not belong
 * inside a critical section. */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t s_text_lock;
static volatile bool s_log_end;     /* heapmap_log owed, from ui_task */
static uint64_t s_frames;           /* encoded */
static uint64_t s_bytes;            /* written */
static uint64_t s_dropped_frames;
static char     s_name[40];
static char     s_path[160];

static bool s_notice;
static char s_notice_head[40];
static char s_notice_body[96];

/*
 * 5114: recordings named while the time was a guess.
 *
 * Before NTP answers, settings_now() is a floor -- the build, a card, the
 * last session -- and a recording is named for it. When the time moves
 * (NTP, a corroborated card, a reset from the NET tab), every recording
 * made this boot under the old guess is renamed by exactly the amount it
 * moved, and its file date with it. settings_clock_offset() is the sum
 * of the moves; each entry remembers the offset its name last matched.
 *
 * Only this boot's: an earlier boot's offset from the truth is not
 * known, because the time that was wrong was carried across the power-
 * off, when the true time kept going and the guess did not. Held in
 * PSRAM with the recorder's buffers; REC_FIX_MAX recordings a boot.
 */
#define REC_FIX_MAX         (32)
typedef struct {
    char    path[160];
    int64_t epoch;          /* the time the name says */
    int64_t offset;         /* settings_clock_offset() the name matches */
} rec_fix_t;
static rec_fix_t *s_fix;
static int        s_fix_n;

static void notice(const char *head, const char *body)
{
    xSemaphoreTake(s_text_lock, portMAX_DELAY);
    snprintf(s_notice_head, sizeof(s_notice_head), "%s", head);
    snprintf(s_notice_body, sizeof(s_notice_body), "%s", body);
    s_notice = true;
    xSemaphoreGive(s_text_lock);
}

/* ---- the file ---- */

static bool file_write(void *ctx, const uint8_t *buf, size_t n)
{
    (void)ctx;
    if (s_write_failed || !s_file) return false;
    if (s_bytes + n > REC_MAX_BYTES) {
        ESP_LOGW(TAG, "4 GB: ending the recording");
        s_write_failed = true;
        notice(N_("Recording stopped"), N_("It reached 4 GB, the most one file can hold."));
        return false;
    }
    storage_io_acquire(STORAGE_IO_BACKGROUND);
    const size_t put = fwrite(buf, 1, n, s_file);
    storage_io_release();
    if (put != n) {
        ESP_LOGE(TAG, "write failed after %" PRIu64 " bytes (errno %d)", s_bytes, errno);
        s_write_failed = true;
        notice(N_("Recording stopped"), N_("The card would not take any more."));
        return false;
    }
    portENTER_CRITICAL(&s_mux);
    s_bytes += n;
    portEXIT_CRITICAL(&s_mux);
    return true;
}

/* 5114: the time a recording is named for, and the clock offset then. */
static int64_t s_start_epoch, s_start_offset;

/* A free name in dir for a recording started at epoch: "2026-09-26
 * 18.04.33.flac", then " (2)" and on if that exists -- two presses in one
 * second, or a clock that has not moved since the last boot. */
static bool name_for(const char *dir, int64_t epoch, char *name, size_t name_len,
                     char *path, size_t path_len)
{
    _Static_assert(sizeof(time_t) == 8, "time_t must be 64-bit for settings_now()");
    const time_t t = (time_t)epoch;
    struct tm tm;
    gmtime_r(&t, &tm);
    char stem[24];
    strftime(stem, sizeof(stem), "%Y-%m-%d %H.%M.%S", &tm);
    struct stat sb;
    for (int n = 1; n < 100; n++) {
        if (n == 1) snprintf(name, name_len, "%s.flac", stem);
        else        snprintf(name, name_len, "%s (%d).flac", stem, n);
        if (!storage_join_path(path, path_len, dir, name)) return false;
        if (stat(path, &sb) != 0) return true;
    }
    return false;
}

/* "<mount>/Recordings/<settings_now()>.flac". */
static bool pick_path(const char *mount)
{
    char dir[96];
    if (!storage_join_path(dir, sizeof(dir), mount, REC_DIR)) return false;
    if (mkdir(dir, 0777) != 0 && errno != EEXIST) {
        ESP_LOGE(TAG, "mkdir %s: errno %d", dir, errno);
        return false;
    }
    /*
     * 5107: settings_now(), not time(). Nothing sets the system clock
     * (settings.h), so time() is 1970 on every boot; settings_now() is
     * the player's own belief -- the last NTP reply, or failing that the
     * build time or the card's floor, carried forward on the monotonic
     * timer. time_t is 64 bits here, so gmtime_r() does not truncate
     * what settings.c keeps in an int64_t.
     */
    s_start_epoch = settings_now();
    s_start_offset = settings_clock_offset();
    return name_for(dir, s_start_epoch, s_name, sizeof(s_name), s_path, sizeof(s_path));
}

static void finish_file(void)
{
    uint8_t si[FLACENC_STREAMINFO_BYTES];
    const bool ok = flacenc_close(s_enc, s_write_failed ? NULL : si) && !s_write_failed;
    s_enc = NULL;
    storage_io_acquire(STORAGE_IO_BACKGROUND);
    if (ok) {
        /* The header's final STREAMINFO: sample count and frame sizes.
         * A file that never gets here still plays -- see flacenc.h. */
        if (fflush(s_file) != 0 ||
            fseek(s_file, FLACENC_STREAMINFO_OFFSET, SEEK_SET) != 0 ||
            fwrite(si, 1, sizeof(si), s_file) != sizeof(si)) {
            ESP_LOGW(TAG, "could not rewrite STREAMINFO; the file plays, length unknown");
        }
    }
    storage_io_close(s_file);
    storage_io_release();
    s_file = NULL;
}

/* ---- the tasks ---- */

static void rec_in_run(void)
{
    uint32_t reads = 0;
    uint32_t settle = s_rate * REC_SETTLE_MS / 1000u;
    const bool usb = (s_src == SETTINGS_REC_UAC);
    while (!s_stop) {
        /* 5208: the USB microphone, or the ES7210 (either input). The
         * same buffer, sized for REC_IN_FRAMES of the widest frame. */
        const size_t n = usb ? uac_mic_read(s_in_buf, REC_IN_FRAMES, 100)
                             : audio_out_capture_read(s_in_buf, REC_IN_FRAMES, 100);
        if (usb && uac_mic_gone()) {
            s_src_gone = true;
            s_stop = true;
            break;
        }
        if (!n) continue;
        reads++;
        if (settle) {                           /* 5113 */
            settle = n >= settle ? 0 : settle - (uint32_t)n;
            continue;
        }
        /* 5214: the screen's level strip and the mute test. */
        int32_t pk = 0;
        for (size_t i = 0; i < n * s_in_ch; i++) {
            const int32_t v = s_in_buf[i] < 0 ? -s_in_buf[i] : s_in_buf[i];
            if (v > pk) pk = v;
        }
        portENTER_CRITICAL(&s_mux);
        s_hist_frames += (uint32_t)n;
        const int ms = (int)(s_hist_frames * 1000u / s_rate);
        s_hist_frames -= (uint32_t)ms * s_rate / 1000u;
        levelhist_push(&s_hist, levelhist_db_peak(pk, (int32_t)1 << (s_bits - 1)), ms);   /* 5273 */
        s_zero_frames = pk ? 0 : s_zero_frames + (uint32_t)n;
        portEXIT_CRITICAL(&s_mux);
        /* All of a read or none of it: a partial send would split a
         * frame and swap left and right for the rest of the file. */
        const size_t want = n * s_in_ch * sizeof(int32_t);
        if (xStreamBufferSpacesAvailable(s_ring) >= want) {
            xStreamBufferSend(s_ring, s_in_buf, want, 0);
        } else {
            portENTER_CRITICAL(&s_mux);
            s_dropped_frames += n;
            portEXIT_CRITICAL(&s_mux);
        }
    }
    /* 5208: the USB microphone is closed on rec_enc, whose stack has
     * room for the driver's control transfers; rec_in's has not been
     * measured against them. */
    if (!usb) audio_out_capture_end();
    ESP_LOGI(TAG, "microphones off after %" PRIu32 " reads (the first %d ms dropped: "
             "the ADC settling)", reads, REC_SETTLE_MS);
    s_in_done = true;
}

static void rec_enc_run(void)
{
    uint64_t last_drop_logged = 0;
    for (;;) {
        /* Whole frames: rec_in sends only whole reads, one writer, so
         * what is available is always a multiple of a frame. */
        const size_t fbytes = s_in_ch * sizeof(int32_t);
        const size_t got = xStreamBufferReceive(s_ring, s_enc_buf,
                                                REC_BLOCK * fbytes,
                                                pdMS_TO_TICKS(100));
        const unsigned frames = (unsigned)(got / fbytes);
        if (frames && !s_write_failed) {
            /* 5109: the beam, in place -- two channels in, one out. */
            if (s_use_beam) beam_process(s_beam, s_enc_buf, s_enc_buf, frames);
            /* 5208: a USB microphone's two channels, folded. */
            if (s_fold) micpcm_mono(s_enc_buf, frames);
            if (!flacenc_write(s_enc, s_enc_buf, frames)) s_stop = true;
            portENTER_CRITICAL(&s_mux);
            s_frames += frames;
            portEXIT_CRITICAL(&s_mux);
        } else if (frames && s_write_failed) {
            s_stop = true;          /* drain and discard */
        }
        if (s_dropped_frames != last_drop_logged) {
            ESP_LOGW(TAG, "ring full: %" PRIu64 " ms of audio dropped so far",
                     s_dropped_frames * 1000 / s_rate);
            last_drop_logged = s_dropped_frames;
        }
        if (s_in_done && xStreamBufferIsEmpty(s_ring)) break;
    }

    if (s_src == SETTINGS_REC_UAC) uac_mic_close();     /* 5208: see rec_in */
    finish_file();

    const uint32_t secs = (uint32_t)(s_frames / s_rate);
    ESP_LOGI(TAG, "recorded %s: %" PRIu32 " s, %" PRIu64 " bytes, %" PRIu64 " ms dropped%s",
             s_path, secs, s_bytes, s_dropped_frames * 1000 / s_rate,
             s_write_failed ? ", ended by a write failure" :
             s_src_gone ? ", ended by the USB microphone going" : "");
    if (s_use_beam) {
        const int g = beam_gain_centi(s_beam);
        ESP_LOGI(TAG, "beam: canceller adapted on %u%% of it; MIC2 matched to MIC1 by %s%d.%d dB",
                 beam_adapt_pct(s_beam), g < 0 ? "-" : "+", abs(g) / 10, abs(g) % 10);
    }
    if (s_src_gone && !s_write_failed) {
        /* 5208: saved, and says why it stopped. The file up to the
         * unplug is whole: rec_in stopped, rec_enc drained the ring. */
        char body[96];
        const storage_id_t vol = storage_of_path(s_path);
        /* 6017: %u rather than PRIu32 -- a macro between the literals
         * would hide the format from tools/i18n.py. */
        snprintf(body, sizeof(body), _("The USB microphone was unplugged.\n"
                 "%u:%02u on %s"), (unsigned)(secs / 60), (unsigned)(secs % 60),
                 vol < STORAGE_COUNT ? storage_label(vol) : "?");
        notice(N_("Recording stopped"), body);
    } else if (!s_write_failed) {
        char body[96];
        /* Which volume, first: with a card and a drive both in, "saved"
         * alone does not say where to look. */
        const storage_id_t vol = storage_of_path(s_path);
        snprintf(body, sizeof(body), _("on %s, %u:%02u\n%s"),
                 vol < STORAGE_COUNT ? storage_label(vol) : "?",
                 (unsigned)(secs / 60), (unsigned)(secs % 60), s_name);
        notice(N_("Recording saved"), body);
    }
    s_log_end = true;           /* the map prints from ui_task's stack */
    files_changed();            /* 5215 */
    s_active = false;
}

/*
 * 6010: the two tasks are created once and never deleted, their stacks
 * in PSRAM, their TCBs static -- medialib.c's reindex task (6005), for
 * the same reason. Created per recording with xTaskCreate(), they took
 * 9 KB of stack from internal RAM at the moment record was pressed, and
 * with Wi-Fi up and playing from USB that was not there: the v0.5.0-9
 * board run got the microphones (6009) and then "No memory for the
 * recording task". Now a recording asks them to run with a notification
 * and they wait again when it ends. Neither writes flash -- rec_enc
 * writes the card, rec_in reads I2S or the USB microphone -- so neither
 * runs with the cache disabled.
 */
#if !CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM
#error "recorder.c puts its task stacks in PSRAM and needs CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM=y"
#endif
static StaticTask_t s_in_tcb, s_enc_tcb;
static StackType_t *s_in_stack, *s_enc_stack;
static TaskHandle_t s_in_task, s_enc_task;

static void rec_in_task(void *arg)
{
    (void)arg;
    for (;;) { ulTaskNotifyTake(pdTRUE, portMAX_DELAY); rec_in_run(); }
}

static void rec_enc_task(void *arg)
{
    (void)arg;
    for (;;) { ulTaskNotifyTake(pdTRUE, portMAX_DELAY); rec_enc_run(); }
}

/* Make both tasks, once. False when PSRAM could not be had. */
static bool tasks_ready(void)
{
    const uint32_t caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    if (!s_enc_task) {
        if (!s_enc_stack) s_enc_stack = heap_caps_malloc(REC_ENC_STACK, caps);
        if (s_enc_stack) {
            s_enc_task = xTaskCreateStatic(rec_enc_task, "rec_enc", REC_ENC_STACK, NULL,
                                           REC_ENC_PRIO, s_enc_stack, &s_enc_tcb);
        }
    }
    if (!s_in_task) {
        if (!s_in_stack) s_in_stack = heap_caps_malloc(REC_IN_STACK, caps);
        if (s_in_stack) {
            s_in_task = xTaskCreateStatic(rec_in_task, "rec_in", REC_IN_STACK, NULL,
                                          REC_IN_PRIO, s_in_stack, &s_in_tcb);
        }
    }
    return s_enc_task && s_in_task;
}

/* ---- the interface ---- */

static bool buffers(void)
{
    if (s_ring) return true;
    if (!s_text_lock) s_text_lock = xSemaphoreCreateMutex();
    if (!s_text_lock) return false;
    s_ring_storage = heap_caps_malloc(REC_RING_BYTES + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_in_buf  = heap_caps_malloc(REC_IN_FRAMES * REC_FRAME_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_enc_buf = heap_caps_malloc(REC_BLOCK * REC_FRAME_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_beam    = heap_caps_malloc(sizeof(beam_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_fix     = heap_caps_calloc(REC_FIX_MAX, sizeof(rec_fix_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_ring_storage || !s_in_buf || !s_enc_buf || !s_beam || !s_fix) {
        /* Kept for the next try rather than freed: see the rule above. */
        ESP_LOGE(TAG, "no PSRAM for the recording buffers");
        return false;
    }
    s_ring = xStreamBufferCreateStatic(REC_RING_BYTES + 1, 1, s_ring_storage, &s_ring_struct);
    return s_ring != NULL;
}

bool recorder_start(char *why, size_t why_len)
{
#define REFUSE(...) do { if (why) snprintf(why, why_len, __VA_ARGS__); return false; } while (0)
    if (s_active) REFUSE(N_("Already recording."));

    storage_id_t vol = STORAGE_COUNT;
    if (storage_present(STORAGE_SD)) vol = STORAGE_SD;
    else if (storage_present(STORAGE_USB)) vol = STORAGE_USB;
    if (vol == STORAGE_COUNT) REFUSE(N_("Insert a card or a USB drive to record to."));

    if (!buffers()) REFUSE(N_("Not enough memory to record."));

    /*
     * 5208: the input, and the format it decides. A USB microphone is
     * opened here, before the file, because its rate and channels are
     * the FLAC header's and are not known until it is -- so every
     * refusal from here on closes it again (ABANDON).
     */
    /*
     * 5216: one switch, six choices (settings.h). AUTO resolves here to
     * the best input that is there -- USB, then the jack, then the
     * built-in pair summed -- and is always mono. A USB microphone AUTO
     * picked that then will not open falls through to the next choice
     * rather than refusing: AUTO promised "whatever is there".
     */
    settings_rec_from_t want = settings_rec_from();
    if (want == SETTINGS_REC_OFF) REFUSE(N_("Recording is off (AUDIO, Record from)."));  /* 5217 */
    const bool autom = (want == SETTINGS_REC_AUTO);
    if (autom) {
        want = uac_mic_announced() ? SETTINGS_REC_UAC
             : audio_out_headphones() ? SETTINGS_REC_HEADSET
             : SETTINGS_REC_MONO;
    }
    s_src_gone = false;
    s_use_beam = false;
    s_fold = false;
    if (want == SETTINGS_REC_UAC) {
        if (!uac_mic_announced()) REFUSE(N_("No USB microphone is plugged in."));
        uint32_t rate = 0;
        uint8_t ch = 0;
        const esp_err_t e = uac_mic_open(&rate, &ch);
        if (e == ESP_OK) {
            s_rate = rate;
            s_in_ch = ch;
            s_bits = 16;
            s_fold = (ch == 2 && autom);
        } else if (autom) {
            ESP_LOGW(TAG, "auto: USB microphone did not open (%s); next input",
                     esp_err_to_name(e));
            want = audio_out_headphones() ? SETTINGS_REC_HEADSET : SETTINGS_REC_MONO;
        } else if (e == ESP_ERR_NOT_FOUND) {
            REFUSE(N_("The USB microphone is not there any more."));
        } else if (e == ESP_ERR_NOT_SUPPORTED) {
            REFUSE(N_("The USB microphone has no 16-bit format."));
        } else {
            /* 6022: the reason to the log, the sentence to the card */
            ESP_LOGW(TAG, "USB microphone did not start: %s", esp_err_to_name(e));
            REFUSE(N_("The USB microphone did not start."));
        }
    }
    if (want == SETTINGS_REC_HEADSET) {
        /* The jack detect cannot tell a headset from headphones; plain
         * headphones record silence, and nothing here can know. */
        if (!audio_out_headphones()) REFUSE(N_("Nothing is plugged into the headset jack."));
        s_rate = AUDIO_CAPTURE_RATE;
        s_in_ch = AUDIO_CAPTURE_HEADSET_CHANNELS;
        s_bits = AUDIO_CAPTURE_HEADSET_BITS;
    } else if (want != SETTINGS_REC_UAC) {
        /* The built-in pair: MONO sums it, STEREO keeps it, FOCUSED is
         * 5109's beam. */
        s_rate = AUDIO_CAPTURE_RATE;
        s_in_ch = AUDIO_CAPTURE_CHANNELS;
        s_bits = AUDIO_CAPTURE_BITS;
        s_use_beam = (want == SETTINGS_REC_FOCUSED);
        s_fold = (want == SETTINGS_REC_MONO);
    }
    s_src = want;
    s_auto = autom;
#define ABANDON() do { if (s_src == SETTINGS_REC_UAC) uac_mic_close(); } while (0)

    if (!pick_path(storage_mount_path(vol))) {
        ABANDON();
        REFUSE(N_("Could not make the Recordings folder."));
    }

    s_file = storage_io_open(s_path, "wb");
    if (!s_file) {
        ABANDON();
        ESP_LOGW(TAG, "could not create %s", s_path);        /* 6022 */
        REFUSE(N_("Could not create the recording file."));
    }

    s_frames = 0; s_bytes = 0; s_dropped_frames = 0;
    portENTER_CRITICAL(&s_mux);                         /* 5214 */
    levelhist_reset(&s_hist);
    s_hist_frames = 0;
    s_zero_frames = 0;
    portEXIT_CRITICAL(&s_mux);
    s_stop = false; s_in_done = false; s_write_failed = false;
    if (s_use_beam) beam_init(s_beam);
    s_enc = flacenc_open((s_use_beam || s_fold) ? 1 : s_in_ch, s_bits,
                         s_rate, REC_BLOCK, file_write, NULL);
    if (!s_enc) {
        storage_io_close(s_file);
        s_file = NULL;
        remove(s_path);
        ABANDON();
        REFUSE(N_("Could not start the file."));
    }

    if (s_src != SETTINGS_REC_UAC) {
        const esp_err_t err = audio_out_capture_begin(s_src == SETTINGS_REC_HEADSET
                                                      ? AUDIO_CAPTURE_HEADSET
                                                      : AUDIO_CAPTURE_BUILTIN);
        if (err != ESP_OK) {
            flacenc_close(s_enc, NULL);
            s_enc = NULL;
            storage_io_close(s_file);
            s_file = NULL;
            remove(s_path);
            ESP_LOGW(TAG, "microphones did not start: %s", esp_err_to_name(err));
            REFUSE(N_("The microphones did not start."));
        }
    }

    xStreamBufferReset(s_ring);
    s_active = true;
    if (!tasks_ready()) {                               /* 6010 */
        if (s_src != SETTINGS_REC_UAC) audio_out_capture_end();
        ABANDON();
        flacenc_close(s_enc, NULL);
        s_enc = NULL;
        storage_io_close(s_file);
        s_file = NULL;
        remove(s_path);
        s_active = false;
        REFUSE(N_("No memory for the recording task."));
    }
    xTaskNotifyGive(s_enc_task);                        /* 6010: both exist now */
    xTaskNotifyGive(s_in_task);
    ESP_LOGI(TAG, "recording to %s (%s, %" PRIu32 " Hz, %u-bit%s%s)%s", s_path,
             s_src == SETTINGS_REC_UAC ? (s_auto ? "auto: USB microphone" : "USB microphone") :
             s_src == SETTINGS_REC_HEADSET ? (s_auto ? "auto: headset microphone, mono"
                                                    : "headset microphone, mono") :
             s_use_beam ? "focused: beam, mono" :
             s_fold ? (s_auto ? "auto: built-in pair, mono" : "built-in pair, mono") :
             "built-in pair, stereo",
             s_rate, s_bits,
             s_src == SETTINGS_REC_UAC ? (s_in_ch == 2 ? ", stereo" : ", mono") : "",
             (s_fold && s_src == SETTINGS_REC_UAC) ? ", folded to mono" : "",
             settings_time_verified() ? "" : "; the time is a guess until NTP, "
                                             "the name follows it if it moves");
    /* 5114: remembered for repair while the time is unverified. */
    if (!settings_time_verified() && s_fix_n < REC_FIX_MAX) {
        rec_fix_t *f = &s_fix[s_fix_n++];
        snprintf(f->path, sizeof(f->path), "%s", s_path);
        f->epoch = s_start_epoch;
        f->offset = s_start_offset;
    }
    heapmap_log("recording started");
    return true;
#undef ABANDON
#undef REFUSE
}

void recorder_stop(void)
{
    if (s_active && !s_stop) {
        ESP_LOGI(TAG, "stop asked");
        s_stop = true;
    }
}

bool recorder_active(void) { return s_active; }

bool recorder_can_start(void)
{
    return storage_present(STORAGE_SD) || storage_present(STORAGE_USB);
}

void recorder_status(recorder_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->active = s_active;
    out->stopping = s_active && s_stop;
    portENTER_CRITICAL(&s_mux);
    out->seconds = (uint32_t)(s_frames / s_rate);
    out->bytes = s_bytes;
    out->dropped_ms = (uint32_t)(s_dropped_frames * 1000 / s_rate);
    out->silent = s_zero_frames >= s_rate * REC_SILENT_MS / 1000u;     /* 5214 */
    portEXIT_CRITICAL(&s_mux);
    snprintf(out->name, sizeof(out->name), "%s", s_name);
}

bool recorder_level_strip(uint8_t *out)
{
    if (!out || !s_active) return false;
    portENTER_CRITICAL(&s_mux);
    levelhist_read(&s_hist, out);
    portEXIT_CRITICAL(&s_mux);
    return true;
}

/*
 * 5114: rename what the clock has moved under. ui_task, from
 * recorder_take_notice(). The recording in progress waits for its close.
 */
static void fix_names(void)
{
    if (!s_fix_n) return;
    const int64_t off = settings_clock_offset();
    int keep = 0;
    for (int i = 0; i < s_fix_n; i++) {
        rec_fix_t *f = &s_fix[i];
        const bool open = s_active && strcmp(f->path, s_path) == 0;
        if (f->offset != off && !open) {
            const int64_t d = off - f->offset;
            char dir[160], name[40], to[160];
            snprintf(dir, sizeof(dir), "%s", f->path);
            char *slash = strrchr(dir, '/');
            if (!slash) continue;               /* not ours; dropped */
            *slash = '\0';
            const char *old_name = slash + 1;
            struct stat st;
            storage_io_acquire(STORAGE_IO_BACKGROUND);
            bool ok = stat(f->path, &st) == 0 &&
                      name_for(dir, f->epoch + d, name, sizeof(name), to, sizeof(to)) &&
                      rename(f->path, to) == 0;
            if (ok) {
                /* The file date by the same amount, and the sidecar --
                 * named for the old name, and keyed on the old date --
                 * removed; the player rebuilds it at the next play. */
                const struct utimbuf ut = { .actime = st.st_mtime + d,
                                            .modtime = st.st_mtime + d };
                utime(to, &ut);
                char side[200];
                snprintf(side, sizeof(side), "%s/.%s.rgcache", dir, old_name);
                remove(side);
            }
            storage_io_release();
            if (!ok) {
                ESP_LOGW(TAG, "could not rename %s after the clock moved %+lld s "
                              "(gone, or the volume is out)", f->path, (long long)d);
                continue;                       /* dropped */
            }
            ESP_LOGI(TAG, "clock moved %+lld s: %s -> %s", (long long)d, old_name, name);
            files_changed();                    /* 5215 */
            snprintf(f->path, sizeof(f->path), "%s", to);
            f->epoch += d;
            f->offset = off;
        }
        /* Kept while the time is still a guess, or while unrepaired. */
        if (!settings_time_verified() || f->offset != off || open) s_fix[keep++] = *f;
    }
    s_fix_n = keep;
}

/*
 * 5115: recordings from earlier boots named in the future.
 *
 * fix_names() can only repair this boot's recordings: it knows how far
 * each was named wrong. An earlier boot that carried a wrong stored time
 * (2028-12-02, from a stray card file, until NTP) named its recordings
 * wrong by the correction NTP later made plus the device's off-time since
 * -- and settings_known_clock_error() keeps that correction. So once the
 * time is verified, each volume's Recordings folder is read, and every
 * recording named more than an hour in the future is brought back by it,
 * its file date with it, its sidecar dropped. Anything the correction
 * would still leave in the future is left alone and logged.
 *
 * And the player's own root files, which cardtime.c skips but a computer
 * shows, get today's date if theirs is in the future.
 *
 * Once per volume per mount, after NTP. ui_task.
 */
#define SWEEP_MAX           (64)
#define FUTURE_SLACK_S      (3600)

/* "2028-12-02 03.20.47.flac" or "... (2).flac" to an epoch, 0 if not. */
static int64_t name_epoch(const char *n)
{
    int y, mo, d, h, mi, se;
    if (sscanf(n, "%4d-%2d-%2d %2d.%2d.%2d", &y, &mo, &d, &h, &mi, &se) != 6) return 0;
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || se > 60) return 0;
    /* days_from_civil (H. Hinnant) */
    const int yy = y - (mo <= 2);
    const int era = (yy >= 0 ? yy : yy - 399) / 400;
    const unsigned yoe = (unsigned)(yy - era * 400);
    const unsigned doy = (unsigned)((153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const int64_t days = (int64_t)era * 146097 + (int64_t)doe - 719468;
    return days * 86400 + h * 3600 + mi * 60 + se;
}

static void sweep_volume(storage_id_t vol)
{
    const char *mount = storage_mount_path(vol);
    const int64_t now = settings_now();
    const int64_t fix = settings_known_clock_error();
    char dir[96];
    if (!storage_join_path(dir, sizeof(dir), mount, REC_DIR)) return;

    /* Names first, then renames: not while the directory is open. */
    char (*names)[40] = heap_caps_malloc(SWEEP_MAX * 40, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!names) return;
    int n = 0;
    storage_io_acquire(STORAGE_IO_BACKGROUND);
    DIR *d = opendir(dir);
    if (d) {
        const struct dirent *e;
        while ((e = readdir(d)) != NULL && n < SWEEP_MAX) {
            if (e->d_name[0] == '.' || strlen(e->d_name) >= 40) continue;
            const int64_t t = name_epoch(e->d_name);
            if (t > now + FUTURE_SLACK_S) snprintf(names[n++], 40, "%s", e->d_name);
        }
        closedir(d);
    }
    storage_io_release();

    int moved = 0, left = 0;
    for (int i = 0; i < n; i++) {
        char from[160], to[160], name[40];
        if (!storage_join_path(from, sizeof(from), dir, names[i])) continue;
        if (s_active && strcmp(from, s_path) == 0) continue;
        const int64_t t = name_epoch(names[i]);
        if (!fix || t + fix > now + FUTURE_SLACK_S) {
            if (left++ < 4) ESP_LOGW(TAG, "%s/%s is named in the future and no known "
                                          "correction brings it back; left as it is",
                                     dir, names[i]);
            continue;
        }
        struct stat st;
        storage_io_acquire(STORAGE_IO_BACKGROUND);
        bool ok = stat(from, &st) == 0 &&
                  name_for(dir, t + fix, name, sizeof(name), to, sizeof(to)) &&
                  rename(from, to) == 0;
        if (ok) {
            const time_t mt = (int64_t)st.st_mtime > now + FUTURE_SLACK_S
                            ? st.st_mtime + fix : st.st_mtime;
            const struct utimbuf ut = { .actime = mt, .modtime = mt };
            utime(to, &ut);
            char side[200];
            snprintf(side, sizeof(side), "%s/.%s.rgcache", dir, names[i]);
            remove(side);
        }
        storage_io_release();
        if (ok) {
            moved++;
            files_changed();                    /* 5215 */
            ESP_LOGI(TAG, "named in the future: %s -> %s (%+lld s, NTP's correction)",
                     names[i], name, (long long)fix);
        }
    }
    free(names);

    /* The player's own root files and the folder itself. */
    static const char *const own[] = { "stations.m3u", "favorites.m3u", "starred.m3u", REC_DIR };
    int touched = 0;
    for (size_t i = 0; i < sizeof(own) / sizeof(own[0]); i++) {
        char p[128];
        struct stat st;
        if (!storage_join_path(p, sizeof(p), mount, own[i])) continue;
        storage_io_acquire(STORAGE_IO_BACKGROUND);
        if (stat(p, &st) == 0 && (int64_t)st.st_mtime > now + FUTURE_SLACK_S) {
            const struct utimbuf ut = { .actime = (time_t)now, .modtime = (time_t)now };
            if (utime(p, &ut) == 0) touched++;
        }
        storage_io_release();
    }
    if (moved || left || touched) {
        ESP_LOGI(TAG, "%s: %d recording(s) brought back from the future, %d left, "
                      "%d of the player's own files re-dated", mount, moved, left, touched);
    }
}

static void sweep(void)
{
    static uint32_t seen_gen;
    static bool seen_verified;
    if (!settings_time_verified()) return;
    const uint32_t gen = storage_generation();
    if (seen_verified && gen == seen_gen) return;
    seen_verified = true;
    seen_gen = gen;
    for (int v = 0; v < STORAGE_COUNT; v++) {
        if (storage_present((storage_id_t)v)) sweep_volume((storage_id_t)v);
    }
}

bool recorder_take_notice(char *head, size_t head_len, char *body, size_t body_len)
{
    if (s_log_end) {
        s_log_end = false;
        heapmap_log("recording stopped");
    }
    fix_names();
    sweep();                            /* 5115 */
    if (!s_text_lock || !s_notice) return false;
    xSemaphoreTake(s_text_lock, portMAX_DELAY);
    const bool had = s_notice;
    if (had) {
        snprintf(head, head_len, "%s", s_notice_head);
        snprintf(body, body_len, "%s", s_notice_body);
        s_notice = false;
    }
    xSemaphoreGive(s_text_lock);
    return had;
}
