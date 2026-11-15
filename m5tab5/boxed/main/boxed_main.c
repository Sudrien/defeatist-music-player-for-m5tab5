/* SPDX-License-Identifier: MIT
 *
 * u-defeatist-0022: the boxed ELF's app_main(), in place of elf_main.c's.
 *
 * elf_main.c requires every ABI section the player calls through, so a
 * launcher that lacks one refuses the ELF by name. A box has the core and
 * the storage section and none of the others; the calls of those this
 * player makes are absent.c's, in the app, and fail the way the player
 * already handles. feckless_abi_require_box() ties the ELF to the box: a
 * launcher that would run it trusted has no feckless_abi_box_v1 and refuses
 * it.
 */
#include "defeatist.h"

#include "feckless_abi.h"
#include "feckless_abi_storage.h"
#include "feckless_box.h"

void app_main(void)
{
    feckless_abi_require();
    feckless_abi_require_storage();
    feckless_abi_require_box();
    defeatist_main();
}
