/*
 * uac.h -- USB Audio Class output.
 *
 * A speaker on the USB-A port. Output only: the RX (microphone) half of
 * the class is deliberately not opened, because this program has nothing
 * to do with a microphone and an open RX interface costs isochronous
 * bandwidth and a ring buffer for a stream nobody reads. (5207: except
 * for the length of a recording -- see uac_mic_open().)
 *
 * The unit here is the interface, not the device. A headset enumerates
 * as two logical UAC devices -- one Audio Streaming interface each --
 * and the driver's connect callback fires once per interface. So
 * "a UAC device is attached" means "a TX interface has been opened",
 * which is the only sense in which it matters here.
 *
 * Nothing in this file resamples. If the device cannot take the format
 * the decoder is producing, uac_stream_start() says so and the caller
 * routes elsewhere -- see audio_out.c. Playing a 44.1 kHz file into a
 * 48 kHz-only device by handing it the bytes anyway is a semitone flat
 * and sounds like a broken player rather than like an unsupported rate.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Register the UAC class driver with usbhost.c.
 *
 * Must run before usbhost_start(), like every other class driver: one
 * registered afterwards is refused rather than installed late, because
 * it would never be offered a device that was already attached.
 */
esp_err_t uac_init(void);

/* A TX (speaker) interface is open. Not the same as streaming: a device
 * can be attached and idle, which is what it is between tracks. */
bool uac_present(void);

/* Bumped on every attach and every detach. Something that wants to
 * notice a headset appearing mid-track watches this rather than polling
 * uac_present(), so it can tell "still absent" from "gone and come
 * back". */
uint32_t uac_generation(void);

/*
 * Start (or restart) the stream in this format.
 *
 * 16-bit only, and the channel count is whatever the caller is handing
 * over -- no downmix here, for the same reason there is no resampler.
 *
 * Returns:
 *   ESP_OK                  streaming; uac_write() may be called
 *   ESP_ERR_NOT_FOUND       no device attached
 *   ESP_ERR_NOT_SUPPORTED   attached, but offers no alternate setting
 *                           matching this rate and channel count
 *   anything else           the driver's error from open or start
 *
 * Idempotent for a format already streaming, so the caller can ask on
 * every track without checking first.
 */
esp_err_t uac_stream_start(uint32_t rate, uint8_t channels);

/*
 * The rate this device would rather be sent, for audio at `rate`.
 *
 * `rate` itself when a 16-bit alternate with `channels` offers it.
 * Otherwise the lowest offered rate above it -- a 22 kHz file goes to
 * 44.1, not to 96 -- and failing that the highest below it, so a 96 kHz
 * file goes to 48. Zero when no device is attached or nothing at this
 * channel count and depth is offered at all. Asks the descriptors only;
 * nothing is started. See audio_out.c for the conversion this chooses.
 */
uint32_t uac_nearest_rate(uint32_t rate, uint8_t channels);

/* Stop streaming; the interface stays open. Safe when not streaming. */
void uac_stream_stop(void);

bool uac_streaming(void);

/*
 * Hand over interleaved 16-bit PCM in the format uac_stream_start()
 * accepted.
 *
 * Blocks up to timeout_ms for room in the driver's ring. A timeout is
 * ESP_ERR_TIMEOUT and the bytes are dropped rather than partially
 * written -- the driver's write is all-or-nothing and there is no
 * partial-write count to resume from.
 *
 * ESP_ERR_INVALID_STATE means the device went away between the check
 * and the write, which is the normal way a headset is unplugged and is
 * the caller's cue to re-route rather than an error to report.
 */
esp_err_t uac_write(const void *data, size_t len, uint32_t timeout_ms);

/*
 * 0-100, matching the player's own scale.
 *
 * Non-blocking, and that is the whole point of it: the value is
 * published and the event task performs the USB control transfer. The
 * caller is the UI task, a volume drag emits one of these per poll, and
 * a control transfer means taking the lock the audio writer holds for
 * the length of a write. Doing that on the UI task stalls the loop that
 * dispatches every other button -- see the note on uac_task().
 *
 * Coalescing rather than queueing: only the latest value matters, so a
 * drag that outruns the event task loses intermediate positions and
 * lands on the right one.
 */
void uac_set_volume(uint8_t percent);

/*
 * Whether the device has a volume control the driver can reach.
 *
 * False until the first attempt has been made, and false forever on the
 * many class-compliant parts that have no feature unit -- the C-Media
 * ones in particular. The caller applies gain in software when this is
 * false rather than leaving the slider dead.
 *
 * Read per block rather than latched at route change, because the answer
 * arrives asynchronously: the route can be taken before the first
 * control transfer has been attempted.
 */
bool uac_has_volume_control(void);

/* The device's product string, or "" -- for the log and the format card.
 * Never NULL. */
const char *uac_product(void);

/*
 * 5207: a microphone, for the recorder.
 *
 * The RX half is still not opened at attach. What changed is that an RX
 * interface is remembered when it is announced, and opened only for the
 * length of a recording -- the cost the note at the top of this file
 * objects to is a ring and isochronous bandwidth for a stream nobody
 * reads, and during a recording somebody does.
 *
 * Remembered is not the same as present. The driver says nothing when an
 * interface nobody opened goes away, so a microphone unplugged before it
 * was ever used is still "announced" until the open fails -- which it
 * does, cleanly, and uac_mic_open() forgets it then. A speaker detach
 * from the same address forgets it too, which covers a headset.
 */
bool uac_mic_announced(void);

/*
 * Open and start the remembered microphone. 16-bit PCM only, the same
 * limit as output; one or two channels, whichever the device offers
 * (a device offering both gives two). The rate is 48 kHz if offered,
 * then 44.1, then the highest offered -- the device's own rate, with no
 * conversion: a recording is written at what it was captured at.
 *
 * On success *rate and *channels say what was started. The product
 * string is uac_mic_product() until uac_mic_close().
 *
 *   ESP_ERR_NOT_FOUND       nothing announced, or it is gone (forgotten)
 *   ESP_ERR_NOT_SUPPORTED   no 16-bit PCM alternate with 1 or 2 channels
 *   ESP_ERR_INVALID_STATE   already open
 */
esp_err_t uac_mic_open(uint32_t *rate, uint8_t *channels);

/*
 * Up to max_frames interleaved frames, each sample a 16-bit value sign-
 * extended in int32, in the format uac_mic_open() reported. Waits up to
 * timeout_ms; 0 on a timeout. Whole frames only: a partial frame the
 * driver hands back is carried to the next call.
 */
size_t uac_mic_read(int32_t *frames, size_t max_frames, uint32_t timeout_ms);

/* The open microphone was unplugged. Sticky until uac_mic_close(). */
bool uac_mic_gone(void);

/* Stop and close. Safe when not open. */
void uac_mic_close(void);

/* The open microphone's product string, or "". Never NULL. */
const char *uac_mic_product(void);

#ifdef __cplusplus
}
#endif
