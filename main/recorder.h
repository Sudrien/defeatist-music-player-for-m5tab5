/*
 * recorder -- the record button: the two built-in microphones to a FLAC
 * file on the card (5106).
 *
 * 48 kHz, 24-bit. By default the beam (5109, beam.h): mono, aimed
 * straight out of the screen. With the AUDIO tab's Microphones switch
 * on STEREO, both microphones as the ES7210 delivers them, MIC1 left.
 * No gain beyond the ADC's own PGA either way. Files go to <volume>/Recordings/, the SD card if one is
 * mounted and the USB drive otherwise, named for settings_now() in UTC
 * ("2026-09-26 18.04.33.flac") -- which before NTP is the build time or
 * the card's floor carried forward, so a best guess, as the README says.
 *
 * Two tasks for the length of a recording, created at start and gone at
 * the end: `rec_in` reads the I2S DMA into a 2 s PSRAM ring and never
 * touches the card; `rec_enc` drains the ring through flacenc.c into the
 * file. A card that stalls for a second costs ring, not samples; a ring
 * that fills is counted and logged as dropped audio, never silently.
 *
 * 5208: or from the headset's microphone (48 kHz, 16-bit, mono; 5206)
 * or a USB microphone (its own rate, 16-bit, its own channels, folded to
 * mono unless STEREO; 5207) -- the AUDIO tab's Record from switch,
 * settings_mic_input(), read at start. An input that is not there is a
 * refusal with the reason, not a silent file.
 *
 * Playback is held paused while this runs (the player's side of it):
 * the microphones take the I2S port away from the DAC. See
 * audio_out_capture_begin(). A USB microphone does not, and playback is
 * held paused for it anyway: one rule for "recording".
 *
 * All of it from ui_task except the tasks themselves.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool     active;        /* start() succeeded and the file is not yet closed */
    bool     stopping;      /* stop asked, the file is being finished */
    uint32_t seconds;       /* audio encoded so far */
    uint64_t bytes;         /* written to the file so far */
    uint32_t dropped_ms;    /* audio lost to a full ring, total */
    char     name[40];      /* the file's name, without the folder */
    /* 5214: the input has been exact digital zero for 300 ms -- muted
     * (a USB headset's mute sends zeros) or sending nothing. A live
     * microphone is never exactly zero, even in a quiet room. */
    bool     silent;
} recorder_status_t;

/*
 * Opens the file, writes the FLAC header, and starts both tasks. False,
 * with a reason for the notice card in why (may be NULL), when there is
 * no volume, the folder or file cannot be made, or the microphones
 * cannot be brought up.
 */
bool recorder_start(char *why, size_t why_len);

/* Asks for the end. The file is finished and closed on rec_enc; active
 * stays true until it is. */
void recorder_stop(void);

bool recorder_active(void);

/*
 * Is there anywhere to record to? The same volume check recorder_start()
 * opens with, and nothing more -- no allocation, no file. For the switch,
 * which marks the record detent unavailable and refuses the slide up
 * front rather than after the countdown. recorder_start() still decides.
 */
bool recorder_can_start(void);
void recorder_status(recorder_status_t *out);

/*
 * 5214: the last minute of input peak, oldest first, in levelhist.h's
 * units, for the level strip -- out[LEVELHIST_COLUMNS]. False when not
 * recording. A copy, under the lock rec_in pushes with.
 */
bool recorder_level_strip(uint8_t *out);

/*
 * 5215: a count that moves whenever a file in a Recordings folder is
 * finished or renamed -- the rename when the clock moves (5114, 5115)
 * is the one that left a stale name on screen, which played nothing
 * when tapped. The chooser compares it, as it does storage_generation().
 */
uint32_t recorder_files_changed(void);

/*
 * A card for the panel, once: why a recording ended by itself (the card
 * filled or went away, the 4 GB cap), or where a finished one went.
 * False when there is nothing new.
 */
bool recorder_take_notice(char *head, size_t head_len, char *body, size_t body_len);

#ifdef __cplusplus
}
#endif
