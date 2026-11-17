/* SPDX-License-Identifier: MIT
 *
 * u-defeatist-0023: esp_audio_codec as absent, for a boxed ELF built with
 * DEFEATIST_BOXED_CODEC off -- for a launcher that does not link the codec,
 * and so cannot answer it in a box (u-feckless-0052). No FLAC, AAC,
 * Vorbis, Opus or ALAC: the open fails and the player says the format is
 * not supported. MP3, MP2, WAV, AIFF, APE and DSD are its own decoders, in
 * the app, and play. Moved here from absent.c (u-defeatist-0022)
 * unchanged.
 */
#include "esp_audio_dec_default.h"
#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"

/* ---- esp_audio_codec: none ------------------------------------------------ */

esp_audio_err_t esp_audio_dec_register_default(void) { return ESP_AUDIO_ERR_NOT_SUPPORT; }
esp_audio_err_t esp_audio_simple_dec_register_default(void) { return ESP_AUDIO_ERR_NOT_SUPPORT; }
esp_audio_err_t esp_audio_simple_dec_open(esp_audio_simple_dec_cfg_t *cfg, esp_audio_simple_dec_handle_t *dec)
{
    (void)cfg;
    if (dec) *dec = NULL;
    return ESP_AUDIO_ERR_NOT_SUPPORT;
}
esp_audio_err_t esp_audio_simple_dec_get_info(esp_audio_simple_dec_handle_t dec, esp_audio_simple_dec_info_t *info)
{
    (void)dec; (void)info;
    return ESP_AUDIO_ERR_NOT_SUPPORT;
}
esp_audio_err_t esp_audio_simple_dec_process(esp_audio_simple_dec_handle_t dec, esp_audio_simple_dec_raw_t *raw,
                                             esp_audio_simple_dec_out_t *frame)
{
    (void)dec; (void)raw; (void)frame;
    return ESP_AUDIO_ERR_NOT_SUPPORT;
}
void esp_audio_simple_dec_close(esp_audio_simple_dec_handle_t dec) { (void)dec; }
