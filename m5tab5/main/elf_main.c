/* SPDX-License-Identifier: MIT
 *
 * 8087: the ELF's app_main(). The player is linked into an ELF for esp_elf
 * (../elf/) and run by whatever launcher is there -- Avoidant, or this
 * firmware's own standalone.c -- through the feckless ABI. esp_elf calls
 * this the way IDF calls app_main(); the launcher has brought up what
 * defeatist.h says. The feckless_abi_require*() calls do nothing at run
 * time: each refers to the version marker of the ABI section the player
 * calls through, so a host that does not have that section at that
 * version cannot resolve it, and esp_elf refuses to load the ELF with the
 * name of the one that is missing -- instead of a launcher that is too old
 * failing at the first call. The build checks the same (abicheck, run when
 * elf/ links).
 */
#include "defeatist.h"

#include "feckless_abi.h"
#include "feckless_abi_crypto.h"
#include "feckless_abi_image.h"
#include "feckless_abi_network.h"
#include "feckless_abi_nvs.h"
#include "feckless_abi_storage.h"

void app_main(void)
{
    feckless_abi_require();
    feckless_abi_require_storage();
    feckless_abi_require_nvs();
    feckless_abi_require_network();
    feckless_abi_require_image();
    feckless_abi_require_crypto();
    defeatist_main();
}
