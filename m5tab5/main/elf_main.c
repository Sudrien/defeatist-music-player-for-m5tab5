/* SPDX-License-Identifier: MIT
 *
 * The ELF's app_main(). esp_elf calls it the way IDF calls app_main(), in a
 * box: by Avoidant's umode branch, or by this firmware's standalone.c.
 *
 * u-defeatist-0022: boxed/'s boxed_main.c, which was the boxed ELF's
 * app_main() in place of the trusted ELF's; u-defeatist-0028 made the box's
 * build the only one, and this the only app_main() the ELF has. The trusted
 * one required every ABI section the player calls through, so a launcher
 * that lacked one refused the ELF by name. A box has the core and
 * the storage section and none of the others; the calls of those this
 * player makes are absent.c's, in the app, and fail the way the player
 * already handles. feckless_abi_require_box() ties the ELF to the box: a
 * launcher that would run it trusted has no feckless_abi_box_v1 and refuses
 * it.
 *
 * u-defeatist-0024: the NVS section too, which a box answers since
 * u-feckless-0053.
 *
 * u-defeatist-0023: with DEFEATIST_BOXED_CODEC (the default), the audio
 * section too: esp_audio_codec is answered by the box where the launcher
 * links it.
 */
#include "defeatist.h"

#include "feckless_abi.h"
#include "feckless_abi_storage.h"
#include "feckless_abi_nvs.h"
#include "feckless_box.h"
#if DEFEATIST_BOXED_CODEC
#include "feckless_abi_audio.h"
#endif

void app_main(void)
{
    feckless_abi_require();
    feckless_abi_require_storage();
    feckless_abi_require_box();
    /* u-defeatist-0024: NVS through the box (u-feckless-0053), in the
     * namespace the launcher names; a box without it refuses the ELF. */
    feckless_abi_require_nvs();
#if DEFEATIST_BOXED_CODEC
    /* u-defeatist-0023: esp_audio_codec through the box (u-feckless-0052);
     * a launcher without the codec refuses the ELF by this name. */
    feckless_abi_require_audio();
#endif
    defeatist_main();
}
